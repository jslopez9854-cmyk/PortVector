#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include <ctime>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

// Small TLS buffers to fit in ESP32-C3's limited heap (~46KB free after WiFi).
// KOSync payloads are tiny JSON (<1KB), so 2KB buffers are sufficient.
// Default 16KB buffers cause OOM during TLS handshake.
constexpr int HTTP_BUF_SIZE = 2048;

// Cloudflare tunnels send a 3-cert Google Trust Services chain. During the TLS handshake
// mbedTLS makes many small allocations that collectively consume ~48KB of heap. With only
// ~50KB free after WiFi connects, the session drove min-free-ever down to 2600 bytes before
// failing with MBEDTLS_ERR_X509_ALLOC_FAILED (-0x2880).
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;

// Aggregate free bytes alone isn't a complete picture: mbedTLS's SSL buffer grows on demand
// during handshake (CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH) to hold each certificate in the
// chain, which needs a single contiguous block, not just enough free bytes in aggregate. Belt
// and suspenders alongside MIN_HEAP_FOR_TLS.
constexpr size_t MIN_CONTIGUOUS_BLOCK_FOR_TLS = 20000;

bool hasHeapForTls() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  LOG_DBG("KOSync", "Heap check: %u free, %u largest contiguous block", (unsigned)freeHeap,
          (unsigned)largestBlock);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return false;
  }
  if (largestBlock < MIN_CONTIGUOUS_BLOCK_FOR_TLS) {
    LOG_ERR("KOSync", "Heap too fragmented for TLS handshake: largest block %u bytes (need %u)",
            (unsigned)largestBlock, (unsigned)MIN_CONTIGUOUS_BLOCK_FOR_TLS);
    return false;
  }
  return true;
}

// Response buffer for reading HTTP body
struct ResponseBuffer {
  char* data = nullptr;
  int len = 0;
  int capacity = 0;

  ~ResponseBuffer() { free(data); }

  bool ensure(int size) {
    if (size <= capacity) return true;
    char* newData = (char*)realloc(data, size);
    if (!newData) return false;
    data = newData;
    capacity = size;
    return true;
  }
};

// HTTP event handler to collect response body
esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
    if (buf->ensure(buf->len + evt->data_len + 1)) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
      buf->data[buf->len] = '\0';
    } else {
      LOG_ERR("KOSync", "Response buffer allocation failed (%d bytes)", evt->data_len);
    }
  }
  return ESP_OK;
}

// Create configured esp_http_client with small TLS buffers
esp_http_client_handle_t createClient(const char* url, ResponseBuffer* buf,
                                      esp_http_client_method_t method = HTTP_METHOD_GET) {
  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = httpEventHandler;
  config.user_data = buf;
  config.method = method;
  config.timeout_ms = 15000;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return nullptr;

  // KOSync auth headers
  if (esp_http_client_set_header(client, "Accept", "application/vnd.koreader.v1+json") != ESP_OK ||
      esp_http_client_set_header(client, "x-auth-user", KOREADER_STORE.getUsername().c_str()) != ESP_OK ||
      esp_http_client_set_header(client, "x-auth-key", KOREADER_STORE.getMd5Password().c_str()) != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set auth headers");
    esp_http_client_cleanup(client);
    return nullptr;
  }

  // HTTP Basic Auth for Calibre-Web-Automated compatibility
  std::string credentials = KOREADER_STORE.getUsername() + ":" + KOREADER_STORE.getPassword();
  String encoded = base64::encode(reinterpret_cast<const uint8_t*>(credentials.data()), credentials.size());
  std::string authHeader = "Basic " + std::string(encoded.c_str());
  if (esp_http_client_set_header(client, "Authorization", authHeader.c_str()) != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set Authorization header");
    esp_http_client_cleanup(client);
    return nullptr;
  }

  return client;
}

// esp_http_client_perform() result, with a couple of retries on transient network/TLS errors.
struct PerformResult {
  esp_err_t err;
  int httpCode;
};

constexpr int RETRY_DELAYS_MS[] = {200, 600};

PerformResult performWithRetry(esp_http_client_handle_t client) {
  esp_err_t err = esp_http_client_perform(client);
  int httpCode = esp_http_client_get_status_code(client);
  for (int delayMs : RETRY_DELAYS_MS) {
    if (err == ESP_OK) break;
    LOG_DBG("KOSync", "Request failed (err %d), retrying in %dms", err, delayMs);
    delay(delayMs);
    err = esp_http_client_perform(client);
    httpCode = esp_http_client_get_status_code(client);
  }
  return {err, httpCode};
}

// ESP-IDF's documented alternative to esp_http_client_set_post_field() + perform() for sending
// a request body: open()/write()/fetch_headers() avoids set_post_field()'s undocumented
// Content-Type override (see esp-idf#2092).
PerformResult openWriteFetchWithRetry(esp_http_client_handle_t client, const std::string& body) {
  auto attempt = [&]() -> PerformResult {
    esp_err_t err = esp_http_client_open(client, body.length());
    int httpCode = 0;
    if (err == ESP_OK) {
      if (esp_http_client_write(client, body.c_str(), body.length()) < 0) {
        err = ESP_FAIL;
      } else if (esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;
      } else {
        char discard[256];
        while (esp_http_client_read(client, discard, sizeof(discard)) > 0) {
        }
        httpCode = esp_http_client_get_status_code(client);
      }
    }
    esp_http_client_close(client);
    return {err, httpCode};
  };

  PerformResult result = attempt();
  for (int delayMs : RETRY_DELAYS_MS) {
    if (result.err == ESP_OK) break;
    LOG_DBG("KOSync", "Request failed (err %d), retrying in %dms", result.err, delayMs);
    delay(delayMs);
    result = attempt();
  }
  return result;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (!hasHeapForTls()) return LOW_MEMORY;

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) return NETWORK_ERROR;

  const auto [err, httpCode] = performWithRetry(client);
  lastHttpCode = httpCode;
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Auth response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (!hasHeapForTls()) return LOW_MEMORY;

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) return NETWORK_ERROR;

  const auto [err, httpCode] = performWithRetry(client);
  lastHttpCode = httpCode;
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Get progress response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;

  if (httpCode == 200 && buf.data) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, buf.data);

    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (!hasHeapForTls()) return LOW_MEMORY;

  // Build JSON body. Scoped so JsonDocument's own pool allocations are freed before opening
  // the connection — every prior test showed the TLS handshake succeeds with a zero-length
  // body and fails with this exact body, and this is the only remaining live heap allocation
  // unique to the failing path once Content-Type and body-length-alone were both ruled out.
  std::string body;
  {
    JsonDocument doc;
    doc["document"] = progress.document;
    doc["progress"] = progress.progress;
    doc["percentage"] = progress.percentage;
    doc["device"] = DEVICE_NAME;
    doc["device_id"] = DEVICE_ID;
    serializeJson(doc, body);
  }

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_PUT);
  if (!client) return NETWORK_ERROR;

  if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set request headers");
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  const auto [err, httpCode] = openWriteFetchWithRetry(client, body);
  lastHttpCode = httpCode;
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Update progress response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

const char* KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found";
    case LOW_MEMORY:
      return "Not enough memory for sync — please retry";
    default:
      return "Unknown error";
  }
}
