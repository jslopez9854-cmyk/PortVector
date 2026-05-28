#ifdef BLE_ENABLED

#include "BluetoothHIDManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

// Standard HID keycodes for page turning
// These cover most generic BLE keyboards and media remotes
static constexpr uint16_t HID_KEY_RIGHT       = 0x004F;
static constexpr uint16_t HID_KEY_LEFT        = 0x0050;
static constexpr uint16_t HID_KEY_PAGE_UP     = 0x004B;
static constexpr uint16_t HID_KEY_PAGE_DOWN   = 0x004E;
static constexpr uint16_t HID_CONSUMER_NEXT   = 0x00B5;
static constexpr uint16_t HID_CONSUMER_PREV   = 0x00B6;
static constexpr uint16_t HID_CONSUMER_FASTF  = 0x00B3;
static constexpr uint16_t HID_CONSUMER_REWIND = 0x00B4;

// Settings keys for persisting learned keycodes
static constexpr const char* SETTINGS_KEY_LEARNED_BACK    = "ble_key_back";
static constexpr const char* SETTINGS_KEY_LEARNED_FORWARD = "ble_key_fwd";

// Cooldown between page turns in ms — prevents double-fire
static constexpr unsigned long PAGE_TURN_COOLDOWN_MS = 300;

// ---------------------------------------------------------------------------
// NimBLE callback implementations
// ---------------------------------------------------------------------------

class BLEScanCallback : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice* device) override {
    std::string name = device->getName();
    std::string addr = device->getAddress().toString();
    if (name.empty()) name = addr;
    LOG_INF("BLE", "Found device: %s [%s]", name.c_str(), addr.c_str());
    BLE_HID.onDeviceFound(addr, name);
  }
};

class BLEClientCallback : public NimBLEClientCallbacks {
 public:
  void onConnect(NimBLEClient* client) override {
    LOG_INF("BLE", "Connected to %s", client->getPeerAddress().toString().c_str());
    BLE_HID.onConnect();
  }

  void onDisconnect(NimBLEClient* client, int reason) override {
    LOG_INF("BLE", "Disconnected (reason: %d)", reason);
    BLE_HID.onDisconnect();
  }
};

class BLEHIDReportCallback : public NimBLECharacteristicCallbacks {
 public:
  void onRead(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {}

  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {}

  void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo,
                   uint16_t subValue) override {}
};

// NimBLE 2.x uses a notification callback on the characteristic directly
static void hidNotifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData,
                              size_t length, bool isNotify) {
  BLE_HID.onHIDReport(pData, length);
}

// ---------------------------------------------------------------------------
// BluetoothHIDManager implementation
// ---------------------------------------------------------------------------

void BluetoothHIDManager::begin() {
  LOG_INF("BLE", "Initializing BLE HID manager");

  // CRITICAL: WiFi and BLE share one radio on ESP32-C3
  if (WiFi.getMode() != WIFI_OFF) {
    LOG_INF("BLE", "Disabling WiFi for BLE (mutual exclusion)");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  NimBLEDevice::init("CrossPoint");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // Max power for range

  loadLearnedKeys();
  LOG_INF("BLE", "BLE ready. Learned keys: back=0x%04X fwd=0x%04X",
          _learnedBack, _learnedForward);
}

void BluetoothHIDManager::update() {
  // Clear single-frame flags every loop tick
  _pageBackFlag = false;
  _pageForwardFlag = false;
}

void BluetoothHIDManager::startScan() {
  if (_scanning) return;

  _scanResults.clear();
  _scanning = true;

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(new BLEScanCallback(), false);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);

  // Scan for 10 seconds
  pScan->start(10, false);
  LOG_INF("BLE", "Scan started");
}

void BluetoothHIDManager::stopScan() {
  if (!_scanning) return;
  NimBLEDevice::getScan()->stop();
  _scanning = false;
  LOG_INF("BLE", "Scan stopped");
}

void BluetoothHIDManager::connectToDevice(const std::string& address) {
  if (_connected) disconnect();

  LOG_INF("BLE", "Connecting to %s", address.c_str());

  _client = NimBLEDevice::createClient();
  _client->setClientCallbacks(new BLEClientCallback(), false);
  _client->setConnectionParams(12, 12, 0, 51);

if (!_client->connect(NimBLEAddress(address, BLE_ADDR_PUBLIC))) {
      LOG_ERR("BLE", "Connection failed");
    NimBLEDevice::deleteClient(_client);
    _client = nullptr;
    return;
  }

  // Find the HID service
  NimBLERemoteService* pHIDService =
      _client->getService(NimBLEUUID("1812"));  // HID Service UUID
  if (!pHIDService) {
    LOG_ERR("BLE", "HID service not found");
    _client->disconnect();
    return;
  }

  // Subscribe to all Report characteristics
  auto characteristics = pHIDService->getCharacteristics(true);
  bool subscribed = false;
  for (auto& pChar : characteristics) {
    // Report characteristic UUID
    if (pChar->getUUID() == NimBLEUUID("2A4D")) {
      if (pChar->canNotify()) {
        pChar->subscribe(true, hidNotifyCallback);
        subscribed = true;
        LOG_INF("BLE", "Subscribed to HID report characteristic");
      }
    }
  }

  if (!subscribed) {
    LOG_ERR("BLE", "No notifiable HID report characteristics found");
    _client->disconnect();
    return;
  }

  // Store device name from scan results
  for (const auto& r : _scanResults) {
    if (r.address == address) {
      _connectedName = r.name;
      break;
    }
  }
  _connectedAddress = address;
}

