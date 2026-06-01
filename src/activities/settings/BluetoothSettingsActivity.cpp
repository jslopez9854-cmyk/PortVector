#ifdef BLE_ENABLED

#include "BluetoothSettingsActivity.h"

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();
  _bleEnabled = SETTINGS.bleEnabled;
  _screen = Screen::Main;
  if (_bleEnabled && !BLE_HID.isInitialized()) {
    BLE_HID.begin();
  }
  _enterTime = millis();
  requestUpdate();
}
void BluetoothSettingsActivity::onExit() {
  Activity::onExit();
  if (BLE_HID.isScanning()) BLE_HID.stopScan();
  if (BLE_HID.isLearning()) BLE_HID.cancelLearnMode();
}

void BluetoothSettingsActivity::loop() {
  switch (_screen) {
    case Screen::Main:      handleMainInput(); break;
    case Screen::Scanning:  handleScanningInput(); break;
    case Screen::Connected: handleConnectedInput(); break;
    case Screen::Learning:  handleLearningInput(); break;
  }

  if (_screen == Screen::Scanning && BLE_HID.isScanning()) requestUpdate();

  if (_screen == Screen::Scanning && BLE_HID.isConnected()) {
    BLE_HID.stopScan();
    _screen = Screen::Connected;
    requestUpdate();
  }

  if (_screen == Screen::Learning && !BLE_HID.isLearning()) {
    _screen = Screen::Connected;
    requestUpdate();
  }
}

void BluetoothSettingsActivity::render(RenderLock&& lock) {
  renderer.clearScreen();
  switch (_screen) {
    case Screen::Main:      drawMain(); break;
    case Screen::Scanning:  drawScanning(); break;
    case Screen::Connected: drawConnected(); break;
    case Screen::Learning:  drawLearning(); break;
  }
  renderer.displayBuffer();
}

void BluetoothSettingsActivity::drawMain() {
  int y = 20;
  renderer.drawText(UI_12_FONT_ID, 10, y, "Bluetooth", true);
  y += 40;

  const char* status = _bleEnabled ? "Enabled" : "Disabled";
  renderer.drawText(UI_10_FONT_ID, 10, y, status, true);
  y += 30;

  if (_bleEnabled) {
    if (BLE_HID.isConnected()) {
      std::string line = "Connected: " + BLE_HID.getConnectedDeviceName();
      renderer.drawText(UI_10_FONT_ID, 10, y, line.c_str(), true);
    } else {
      renderer.drawText(UI_10_FONT_ID, 10, y, "Not connected", true);
    }
    y += 30;
    const auto labels = mappedInput.mapLabels(
      "Back",
      BLE_HID.isConnected() ? "Learn Keys" : "",
      BLE_HID.isConnected() ? "Disconnect" : "Scan",
      "Disable");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels("Back", "Enable", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void BluetoothSettingsActivity::drawScanning() {
  int y = 20;
  renderer.drawText(UI_12_FONT_ID, 10, y, "Scanning...", true);
  y += 40;

  const auto& results = BLE_HID.getScanResults();
  if (results.empty()) {
    renderer.drawText(UI_10_FONT_ID, 10, y, "No devices found yet", true);
  } else {
    for (int i = 0; i < static_cast<int>(results.size()) && i < 8; i++) {
      const bool selected = (i == _selectedDevice);
      std::string line = (selected ? "> " : "  ") + results[i].name;
      renderer.drawText(UI_10_FONT_ID, 10, y, line.c_str(), true);
      y += 25;
    }
  }

  const auto labels = mappedInput.mapLabels("Back", "Connect", "", "Stop");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BluetoothSettingsActivity::drawConnected() {
  int y = 20;
  renderer.drawText(UI_12_FONT_ID, 10, y, "Bluetooth", true);
  y += 40;

  std::string line = "Connected: " + BLE_HID.getConnectedDeviceName();
  renderer.drawText(UI_10_FONT_ID, 10, y, line.c_str(), true);
  y += 30;

  if (BLE_HID.hasLearnedKeys()) {
    renderer.drawText(UI_10_FONT_ID, 10, y, "Keys: Learned", true);
  } else {
    renderer.drawText(UI_10_FONT_ID, 10, y, "Keys: Default", true);
  }
  y += 30;

  const auto labels = mappedInput.mapLabels("Back", "Learn Keys", "Disconnect", "Clear Keys");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BluetoothSettingsActivity::drawLearning() {
  int y = 20;
  renderer.drawText(UI_12_FONT_ID, 10, y, "Learn Mode", true);
  y += 50;

  if (_learnStep == 0) {
    renderer.drawText(UI_10_FONT_ID, 10, y, "Press your PageBack button", true);
    y += 30;
    renderer.drawText(UI_10_FONT_ID, 10, y, "on your phone/remote", true);
  } else if (_learnStep == 1) {
    renderer.drawText(UI_10_FONT_ID, 10, y, "Good! Now press PageForward", true);
    y += 30;
    renderer.drawText(UI_10_FONT_ID, 10, y, "on your phone/remote", true);
  }

  const auto labels = mappedInput.mapLabels("", "", "", "Cancel");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BluetoothSettingsActivity::handleMainInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (millis() - _enterTime < 500) return;

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

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (BLE_HID.isConnected()) {
      BLE_HID.disconnect();
      requestUpdate();
    } else {
      BLE_HID.clearScanResults();
      _selectedDevice = 0;
      _screen = Screen::Scanning;
      BLE_HID.startScan();
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (BLE_HID.isConnected()) {
      _learnStep = 0;
      _screen = Screen::Learning;
      BLE_HID.startLearnMode();
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (BLE_HID.isConnected()) {
      BLE_HID.clearLearnedKeys();
    } else {
      _bleEnabled = false;
      SETTINGS.bleEnabled = false;
      SETTINGS.saveToFile();
    }
    requestUpdate();
    return;
  }
}

void BluetoothSettingsActivity::handleScanningInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    BLE_HID.stopScan();
    _screen = Screen::Main;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (_selectedDevice > 0) { _selectedDevice--; requestUpdate(); }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (_selectedDevice < static_cast<int>(BLE_HID.getScanResults().size()) - 1) {
      _selectedDevice++; requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto& results = BLE_HID.getScanResults();
    if (!results.empty() && _selectedDevice < static_cast<int>(results.size())) {
      BLE_HID.stopScan();
      BLE_HID.connectToDevice(results[_selectedDevice].address);
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    BLE_HID.stopScan();
    _screen = Screen::Main;
    requestUpdate();
    return;
  }
}

void BluetoothSettingsActivity::handleConnectedInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    _screen = Screen::Main;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    BLE_HID.disconnect();
    _screen = Screen::Main;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    _learnStep = 0;
    _screen = Screen::Learning;
    BLE_HID.startLearnMode();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    BLE_HID.clearLearnedKeys();
    requestUpdate();
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
    _screen = Screen::Connected;
    requestUpdate();
    return;
  }

  requestUpdate();
}

#endif  // BLE_ENABLED