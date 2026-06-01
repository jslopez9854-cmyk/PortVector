#pragma once

#ifdef BLE_ENABLED

#include <Arduino.h>
#include <NimBLEDevice.h>

class BluetoothHIDManager {
 public:
  static BluetoothHIDManager& getInstance() {
    static BluetoothHIDManager instance;
    return instance;
  }

  void begin();
  void update();

  void startScan();
  void stopScan();
  void connectToDevice(const std::string& address);
  void disconnect();

  void startLearnMode();
  void cancelLearnMode();
  bool isLearning() const { return _learnMode; }

  bool isConnected() const { return _connected; }
  bool isScanning() const { return _scanning; }

  const std::string& getConnectedDeviceName() const { return _connectedName; }

  bool wasPageBackPressed() const { return _pageBackFlag; }
  bool wasPageForwardPressed() const { return _pageForwardFlag; }
  bool isInitialized() const { return _initialized; }

  struct ScanResult {
    std::string address;
    std::string name;
  };
  const std::vector<ScanResult>& getScanResults() const { return _scanResults; }
  void clearScanResults() { _scanResults.clear(); }

  void saveLearnedKeys();
  void loadLearnedKeys();
  void clearLearnedKeys();
  bool hasLearnedKeys() const { return _learnedBack != 0 || _learnedForward != 0; }

  void onHIDReport(const uint8_t* data, size_t length);
  void onDeviceFound(const std::string& address, const std::string& name);
  void onConnect();
  void onDisconnect();

 private:
  BluetoothHIDManager() = default;

  bool _initialized = false;
  bool _connected = false;
  bool _scanning = false;
  bool _learnMode = false;

  bool _pageBackFlag = false;
  bool _pageForwardFlag = false;
  

  uint16_t _learnedBack = 0;
  uint16_t _learnedForward = 0;
  uint16_t _learnFirst = 0;
  uint16_t _learnSecond = 0;
  uint8_t _learnStep = 0;

  std::string _connectedAddress;
  std::string _connectedName;
  std::vector<ScanResult> _scanResults;

  NimBLEClient* _client = nullptr;

  friend class BLEScanCallback;
  friend class BLEClientCallback;
};

#define BLE_HID BluetoothHIDManager::getInstance()

#endif  // BLE_ENABLED