void BluetoothHIDManager::disconnect() {
  if (_client) {
    _client->disconnect();
    NimBLEDevice::deleteClient(_client);
    _client = nullptr;
  }
  _connected = false;
  _connectedAddress.clear();
  _connectedName.clear();
  LOG_INF("BLE", "Disconnected");
}

void BluetoothHIDManager::startLearnMode() {
  _learnMode = true;
  _learnFirst = 0;
  _learnSecond = 0;
  _learnStep = 0;
  LOG_INF("BLE", "Learn Mode started — press PageBack button on remote");
}

void BluetoothHIDManager::cancelLearnMode() {
  _learnMode = false;
  _learnStep = 0;
  _learnFirst = 0;
  _learnSecond = 0;
  LOG_INF("BLE", "Learn Mode cancelled");
}

void BluetoothHIDManager::saveLearnedKeys() {
  // Persist to a simple file on SD
  // Format: two uint16_t values
  FsFile f;
  if (Storage.openFileForWrite("BLE", "/ble_keys.bin", f)) {
    uint8_t buf[4];
    buf[0] = _learnedBack & 0xFF;
    buf[1] = (_learnedBack >> 8) & 0xFF;
    buf[2] = _learnedForward & 0xFF;
    buf[3] = (_learnedForward >> 8) & 0xFF;
    f.write(buf, 4);
    f.close();
    LOG_INF("BLE", "Saved learned keys: back=0x%04X fwd=0x%04X",
            _learnedBack, _learnedForward);
  }
}

void BluetoothHIDManager::loadLearnedKeys() {
  FsFile f;
  if (Storage.openFileForRead("BLE", "/ble_keys.bin", f)) {
    uint8_t buf[4];
    if (f.read(buf, 4) == 4) {
      _learnedBack    = buf[0] | (buf[1] << 8);
      _learnedForward = buf[2] | (buf[3] << 8);
      LOG_INF("BLE", "Loaded learned keys: back=0x%04X fwd=0x%04X",
              _learnedBack, _learnedForward);
    }
    f.close();
  }
}

void BluetoothHIDManager::clearLearnedKeys() {
  _learnedBack = 0;
  _learnedForward = 0;
  Storage.remove("/ble_keys.bin");
  LOG_INF("BLE", "Cleared learned keys");
}

// ---------------------------------------------------------------------------
// Internal callbacks
// ---------------------------------------------------------------------------

void BluetoothHIDManager::onDeviceFound(const std::string& address,
                                         const std::string& name) {
  // Deduplicate
  for (const auto& r : _scanResults) {
    if (r.address == address) return;
  }
  _scanResults.push_back({address, name});
}

void BluetoothHIDManager::onConnect() {
  _connected = true;
  _scanning = false;
}

void BluetoothHIDManager::onDisconnect() {
  _connected = false;
  _client = nullptr;
  _connectedAddress.clear();
  _connectedName.clear();
}

void BluetoothHIDManager::onHIDReport(const uint8_t* data, size_t length) {
  if (length == 0) return;

  // Extract a keycode from the report — try common report layouts
  // Most BLE HID keyboards send: [modifier, reserved, key1, key2, ...]
  // Consumer reports send a 2-byte little-endian usage code
  uint16_t keycode = 0;

  if (length >= 3) {
    // Standard keyboard report: byte 2 is the first keycode
    keycode = data[2];
  } else if (length == 2) {
    // Consumer report: 2-byte little-endian
    keycode = data[0] | (data[1] << 8);
  } else if (length == 1) {
    keycode = data[0];
  }

  if (keycode == 0) return;

  static unsigned long lastPageTurn = 0;
  const unsigned long now = millis();

  // Learn Mode — capture two distinct keycodes
  if (_learnMode) {
    if (_learnStep == 0) {
      _learnFirst = keycode;
      _learnStep = 1;
      LOG_INF("BLE", "Learn: captured PageBack keycode 0x%04X — now press PageForward", keycode);
    } else if (_learnStep == 1 && keycode != _learnFirst) {
      _learnSecond = keycode;
      _learnStep = 2;
      _learnedBack = _learnFirst;
      _learnedForward = _learnSecond;
      _learnMode = false;
      saveLearnedKeys();
      LOG_INF("BLE", "Learn: captured PageForward keycode 0x%04X — done", keycode);
    }
    return;
  }

  // Cooldown check
  if ((now - lastPageTurn) < PAGE_TURN_COOLDOWN_MS) return;

  // Check learned keycodes first
  if (_learnedBack != 0 && keycode == _learnedBack) {
    _pageBackFlag = true;
    lastPageTurn = now;
    LOG_DBG("BLE", "PageBack (learned)");
    return;
  }
  if (_learnedForward != 0 && keycode == _learnedForward) {
    _pageForwardFlag = true;
    lastPageTurn = now;
    LOG_DBG("BLE", "PageForward (learned)");
    return;
  }

  // Fall back to standard keycodes
  switch (keycode) {
    case HID_KEY_PAGE_DOWN:
    case HID_KEY_RIGHT:
    case HID_CONSUMER_NEXT:
    case HID_CONSUMER_FASTF:
      _pageForwardFlag = true;
      lastPageTurn = now;
      LOG_DBG("BLE", "PageForward (standard 0x%04X)", keycode);
      break;

    case HID_KEY_PAGE_UP:
    case HID_KEY_LEFT:
    case HID_CONSUMER_PREV:
    case HID_CONSUMER_REWIND:
      _pageBackFlag = true;
      lastPageTurn = now;
      LOG_DBG("BLE", "PageBack (standard 0x%04X)", keycode);
      break;

    default:
      LOG_DBG("BLE", "Unrecognized keycode 0x%04X", keycode);
      break;
  }
}

#endif  // BLE_ENABLED