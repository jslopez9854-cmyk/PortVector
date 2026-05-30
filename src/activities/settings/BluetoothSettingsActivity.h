#pragma once

#ifdef BLE_ENABLED

#include "activities/Activity.h"
#include "MappedInputManager.h"
#include <GfxRenderer.h>

class BluetoothSettingsActivity : public Activity {
 public:
  BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Bluetooth", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
enum class Screen {
    Main,
    Learning,
  };
  
  Screen _screen = Screen::Main;

  // Index of selected device in scan results
  int _selectedDevice = 0;

  // Learn Mode step label
  // 0 = press PageBack, 1 = press PageForward, 2 = done
  uint8_t _learnStep = 0;

  // Whether BLE is enabled in settings
  bool _bleEnabled = false;

  void drawMain();
  void drawScanning();
  void drawConnected();
  void drawLearning();

  void handleMainInput();
  void handleScanningInput();
  void handleConnectedInput();
  void handleLearningInput();
};

#endif  // BLE_ENABLED