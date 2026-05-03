#include "KOReaderSyncClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <base64.h>

#include <ctime>

#include "KOReaderCredentialStore.h"

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";
constexpr uint16_t HTTP_CONNECT_TIMEOUT_MS = 5000;
constexpr uint16_t HTTP_IO_TIMEOUT_MS = 7000;

struct ParsedUrl {
  bool https = false;
  std::string host;
  uint16_t port = 80;
  std::string path;
};

bool parseUrl(const std::string& url, ParsedUrl& out) {
  const bool https = url.rfind("https://", 0) == 0;
  const bool http = url.rfind("http://", 0) == 0;
  if (!https && !http) return false;

  const size_t schemeLen = https ? 8 : 7;
  const size_t slashPos = url.find('/', schemeLen);
  const std::string hostPort = (slashPos == std::string::npos) ? url.substr(schemeLen) : url.substr(schemeLen, slashPos - schemeLen);
  out.path = (slashPos == std::string::npos) ? "/" : url.substr(slashPos);
  out.https = https;
  out.port = https ? 443 : 80;

  const size_t colonPos = hostPort.find(':');
  if (colonPos == std::string::npos) {
    out.host = hostPort;
    return !out.host.empty();
  }

  out.host = hostPort.substr(0, colonPos);
  const String portStr = hostPort.substr(colonPos + 1).c_str();
  const long parsedPort = portStr.toInt();
  if (parsedPort <= 0 || parsedPort > 65535 || out.host.empty()) return false;
  out.port = static_cast<uint16_t>(parsedPort);
  return true;
}

bool readLineWithDeadline(Client& client, String& outLine, const uint32_t deadlineMs) {
  outLine = "";
  while (millis() < deadlineMs) {
    while (client.available() > 0) {
      const int c = client.read();
      if (c < 0) break;
      if (c == '\n') return true;
      if (c != '\r') outLine += static_cast<char>(c);
    }
    delay(1);
  }
  return false;
}

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

  ParsedUrl parsed;
  if (!parseUrl(url, parsed)) {
    LOG_ERR("KOSync", "Invalid progress URL: %s", url.c_str());
    return SERVER_ERROR;
  }

  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;
  Client* client = nullptr;

  if (parsed.https) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    secureClient->setTimeout(1);
    secureClient->setHandshakeTimeout((HTTP_CONNECT_TIMEOUT_MS + 999) / 1000);
    LOG_DBG("KOSync", "Progress fetch heap after secure client init: %u", ESP.getFreeHeap());
    client = secureClient.get();
  } else {
    plainClient.setTimeout(1);
    client = &plainClient;
  }
  LOG_DBG("KOSync", "Progress fetch manual timeouts set: connect=%u ms io=%u ms", HTTP_CONNECT_TIMEOUT_MS,
          HTTP_IO_TIMEOUT_MS);
  LOG_DBG("KOSync", "Progress fetch heap before connect: %u", ESP.getFreeHeap());

  const uint32_t startedAtMs = millis();
  const uint32_t deadlineMs = startedAtMs + HTTP_IO_TIMEOUT_MS;

  if (!client->connect(parsed.host.c_str(), parsed.port, HTTP_CONNECT_TIMEOUT_MS)) {
    LOG_ERR("KOSync", "Progress fetch connect failed to %s:%u", parsed.host.c_str(), parsed.port);
    client->stop();
    LOG_DBG("KOSync", "Progress fetch resources closed: yes heap_after=%u", ESP.getFreeHeap());
    return NETWORK_ERROR;
  }
  LOG_DBG("KOSync", "Progress fetch connection established");

  String request;
  request.reserve(384);
  request += "GET ";
  request += parsed.path.c_str();
  request += " HTTP/1.1\r\nHost: ";
  request += parsed.host.c_str();
  request += "\r\nAccept: application/vnd.koreader.v1+json\r\nx-auth-user: ";
  request += KOREADER_STORE.getUsername().c_str();
  request += "\r\nx-auth-key: ";
  request += KOREADER_STORE.getMd5Password().c_str();
  request += "\r\nAuthorization: Basic ";
  request += base64::encode(String(KOREADER_STORE.getUsername().c_str()) + ":" + KOREADER_STORE.getPassword().c_str());
  request += "\r\nConnection: close\r\n\r\n";
  client->print(request);

  bool gotFirstByte = false;
  while (!gotFirstByte && millis() < deadlineMs) {
    if (client->available() > 0) {
      gotFirstByte = true;
      LOG_DBG("KOSync", "Progress fetch first byte received at +%lu ms", millis() - startedAtMs);
      break;
    }
    delay(1);
  }
  if (!gotFirstByte) {
    LOG_ERR("KOSync", "Progress fetch timeout before first byte; manual abort at %lu ms", millis() - startedAtMs);
    client->stop();
    LOG_DBG("KOSync", "Progress fetch resources closed: yes heap_after=%u", ESP.getFreeHeap());
    return NETWORK_ERROR;
  }

  String statusLine;
  if (!readLineWithDeadline(*client, statusLine, deadlineMs)) {
    LOG_ERR("KOSync", "Progress fetch timeout reading status line; manual abort");
    client->stop();
    return NETWORK_ERROR;
  }
  int httpCode = -1;
  if (statusLine.startsWith("HTTP/1.1 ") || statusLine.startsWith("HTTP/1.0 ")) {
    httpCode = statusLine.substring(9, 12).toInt();
  }

  int contentLength = -1;
  while (true) {
    String headerLine;
    if (!readLineWithDeadline(*client, headerLine, deadlineMs)) {
      LOG_ERR("KOSync", "Progress fetch timeout while reading headers; manual abort");
      client->stop();
      return NETWORK_ERROR;
    }
    if (headerLine.length() == 0) break;
    if (headerLine.startsWith("Content-Length:")) {
      contentLength = headerLine.substring(15).toInt();
    }
  }

  const uint32_t endedAtMs = millis();
  LOG_DBG("KOSync", "Progress fetch end: t=%lu elapsed=%lu code=%d", endedAtMs, endedAtMs - startedAtMs, httpCode);
  LOG_DBG("KOSync", "Progress fetch heap after headers: %u", ESP.getFreeHeap());

  if (httpCode == 200) {
    LOG_DBG("KOSync", "Progress response declared length: %d", contentLength);
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, *client);
    LOG_DBG("KOSync", "Progress response parse complete (streaming), heap_now=%u", ESP.getFreeHeap());

    client->stop();
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

  client->stop();
  LOG_DBG("KOSync", "Progress response declared length: %d", contentLength);
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
