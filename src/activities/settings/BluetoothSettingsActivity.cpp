#ifdef BLE_ENABLED

#include "BluetoothSettingsActivity.h"

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include <NimBLEDevice.h>

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();
  _bleEnabled = SETTINGS.bleEnabled;
  _screen = Screen::Main;
  if (_bleEnabled) {
    BLE_HID.begin();
    BLE_HID.startAdvertising();
  }
  requestUpdate();
}

void BluetoothSettingsActivity::onExit() {
  Activity::onExit();
  if (BLE_HID.isLearning()) {
    BLE_HID.cancelLearnMode();
  }
}

void BluetoothSettingsActivity::loop() {
  switch (_screen) {
    case Screen::Main:
      handleMainInput();
      break;
    case Screen::Learning:
      handleLearningInput();
      break;
  }

  // Auto-advance when Learn Mode finishes
  if (_screen == Screen::Learning && !BLE_HID.isLearning()) {
    _screen = Screen::Main;
    requestUpdate();
  }

  // Refresh periodically to update connection status
  static unsigned long lastRefresh = 0;
  if (millis() - lastRefresh > 1000) {
    lastRefresh = millis();
    requestUpdate();
  }
}

void BluetoothSettingsActivity::render(RenderLock&& lock) {
  renderer.clearScreen();
  switch (_screen) {
    case Screen::Main:
      drawMain();
      break;
    case Screen::Learning:
      drawLearning();
      break;
  }
  renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// Draw methods
// ---------------------------------------------------------------------------

void BluetoothSettingsActivity::drawMain() {
  int y = 20;
  renderer.drawText(UI_12_FONT_ID, 10, y, "Bluetooth", true);
  y += 40;

  if (!_bleEnabled) {
    renderer.drawText(UI_10_FONT_ID, 10, y, "Disabled", true);
    y += 30;
    const auto labels = mappedInput.mapLabels("Back", "Enable", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  renderer.drawText(UI_10_FONT_ID, 10, y, "Enabled — advertising as CrossPoint", true);
  y += 30;
  std::string mac = "MAC: " + std::string(NimBLEDevice::getAddress().toString().c_str());
  renderer.drawText(UI_10_FONT_ID, 10, y, mac.c_str(), true);
  y += 25;

  if (BLE_HID.isConnected()) {
    renderer.drawText(UI_10_FONT_ID, 10, y, "Phone connected", true);
    y += 30;
    if (BLE_HID.hasLearnedKeys()) {
      renderer.drawText(UI_10_FONT_ID, 10, y, "Keys: Learned", true);
    } else {
      renderer.drawText(UI_10_FONT_ID, 10, y, "Keys: Default", true);
    }
    y += 30;
    const auto labels = mappedInput.mapLabels("Back", "Learn Keys", "", "Clear Keys");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    renderer.drawText(UI_10_FONT_ID, 10, y, "Waiting for phone to connect...", true);
    y += 25;
    renderer.drawText(UI_10_FONT_ID, 10, y, "Pair CrossPoint in phone Bluetooth settings", true);
    y += 30;
    const auto labels = mappedInput.mapLabels("Back", "", "", "Disable");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void BluetoothSettingsActivity::drawLearning() {
  int y = 20;
  renderer.drawText(UI_12_FONT_ID, 10, y, "Learn Mode", true);
  y += 50;

  if (_learnStep == 0) {
    renderer.drawText(UI_10_FONT_ID, 10, y, "Press PageBack on your phone app", true);
  } else if (_learnStep == 1) {
    renderer.drawText(UI_10_FONT_ID, 10, y, "Good! Now press PageForward", true);
  }

  const auto labels = mappedInput.mapLabels("", "", "", "Cancel");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---------------------------------------------------------------------------
// Input handlers
// ---------------------------------------------------------------------------

void BluetoothSettingsActivity::handleMainInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (!_bleEnabled) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      _bleEnabled = true;
      SETTINGS.bleEnabled = true;
      SETTINGS.saveToFile();
      BLE_HID.begin();
      requestUpdate();
    }
    return;
  }

  // Confirm = Learn Keys (only when connected)
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (BLE_HID.isConnected()) {
      _learnStep = 0;
      _screen = Screen::Learning;
      BLE_HID.startLearnMode();
      requestUpdate();
    }
    return;
  }

  // Right = Disable or Clear Keys
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (BLE_HID.isConnected()) {
      BLE_HID.clearLearnedKeys();
      requestUpdate();
    } else {
      _bleEnabled = false;
      SETTINGS.bleEnabled = false;
      SETTINGS.saveToFile();
      requestUpdate();
    }
    return;
  }
}

void BluetoothSettingsActivity::handleLearningInput() {
  if (!BLE_HID.isLearning() && _learnStep < 2) {
    _learnStep = 2;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    BLE_HID.cancelLearnMode();
    _screen = Screen::Main;
    requestUpdate();
    return;
  }

  requestUpdate();
}

#endif  // BLE_ENABLED