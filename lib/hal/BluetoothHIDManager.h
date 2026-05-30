#pragma once

#ifdef BLE_ENABLED

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <NimBLEServer.h>

class BluetoothHIDManager {
 public:
  static BluetoothHIDManager& getInstance() {
    static BluetoothHIDManager instance;
    return instance;
  }

  // Call once at startup
  void begin();

  // Call once per main loop frame — clears single-frame flags
  void update();

  // True if a phone/device is currently connected
  bool isConnected() const { return _connected; }

  // Single-frame page turn flags — consumed by MappedInputManager each frame
  bool wasPageBackPressed() const { return _pageBackFlag; }
  bool wasPageForwardPressed() const { return _pageForwardFlag; }

  // Called by server callbacks
  void onConnect();
  void onDisconnect();

  // Save/load/clear learned keycodes
  void saveLearnedKeys();
  void loadLearnedKeys();
  void clearLearnedKeys();
  bool hasLearnedKeys() const { return _learnedBack != 0 || _learnedForward != 0; }

  void startAdvertising();
  void stopAdvertising();
  // Learn Mode
  void startLearnMode();
  void cancelLearnMode();
  bool isLearning() const { return _learnMode; }

  // Called when a keypress report arrives from the connected phone
  void onHIDReport(const uint8_t* data, size_t length);

  // Send a keypress from X4 to phone (not needed for page turning but useful)
  void sendKey(uint8_t keycode);

 private:
  BluetoothHIDManager() = default;

  bool _initialized = false;
  bool _connected = false;
  bool _learnMode = false;

  bool _pageBackFlag = false;
  bool _pageForwardFlag = false;

  uint16_t _learnedBack = 0;
  uint16_t _learnedForward = 0;
  uint16_t _learnFirst = 0;
  uint8_t _learnStep = 0;

  NimBLEHIDDevice* _hid = nullptr;
  NimBLECharacteristic* _inputReport = nullptr;

  friend class BLEServerCallback;
};

#define BLE_HID BluetoothHIDManager::getInstance()

#endif  // BLE_ENABLED