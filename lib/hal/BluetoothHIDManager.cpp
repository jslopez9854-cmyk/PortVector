#ifdef BLE_ENABLED

#include "BluetoothHIDManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

#if defined(ARDUINO) && __has_include(<esp32-hal-bt-mem.h>)
#include <esp32-hal-bt-mem.h>
#endif

static constexpr uint16_t HID_KEY_RIGHT       = 0x004F;
static constexpr uint16_t HID_KEY_LEFT        = 0x0050;
static constexpr uint16_t HID_KEY_PAGE_UP     = 0x004B;
static constexpr uint16_t HID_KEY_PAGE_DOWN   = 0x004E;
static constexpr uint16_t HID_CONSUMER_NEXT   = 0x00B5;
static constexpr uint16_t HID_CONSUMER_PREV   = 0x00B6;
static constexpr uint16_t HID_CONSUMER_FASTF  = 0x00B3;
static constexpr uint16_t HID_CONSUMER_REWIND = 0x00B4;

static constexpr unsigned long PAGE_TURN_COOLDOWN_MS = 300;

// ---------------------------------------------------------------------------
// NimBLE callbacks
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

static void hidNotifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData,
                              size_t length, bool isNotify) {
  BLE_HID.onHIDReport(pData, length);
}

// ---------------------------------------------------------------------------
// BluetoothHIDManager implementation
// ---------------------------------------------------------------------------

void BluetoothHIDManager::begin() {
  if (_initialized) {
    LOG_INF("BLE", "BLE already initialized, skipping");
    return;
  }
  _initialized = true;
  LOG_INF("BLE", "Initializing BLE HID client");

  if (WiFi.getMode() != WIFI_OFF) {
    LOG_INF("BLE", "Disabling WiFi for BLE (mutual exclusion)");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);
  }

  NimBLEDevice::init("CrossPoint");
  // Disable security so phone doesn't drop connection during pairing
  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_SM_IO_CAP_NO_IO);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  loadLearnedKeys();
  LOG_INF("BLE", "BLE ready. Learned keys: back=0x%04X fwd=0x%04X",
          _learnedBack, _learnedForward);
}

void BluetoothHIDManager::update() {
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

  if (!_client->connect(NimBLEAddress(address, BLE_ADDR_RANDOM))) {
    LOG_ERR("BLE", "Connection failed");
    NimBLEDevice::deleteClient(_client);
    _client = nullptr;
    return;
  }

  NimBLERemoteService* pHIDService = _client->getService(NimBLEUUID("1812"));
  if (!pHIDService) {
    LOG_ERR("BLE", "HID service not found");
    _client->disconnect();
    return;
  }

  auto characteristics = pHIDService->getCharacteristics(true);
  bool subscribed = false;
  for (auto& pChar : characteristics) {
    if (pChar->getUUID() == NimBLEUUID("2A4D")) {
      if (pChar->canNotify()) {
        pChar->subscribe(true, hidNotifyCallback);
        subscribed = true;
        LOG_INF("BLE", "Subscribed to HID report");
      }
    }
  }

  if (!subscribed) {
    LOG_ERR("BLE", "No notifiable HID characteristics found");
    _client->disconnect();
    return;
  }

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
  LOG_INF("BLE", "Learn Mode started");
}

void BluetoothHIDManager::cancelLearnMode() {
  _learnMode = false;
  _learnStep = 0;
  _learnFirst = 0;
  _learnSecond = 0;
  LOG_INF("BLE", "Learn Mode cancelled");
}

void BluetoothHIDManager::saveLearnedKeys() {
  FsFile f;
  if (Storage.openFileForWrite("BLE", "/ble_keys.bin", f)) {
    uint8_t buf[4];
    buf[0] = _learnedBack & 0xFF;
    buf[1] = (_learnedBack >> 8) & 0xFF;
    buf[2] = _learnedForward & 0xFF;
    buf[3] = (_learnedForward >> 8) & 0xFF;
    f.write(buf, 4);
    f.close();
  }
}

void BluetoothHIDManager::loadLearnedKeys() {
  FsFile f;
  if (Storage.openFileForRead("BLE", "/ble_keys.bin", f)) {
    uint8_t buf[4];
    if (f.read(buf, 4) == 4) {
      _learnedBack    = buf[0] | (buf[1] << 8);
      _learnedForward = buf[2] | (buf[3] << 8);
    }
    f.close();
  }
}

void BluetoothHIDManager::clearLearnedKeys() {
  _learnedBack = 0;
  _learnedForward = 0;
  Storage.remove("/ble_keys.bin");
}

void BluetoothHIDManager::onDeviceFound(const std::string& address,
                                         const std::string& name) {
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

  uint16_t keycode = 0;
  if (length >= 3) {
    keycode = data[2];
  } else if (length == 2) {
    keycode = data[0] | (data[1] << 8);
  } else if (length == 1) {
    keycode = data[0];
  }

  if (keycode == 0) return;

  static unsigned long lastPageTurn = 0;
  const unsigned long now = millis();

  if (_learnMode) {
    if (_learnStep == 0) {
      _learnFirst = keycode;
      _learnStep = 1;
      LOG_INF("BLE", "Learn: PageBack=0x%04X, now press PageForward", keycode);
    } else if (_learnStep == 1 && keycode != _learnFirst) {
      _learnSecond = keycode;
      _learnStep = 2;
      _learnedBack = _learnFirst;
      _learnedForward = _learnSecond;
      _learnMode = false;
      saveLearnedKeys();
      LOG_INF("BLE", "Learn: PageForward=0x%04X, done", keycode);
    }
    return;
  }

  if ((now - lastPageTurn) < PAGE_TURN_COOLDOWN_MS) return;

  if (_learnedBack != 0 && keycode == _learnedBack) {
    _pageBackFlag = true;
    lastPageTurn = now;
    return;
  }
  if (_learnedForward != 0 && keycode == _learnedForward) {
    _pageForwardFlag = true;
    lastPageTurn = now;
    return;
  }

  switch (keycode) {
    case HID_KEY_PAGE_DOWN:
    case HID_KEY_RIGHT:
    case HID_CONSUMER_NEXT:
    case HID_CONSUMER_FASTF:
      _pageForwardFlag = true;
      lastPageTurn = now;
      break;
    case HID_KEY_PAGE_UP:
    case HID_KEY_LEFT:
    case HID_CONSUMER_PREV:
    case HID_CONSUMER_REWIND:
      _pageBackFlag = true;
      lastPageTurn = now;
      break;
    default:
      LOG_DBG("BLE", "Unrecognized keycode 0x%04X", keycode);
      break;
  }
}

#endif  // BLE_ENABLED