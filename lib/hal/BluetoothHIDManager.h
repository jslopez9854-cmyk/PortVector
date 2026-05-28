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

  // Call once at startup when BLE is enabled
  void begin();

  // Call once per main loop frame — clears single-frame flags
  void update();

  // Start scanning for BLE HID devices
  void startScan();

  // Stop scanning
  void stopScan();

  // Connect to a specific device by address
  void connectToDevice(const std::string& address);

  // Disconnect current device
  void disconnect();

  // Enter Learn Mode — next two distinct keycodes received become PageBack and PageForward
  void startLearnMode();

  // Cancel Learn Mode without saving
  void cancelLearnMode();

  // True while Learn Mode is active
  bool isLearning() const { return _learnMode; }

  // True if a device is currently connected
  bool isConnected() const { return _connected; }

  // True if currently scanning
  bool isScanning() const { return _scanning; }

  // Name of connected device, empty if none
  const std::string& getConnectedDeviceName() const { return _connectedName; }

  // Single-frame page turn flags — consumed by MappedInputManager each frame
  bool wasPageBackPressed() const { return _pageBackFlag; }
  bool wasPageForwardPressed() const { return _pageForwardFlag; }

  // Scan result access
  struct ScanResult {
    std::string address;
    std::string name;
  };
  const std::vector<ScanResult>& getScanResults() const { return _scanResults; }
  void clearScanResults() { _scanResults.clear(); }

  // Persist learned keycodes to SD settings
  void saveLearnedKeys();

  // Load learned keycodes from SD settings
  void loadLearnedKeys();

  // Clear learned keycodes
  void clearLearnedKeys();

  bool hasLearnedKeys() const { return _learnedBack != 0 || _learnedForward != 0; }

public:
  // Called by NimBLE callbacks — public so free callback functions can access it
  void onHIDReport(const uint8_t* data, size_t length);

 private:
  BluetoothHIDManager() = default;
  
  // Called by NimBLE scan callback when a device is found
  void onDeviceFound(const std::string& address, const std::string& name);

  // Called on connect/disconnect events
  void onConnect();
  void onDisconnect();

  bool _connected = false;
  bool _scanning = false;
  bool _learnMode = false;

  // Single-frame flags, cleared each update()
  bool _pageBackFlag = false;
  bool _pageForwardFlag = false;

  // Learned keycodes from Learn Mode (0 = not set)
  uint16_t _learnedBack = 0;
  uint16_t _learnedForward = 0;

  // Learn Mode state — captures first two distinct codes seen
  uint16_t _learnFirst = 0;
  uint16_t _learnSecond = 0;
  uint8_t _learnStep = 0;  // 0 = waiting for back, 1 = waiting for forward, 2 = done

  std::string _connectedAddress;
  std::string _connectedName;
  std::vector<ScanResult> _scanResults;

  NimBLEClient* _client = nullptr;

  // NimBLE callback classes — declared as friends so they can call private methods
  friend class BLEScanCallback;
  friend class BLEClientCallback;
  friend class BLEHIDReportCallback;
};

#define BLE_HID BluetoothHIDManager::getInstance()

#endif  // BLE_ENABLED