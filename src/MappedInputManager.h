#pragma once

#include <HalGPIO.h>

#ifdef BLE_ENABLED
#include <BluetoothHIDManager.h>
#endif

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };
  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}
  void update() const {
    gpio.update();
#ifdef BLE_ENABLED
    BLE_HID.update();
#endif
  }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous,
                   const char* next) const;
  int getPressedFrontButton() const;

 private:
  HalGPIO& gpio;
  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;

#ifdef BLE_ENABLED
  bool mapButtonBLE(Button button) const;
#endif
};