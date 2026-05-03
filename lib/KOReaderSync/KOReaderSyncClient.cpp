#include "KOReaderSyncClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ctime>

#include "KOReaderCredentialStore.h"

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";
constexpr uint16_t HTTP_CONNECT_TIMEOUT_MS = 5000;
constexpr uint16_t HTTP_IO_TIMEOUT_MS = 7000;

void addAuthHeaders(HTTPClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password().c_str());

  // HTTP Basic Auth (RFC 7617) header. This is needed to support koreader sync server embedded in Calibre Web Automated
  // (https://github.com/crocodilestick/Calibre-Web-Automated/blob/main/cps/progress_syncing/protocols/kosync.py)
  http.setAuthorization(KOREADER_STORE.getUsername().c_str(), KOREADER_STORE.getPassword().c_str());
}

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s", url.c_str());

  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);

  const int httpCode = http.GET();
  http.end();

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode == 200) {
    return OK;
  } else if (httpCode == 401) {
    return AUTH_FAILED;
  } else if (httpCode < 0) {
    return NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s", url.c_str());
  LOG_DBG("KOSync", "Progress fetch start: t=%lu heap_before=%u", millis(), ESP.getFreeHeap());

  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    secureClient->setTimeout((HTTP_IO_TIMEOUT_MS + 999) / 1000);
    LOG_DBG("KOSync", "Progress fetch heap after secure client init: %u", ESP.getFreeHeap());
    LOG_DBG("KOSync", "Progress fetch secure client timeout set to %u sec", (HTTP_IO_TIMEOUT_MS + 999) / 1000);
    http.begin(*secureClient, url.c_str());
  } else {
    plainClient.setTimeout((HTTP_IO_TIMEOUT_MS + 999) / 1000);
    LOG_DBG("KOSync", "Progress fetch plain client timeout set to %u sec", (HTTP_IO_TIMEOUT_MS + 999) / 1000);
    http.begin(plainClient, url.c_str());
  }
  LOG_DBG("KOSync", "Progress fetch heap after http.begin: %u", ESP.getFreeHeap());
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_IO_TIMEOUT_MS);
  LOG_DBG("KOSync", "Progress fetch HTTP timeouts set: connect=%u ms io=%u ms", HTTP_CONNECT_TIMEOUT_MS,
          HTTP_IO_TIMEOUT_MS);
  addAuthHeaders(http);

  const uint32_t startedAtMs = millis();
  LOG_DBG("KOSync", "Progress fetch heap before GET: %u", ESP.getFreeHeap());
  const int httpCode = http.GET();
  const uint32_t endedAtMs = millis();
  LOG_DBG("KOSync", "Progress fetch end: t=%lu elapsed=%lu code=%d", endedAtMs, endedAtMs - startedAtMs, httpCode);
  LOG_DBG("KOSync", "Progress fetch heap after GET: %u", ESP.getFreeHeap());

  if (httpCode == 200) {
    const int responseLength = http.getSize();
    LOG_DBG("KOSync", "Progress response declared length: %d", responseLength);
    JsonDocument doc;
    WiFiClient* stream = http.getStreamPtr();
    const DeserializationError error = deserializeJson(doc, *stream);
    LOG_DBG("KOSync", "Progress response parse complete (streaming), heap_now=%u", ESP.getFreeHeap());

    http.end();
    LOG_DBG("KOSync", "Progress fetch resources closed: yes heap_after=%u", ESP.getFreeHeap());

    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      LOG_ERR("KOSync", "Progress fetch failure type: parse_error");
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

  const int responseLength = http.getSize();
  http.end();
  LOG_DBG("KOSync", "Progress response declared length: %d", responseLength);
  LOG_DBG("KOSync", "Progress fetch resources closed: yes heap_after=%u", ESP.getFreeHeap());

  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode == 401) {
    LOG_ERR("KOSync", "Progress fetch failure type: auth_error");
    return AUTH_FAILED;
  } else if (httpCode == 404) {
    LOG_DBG("KOSync", "Progress fetch failure type: not_found");
    return NOT_FOUND;
  } else if (httpCode < 0) {
    LOG_ERR("KOSync", "Progress fetch failure type: timeout_or_network_error");
    return NETWORK_ERROR;
  }
  LOG_ERR("KOSync", "Progress fetch failure type: server_error");
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s", url.c_str());

  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  // Build JSON body (timestamp not required per API spec)
  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

  const int httpCode = http.PUT(body.c_str());
  http.end();

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  if (httpCode == 200 || httpCode == 202) {
    return OK;
  } else if (httpCode == 401) {
    return AUTH_FAILED;
  } else if (httpCode < 0) {
    return NETWORK_ERROR;
  }
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
    default:
      return "Unknown error";
  }
}
