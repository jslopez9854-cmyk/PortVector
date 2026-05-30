#ifdef BLE_ENABLED

#include "BluetoothHIDManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

#if defined(ARDUINO) && __has_include(<esp32-hal-bt-mem.h>)
#include <esp32-hal-bt-mem.h>
#endif

// HID Report Descriptor — simple 2-button consumer control device
// Reports page up/down as consumer page keycodes
static const uint8_t hidReportDescriptor[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x02,        //   Report Count (2)
    0x0A, 0x37, 0x02,  //   Usage (AC Scroll Down / Page Down)
    0x0A, 0x36, 0x02,  //   Usage (AC Scroll Up / Page Up)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0x95, 0x06,        //   Report Count (6) — padding
    0x81, 0x03,        //   Input (Constant)
    0xC0               // End Collection
};

static constexpr unsigned long PAGE_TURN_COOLDOWN_MS = 300;

// ---------------------------------------------------------------------------
// Server callback
// ---------------------------------------------------------------------------

class BLEServerCallback : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    LOG_INF("BLE", "Phone connected");
    BLE_HID.onConnect();
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    LOG_INF("BLE", "Phone disconnected (reason: %d)", reason);
    BLE_HID.onDisconnect();
    // Restart advertising so phone can reconnect
    NimBLEDevice::startAdvertising();
  }
};

// ---------------------------------------------------------------------------
// BluetoothHIDManager implementation
// ---------------------------------------------------------------------------

void BluetoothHIDManager::begin() {
  if (_initialized) {
    LOG_INF("BLE", "BLE already initialized, skipping");
    return;
  }
  _initialized = true;
  LOG_INF("BLE", "Initializing BLE HID server");

  // CRITICAL: WiFi and BLE share one radio on ESP32-C3
  if (WiFi.getMode() != WIFI_OFF) {
    LOG_INF("BLE", "Disabling WiFi for BLE (mutual exclusion)");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);
  }

  NimBLEDevice::init("CrossPoint");
  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND);
  NimBLEDevice::setSecurityIOCap(BLE_SM_IO_CAP_NO_IO);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);;
  NimBLEDevice::setPower(3);  // Medium power

  // Create server
  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new BLEServerCallback());

  // Create HID device
  _hid = new NimBLEHIDDevice(pServer);
  _hid->setManufacturer("CrossPoint");
  _hid->setPnp(0x02, 0x05AC, 0x820A, 0x0210);
  _hid->setHidInfo(0x00, 0x01);
  _hid->setReportMap((uint8_t*)hidReportDescriptor, sizeof(hidReportDescriptor));

  // Get input report characteristic for report ID 1
  _inputReport = _hid->getInputReport(1);

  _hid->setBatteryLevel(100);
  _hid->startServices();

  // Start advertising
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setAppearance(HID_KEYBOARD);
  pAdvertising->addServiceUUID(_hid->getHidService()->getUUID());
  pAdvertising->enableScanResponse(true);
loadLearnedKeys();
  LOG_INF("BLE", "BLE HID server ready");

  startAdvertising();
}

void BluetoothHIDManager::startAdvertising() {  if (!_initialized || !_hid) {
    LOG_ERR("BLE", "Cannot advertise - not initialized");
    return;
  }
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  if (!pAdvertising) {
    LOG_ERR("BLE", "Cannot advertise - no advertising object");
    return;
  }
  pAdvertising->reset();
  pAdvertising->setAppearance(HID_KEYBOARD);
  pAdvertising->addServiceUUID(_hid->getHidService()->getUUID());
  pAdvertising->addServiceUUID(_hid->getBatteryService()->getUUID());
  pAdvertising->addServiceUUID(_hid->getDeviceInfoService()->getUUID());
  pAdvertising->enableScanResponse(true);
  pAdvertising->setName("CrossPoint");
  pAdvertising->start();
  LOG_INF("BLE", "Advertising as CrossPoint (HID keyboard)");
}
void BluetoothHIDManager::update() {
  _pageBackFlag = false;
  _pageForwardFlag = false;
}

void BluetoothHIDManager::onConnect() {
  _connected = true;
}

void BluetoothHIDManager::onDisconnect() {
  _connected = false;
}

void BluetoothHIDManager::sendKey(uint8_t keycode) {
  if (!_inputReport || !_connected) return;
  uint8_t report[1] = {keycode};
  _inputReport->setValue(report, sizeof(report));
  _inputReport->notify();
  delay(10);
  uint8_t release[1] = {0};
  _inputReport->setValue(release, sizeof(release));
  _inputReport->notify();
}
void BluetoothHIDManager::stopAdvertising() {
  if (!_initialized) return;
  NimBLEDevice::stopAdvertising();
  LOG_INF("BLE", "Advertising stopped for WiFi");
}
void BluetoothHIDManager::startLearnMode() {
  _learnMode = true;
  _learnFirst = 0;
  _learnStep = 0;
  LOG_INF("BLE", "Learn Mode started");
}

void BluetoothHIDManager::cancelLearnMode() {
  _learnMode = false;
  _learnStep = 0;
  _learnFirst = 0;
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

void BluetoothHIDManager::onHIDReport(const uint8_t* data, size_t length) {
  // Not used in server mode — phone sends keypresses via OS
  // This is kept for future use
}

#endif  // BLE_ENABLED