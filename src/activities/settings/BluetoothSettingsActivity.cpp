// The class declaration in BluetoothSettingsActivity.h has no BLE-only
// members, but this whole translation unit references SETTINGS.ble* fields,
// which only exist under BLE_ENABLED (see CrossPointSettings.h). src/ files
// compile unconditionally regardless of #include chains, so guard the body.
#ifdef BLE_ENABLED

#include "BluetoothSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "DeviceProfiles.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Upstream 1.6.0 replaced BaseTheme::drawList() with the declarative
// UiListActivity/FreeInkUI screen framework. Adopting that here would mean
// rewriting this activity's whole render/input state machine, which is out
// of scope for this port. This is a scoped-down reimplementation covering
// only what BluetoothSettingsActivity actually used: a title + right-aligned
// value per row, paged, with the selected row highlighted. No subtitle, icon,
// or dimmed-row support (BluetoothSettingsActivity never passed those).
void drawSimpleList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                    const std::function<std::string(int index)>& rowTitle,
                    const std::function<std::string(int index)>& rowValue) {
  const int rowHeight = BaseMetrics::values.listRowHeight;
  const int pageItems = std::max(1, rect.height / rowHeight);
  const int contentWidth = rect.width - 5;

  if (selectedIndex >= 0) {
    renderer.fillRect(0, rect.y + selectedIndex % pageItems * rowHeight - 2, rect.width, rowHeight);
  }

  const int pageStartIndex = selectedIndex / pageItems * pageItems;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    int rowTextWidth = contentWidth - BaseMetrics::values.contentSidePadding * 2;

    std::string valueText;
    if (rowValue) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        const int maxValW = std::max(0, rowTextWidth - 40 - 10);
        valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxValW);
        rowTextWidth -= renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + 10;
      }
    }

    const std::string item = renderer.truncatedText(UI_10_FONT_ID, rowTitle(i).c_str(), rowTextWidth);
    renderer.drawText(UI_10_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, itemY, item.c_str(),
                      i != selectedIndex);

    if (!valueText.empty()) {
      const int valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - BaseMetrics::values.contentSidePadding - valueTextWidth,
                        itemY, valueText.c_str(), i != selectedIndex);
    }
  }
}
}  // namespace

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  viewMode = ViewMode::MAIN_MENU;
  lastError = "";
  lastScanTime = 0;
  pendingLearnKey = 0;
  pendingLearnIndex = 0xFF;
  learnedPrevKey = 0;
  learnedNextKey = 0;
  learnedReportIndex = 2;
  learnTestDeadlineMs = 0;
  learnTestForwardSeen = false;
  learnTestBackSeen = false;
  learnTestForwardCount = 0;
  learnTestBackCount = 0;
  debugLastKeycode = 0;
  debugEventCount = 0;
  debugLastEventMs = 0;
  debugUniqueCount = 0;
  memset(debugUniqueKeys, 0, sizeof(debugUniqueKeys));
  memset(debugUniqueCounts, 0, sizeof(debugUniqueCounts));
  learnStep = LearnStep::WAIT_PREV;

  // Get BLE manager instance
  btMgr = &BluetoothHIDManager::getInstance();
  LOG_INF("BT", "BluetoothHIDManager ready");

  // Restore Bluetooth persistent state on entry
  if (SETTINGS.bleEnabled && !btMgr->isEnabled()) {
    LOG_INF("BT", "Restoring Bluetooth from settings (enabled)");
    if (btMgr->enable()) {
      lastError = tr(STR_BT_RESTORED);
    } else {
      lastError = tr(STR_BT_RESTORE_FAILED);
      SETTINGS.bleEnabled = 0;
    }
  } else if (!SETTINGS.bleEnabled && btMgr->isEnabled()) {
    LOG_INF("BT", "Disabling Bluetooth per settings (disabled)");
    btMgr->disable();
    lastError = tr(STR_BT_DISABLED_PER_SETTINGS);
  }

  requestUpdate();
}

void BluetoothSettingsActivity::onExit() {
  if (btMgr) {
    btMgr->setLearnInputCallback(nullptr);
    btMgr->setInputCallback(nullptr);
  }
  Activity::onExit();
}

void BluetoothSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (viewMode == ViewMode::DEVICE_LIST) {
      // Return to main menu
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      if (btMgr && btMgr->isScanning()) {
        btMgr->stopScan();
      }
      requestUpdate();
      return;
    } else if (viewMode == ViewMode::LEARN_KEYS) {
      if (btMgr) {
        btMgr->setLearnInputCallback(nullptr);
      }
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      if (learnStep != LearnStep::DONE) {
        lastError = tr(STR_BT_LEARN_CANCELED);
      }
      requestUpdate();
      return;
    } else if (viewMode == ViewMode::DEBUG_MONITOR) {
      if (btMgr) {
        btMgr->setInputCallback(nullptr);
      }
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      requestUpdate();
      return;
    } else {
      if (onComplete) onComplete();
      return;
    }
  }

  // Check if scan completed
  if (btMgr && viewMode == ViewMode::DEVICE_LIST && !btMgr->isScanning() && lastScanTime > 0) {
    if (millis() - lastScanTime > 500) {  // Small delay to see final results
      lastScanTime = 0;
      requestUpdate();
    }
  }

  if (viewMode == ViewMode::MAIN_MENU) {
    handleMainMenuInput();
  } else if (viewMode == ViewMode::DEVICE_LIST) {
    handleDeviceListInput();
  } else if (viewMode == ViewMode::DEBUG_MONITOR) {
    handleDebugInput();
  } else {
    handleLearnInput();
  }
}

void BluetoothSettingsActivity::handleMainMenuInput() {
  constexpr int kMainMenuItemCount =
#ifdef ENABLE_BT_DEBUG_MONITOR
      8;
#else
      7;
#endif

  constexpr int kToggleBluetoothIndex = 0;
  constexpr int kReconnectBondedIndex = 1;
  constexpr int kDisconnectDevicesIndex = 2;
  constexpr int kScanForDevicesIndex = 3;
  constexpr int kRemoteSetupWizardIndex = 4;
#ifdef ENABLE_BT_DEBUG_MONITOR
  constexpr int kDebugMonitorIndex = 5;
  constexpr int kClearLearnedKeysIndex = 6;
  constexpr int kForgetBondedRemoteIndex = 7;
#else
  constexpr int kClearLearnedKeysIndex = 5;
  constexpr int kForgetBondedRemoteIndex = 6;
#endif

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : (kMainMenuItemCount - 1);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedIndex = (selectedIndex < (kMainMenuItemCount - 1)) ? selectedIndex + 1 : 0;
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (!btMgr) {
      lastError = tr(STR_BT_NOT_AVAILABLE);
      LOG_ERR("BT", "BLE manager not available");
      requestUpdate();
      return;
    }

    if (selectedIndex == kToggleBluetoothIndex) {
      // Toggle Bluetooth
      if (btMgr->isEnabled()) {
        LOG_INF("BT", "Disabling Bluetooth...");
        if (btMgr->disable()) {
          lastError = tr(STR_BT_DISABLED);
          SETTINGS.bleEnabled = 0;
          SETTINGS.saveToFile();
        } else {
          lastError = tr(STR_BT_DISABLE_FAILED);
        }
      } else {
        LOG_INF("BT", "Enabling Bluetooth...");
        if (btMgr->enable()) {
          lastError = tr(STR_BT_ENABLED);
          SETTINGS.bleEnabled = 1;
          SETTINGS.saveToFile();
        } else {
          lastError = btMgr->lastError.empty() ? tr(STR_BT_ENABLE_FAILED) : btMgr->lastError;
        }
      }
      requestUpdate();
    } else if (selectedIndex == kReconnectBondedIndex) {
      if (!btMgr->isEnabled()) {
        lastError = tr(STR_BT_ENABLE_FIRST);
      } else if (SETTINGS.bleBondedDeviceAddr[0] == '\0') {
        lastError = tr(STR_BT_NO_BONDED_REMOTE);
      } else if (btMgr->isConnected(SETTINGS.bleBondedDeviceAddr)) {
        lastError = tr(STR_BT_ALREADY_CONNECTED);
      } else {
        LOG_INF("BT", "Reconnecting to bonded remote %s (%s)", SETTINGS.bleBondedDeviceName,
                SETTINGS.bleBondedDeviceAddr);
        lastError = tr(STR_BT_RECONNECTING);
        requestUpdate();

        if (btMgr->connectToDevice(SETTINGS.bleBondedDeviceAddr)) {
          char buf[64];
          snprintf(buf, sizeof(buf), tr(STR_BT_RECONNECTED_TO),
                   SETTINGS.bleBondedDeviceName[0] ? SETTINGS.bleBondedDeviceName : tr(STR_BT_BONDED_REMOTE_FALLBACK));
          lastError = buf;
        } else {
          lastError = btMgr->lastError.empty() ? tr(STR_BT_RECONNECT_FAILED) : btMgr->lastError;
        }
      }
      requestUpdate();
    } else if (selectedIndex == kDisconnectDevicesIndex) {
      if (!btMgr->isEnabled()) {
        lastError = tr(STR_BT_ENABLE_FIRST);
      } else {
        const auto& connectedDevices = btMgr->getConnectedDevices();
        if (connectedDevices.empty()) {
          lastError = tr(STR_BT_NO_DEVICES_CONNECTED);
        } else {
          std::vector<std::string> deviceAddresses = connectedDevices;
          for (const auto& addr : deviceAddresses) {
            btMgr->disconnectFromDevice(addr);
          }
          lastError = tr(STR_BT_DISCONNECTED);
        }
      }
      requestUpdate();
    } else if (selectedIndex == kScanForDevicesIndex) {
      // Start scan and switch to device list
      if (btMgr->isEnabled()) {
        btMgr->startScan(10000);
        lastScanTime = millis();
        viewMode = ViewMode::DEVICE_LIST;
        selectedIndex = 0;
        lastError = "";
      } else {
        lastError = tr(STR_BT_ENABLE_FIRST);
      }
      requestUpdate();
    } else if (selectedIndex == kRemoteSetupWizardIndex) {
      if (!btMgr->isEnabled()) {
        lastError = tr(STR_BT_ENABLE_FIRST);
      } else if (btMgr->getConnectedDevices().empty()) {
        lastError = tr(STR_BT_CONNECT_REMOTE_FIRST);
      } else {
        viewMode = ViewMode::LEARN_KEYS;
        learnStep = LearnStep::WAIT_PREV;
        pendingLearnKey = 0;
        pendingLearnIndex = 0xFF;
        learnedPrevKey = 0;
        learnedNextKey = 0;
        learnedReportIndex = 2;
        learnTestDeadlineMs = 0;
        learnTestForwardSeen = false;
        learnTestBackSeen = false;
        learnTestForwardCount = 0;
        learnTestBackCount = 0;
        btMgr->setLearnInputCallback([this](uint8_t keycode, uint8_t reportIndex) {
          if (viewMode == ViewMode::LEARN_KEYS && keycode > 0 && reportIndex != 0xFF) {
            pendingLearnKey = keycode;
            pendingLearnIndex = reportIndex;
          }
        });
        lastError = tr(STR_BT_WIZARD_PRESS_FORWARD);
      }
      requestUpdate();
    }
#ifdef ENABLE_BT_DEBUG_MONITOR
    else if (selectedIndex == kDebugMonitorIndex) {
      if (!btMgr->isDebugCaptureEnabled()) {
        btMgr->setDebugCaptureEnabled(true);
      }
      debugLastKeycode = 0;
      debugEventCount = 0;
      debugLastEventMs = 0;
      debugUniqueCount = 0;
      memset(debugUniqueKeys, 0, sizeof(debugUniqueKeys));
      memset(debugUniqueCounts, 0, sizeof(debugUniqueCounts));
      btMgr->setInputCallback([this](uint16_t keycode) {
        debugLastKeycode = keycode & 0xFF;
        debugEventCount++;
        debugLastEventMs = millis();

        const uint8_t code = static_cast<uint8_t>(keycode & 0xFF);
        bool found = false;
        for (uint8_t i = 0; i < debugUniqueCount; i++) {
          if (debugUniqueKeys[i] == code) {
            if (debugUniqueCounts[i] < 65535) {
              debugUniqueCounts[i]++;
            }
            found = true;
            break;
          }
        }

        if (!found && debugUniqueCount < kDebugUniqueKeyMax) {
          debugUniqueKeys[debugUniqueCount] = code;
          debugUniqueCounts[debugUniqueCount] = 1;
          debugUniqueCount++;
        }
      });
      viewMode = ViewMode::DEBUG_MONITOR;
      lastError = tr(STR_BT_DEBUG_MONITOR);
      requestUpdate();
    }
#endif
    else if (selectedIndex == kClearLearnedKeysIndex) {
      DeviceProfiles::clearCustomProfile();
      lastError = tr(STR_BT_LEARNED_CLEARED);
      requestUpdate();
    } else if (selectedIndex == kForgetBondedRemoteIndex) {
      SETTINGS.bleBondedDeviceAddr[0] = '\0';
      SETTINGS.bleBondedDeviceName[0] = '\0';
      SETTINGS.bleBondedDeviceAddrType = 0;
      SETTINGS.saveToFile();
      btMgr->setBondedDevice("", "");
      lastError = tr(STR_BT_BONDED_CLEARED);
      requestUpdate();
    }
  }
}

void BluetoothSettingsActivity::handleLearnInput() {
  if (pendingLearnKey != 0) {
    const uint8_t capturedKey = pendingLearnKey;
    const uint8_t capturedIndex = pendingLearnIndex;
    pendingLearnKey = 0;
    pendingLearnIndex = 0xFF;

    if (learnStep == LearnStep::WAIT_PREV) {
      learnedNextKey = capturedKey;  // Wizard step 1 = forward/next
      learnedReportIndex = (capturedIndex == 0xFF) ? 2 : capturedIndex;
      learnStep = LearnStep::WAIT_NEXT;
      char buf[96];
      snprintf(buf, sizeof(buf), tr(STR_BT_FORWARD_CAPTURED_FMT), learnedNextKey,
               static_cast<unsigned>(learnedReportIndex));
      lastError = buf;
      requestUpdate();
      return;
    }

    if (learnStep == LearnStep::WAIT_NEXT) {
      if (capturedKey == learnedNextKey) {
        lastError = tr(STR_BT_BACK_KEY_DIFFERENT);
        requestUpdate();
        return;
      }

      learnedPrevKey = capturedKey;  // Wizard step 2 = back/prev
      learnStep = LearnStep::WAIT_TEST;
      learnTestDeadlineMs = millis() + 10000;
      learnTestForwardSeen = false;
      learnTestBackSeen = false;
      learnTestForwardCount = 0;
      learnTestBackCount = 0;
      lastError = tr(STR_BT_TEST_INSTRUCTIONS);
      requestUpdate();
      return;
    }

    if (learnStep == LearnStep::WAIT_TEST) {
      if (capturedKey == learnedNextKey) {
        learnTestForwardSeen = true;
        if (learnTestForwardCount < 65535) {
          learnTestForwardCount++;
        }
      } else if (capturedKey == learnedPrevKey) {
        learnTestBackSeen = true;
        if (learnTestBackCount < 65535) {
          learnTestBackCount++;
        }
      }

      char buf[96];
      snprintf(buf, sizeof(buf), tr(STR_BT_TEST_STATUS_FMT),
               learnTestForwardSeen ? tr(STR_BT_TEST_OK) : tr(STR_BT_TEST_PENDING),
               static_cast<unsigned>(learnTestForwardCount),
               learnTestBackSeen ? tr(STR_BT_TEST_OK) : tr(STR_BT_TEST_PENDING),
               static_cast<unsigned>(learnTestBackCount));
      lastError = buf;
      requestUpdate();
      return;
    }
  }

  if (learnStep == LearnStep::WAIT_TEST && millis() > learnTestDeadlineMs) {
    if (btMgr) {
      btMgr->setLearnInputCallback(nullptr);
    }
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    lastError = tr(STR_BT_WIZARD_TIMEOUT);
    requestUpdate();
    return;
  }

  if (learnStep == LearnStep::WAIT_TEST && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    DeviceProfiles::setCustomProfile(learnedPrevKey, learnedNextKey, learnedReportIndex);
    if (btMgr) {
      const auto& connected = btMgr->getConnectedDevices();
      for (const auto& addr : connected) {
        DeviceProfiles::setCustomProfileForDevice(addr, learnedPrevKey, learnedNextKey, learnedReportIndex);
      }
      btMgr->setLearnInputCallback(nullptr);
    }
    learnStep = LearnStep::DONE;
    char buf[96];
    snprintf(buf, sizeof(buf), tr(STR_BT_SAVED_FMT), learnedPrevKey, learnedNextKey);
    lastError = buf;

    // On successful wizard completion, return immediately to menu (or back to book).
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    if (exitOnSuccessfulConnect) {
      if (onComplete) onComplete();
      return;
    }

    requestUpdate();
    return;
  }

  if (learnStep == LearnStep::DONE && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (btMgr) {
      btMgr->setLearnInputCallback(nullptr);
    }
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    requestUpdate();
  }
}

void BluetoothSettingsActivity::handleDeviceListInput() {
  if (!btMgr) return;

  const auto& devices = btMgr->getDiscoveredDevices();
  const auto& connectedDevices = btMgr->getConnectedDevices();

  // Calculate menu items: devices + "Refresh" + "Disconnect" (if connected)
  int menuItems = devices.size() + 1;  // +1 for Refresh
  if (!connectedDevices.empty()) {
    menuItems++;  // +1 for Disconnect
  }
  int maxIndex = menuItems - 1;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : maxIndex;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedIndex = (selectedIndex < maxIndex) ? selectedIndex + 1 : 0;
    requestUpdate();
  }

  // Left/Right for back/refresh
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Go back to main menu
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    if (btMgr && btMgr->isScanning()) {
      btMgr->stopScan();
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    // Quick rescan
    LOG_INF("BT", "Quick rescan...");
    lastError = tr(STR_SCANNING);
    btMgr->startScan(10000);
    lastScanTime = millis();
    selectedIndex = 0;
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // Check if "Refresh" is selected
    if (selectedIndex == static_cast<int>(devices.size())) {
      LOG_INF("BT", "Refreshing scan...");
      lastError = tr(STR_SCANNING);
      btMgr->startScan(10000);
      lastScanTime = millis();
      selectedIndex = 0;
      requestUpdate();
      return;
    }

    // Check if "Disconnect" is selected
    if (!connectedDevices.empty() && selectedIndex == static_cast<int>(devices.size()) + 1) {
      LOG_INF("BT", "Disconnecting from all devices...");
      // Make a copy of addresses to avoid iterator invalidation
      std::vector<std::string> deviceAddresses = connectedDevices;
      for (const auto& addr : deviceAddresses) {
        LOG_DBG("BT", "Disconnecting from %s", addr.c_str());
        btMgr->disconnectFromDevice(addr);
      }
      lastError = tr(STR_BT_DISCONNECTED);
      selectedIndex = 0;
      requestUpdate();
      return;
    }

    // Otherwise, connect to selected device
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(devices.size())) {
      const auto& device = devices[selectedIndex];

      LOG_INF("BT", "Connecting to %s (%s)", device.name.c_str(), device.address.c_str());
      lastError = tr(STR_BT_CONNECTING);
      requestUpdate();

      if (btMgr->connectToDevice(device.address)) {
        strncpy(SETTINGS.bleBondedDeviceAddr, device.address.c_str(), sizeof(SETTINGS.bleBondedDeviceAddr) - 1);
        SETTINGS.bleBondedDeviceAddr[sizeof(SETTINGS.bleBondedDeviceAddr) - 1] = '\0';
        strncpy(SETTINGS.bleBondedDeviceName, device.name.c_str(), sizeof(SETTINGS.bleBondedDeviceName) - 1);
        SETTINGS.bleBondedDeviceName[sizeof(SETTINGS.bleBondedDeviceName) - 1] = '\0';
        SETTINGS.bleBondedDeviceAddrType = 0;
        SETTINGS.saveToFile();
        btMgr->setBondedDevice(device.address, device.name);

        lastError = tr(STR_BT_ENABLED);
        LOG_INF("BT", "Successfully connected to %s", device.name.c_str());
        if (exitOnSuccessfulConnect) {
          if (onComplete) onComplete();
          return;
        }
      } else {
        lastError = btMgr->lastError.empty() ? tr(STR_BT_CONNECTION_FAILED) : btMgr->lastError;
        LOG_ERR("BT", "Failed to connect: %s", lastError.c_str());
      }
      requestUpdate();
    }
  }
}

void BluetoothSettingsActivity::render(RenderLock&&) {
  if (viewMode == ViewMode::MAIN_MENU) {
    renderMainMenu();
  } else if (viewMode == ViewMode::DEVICE_LIST) {
    renderDeviceList();
  } else if (viewMode == ViewMode::DEBUG_MONITOR) {
    renderDebugMonitor();
  } else {
    renderLearnKeys();
  }
}

void BluetoothSettingsActivity::handleDebugInput() {
  if (!btMgr) {
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const bool next = !btMgr->isDebugCaptureEnabled();
    btMgr->setDebugCaptureEnabled(next);
    lastError = next ? tr(STR_BT_DEBUG_CAPTURE_ON) : tr(STR_BT_DEBUG_CAPTURE_OFF);
    requestUpdate();
    return;
  }
}

void BluetoothSettingsActivity::renderMainMenu() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Header with Bluetooth title
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH));

  // Status subheader
  std::string statusLine;
  if (btMgr) {
    if (btMgr->isEnabled()) {
      auto connDevices = btMgr->getConnectedDevices();
      if (!connDevices.empty()) {
        char buf[64];
        snprintf(buf, sizeof(buf), tr(STR_BT_ENABLED_DEVICES_FMT), connDevices.size());
        statusLine = buf;
      } else {
        statusLine = tr(STR_BT_ENABLED_NO_DEVICES);
      }
    } else {
      statusLine = tr(STR_BT_STATUS_DISABLED);
    }
  } else {
    statusLine = tr(STR_BT_INIT_ERROR);
  }

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    statusLine.c_str());

  int listOffsetY = 0;
  if (btMgr && btMgr->isEnabled() && SETTINGS.bleBondedDeviceName[0] != '\0') {
    const int nameY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 4;
    char remoteBuf[64];
    snprintf(remoteBuf, sizeof(remoteBuf), tr(STR_BT_REMOTE_NAME_FMT), SETTINGS.bleBondedDeviceName);
    std::string deviceLine =
        renderer.truncatedText(UI_10_FONT_ID, remoteBuf, pageWidth - metrics.contentSidePadding * 2);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, nameY, deviceLine.c_str(), true);
    listOffsetY = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  }

  // Use drawSimpleList for consistent formatting with main settings
  const char* items[] = {
      btMgr && btMgr->isEnabled() ? tr(STR_BT_TOGGLE_DISABLE) : tr(STR_BT_TOGGLE_ENABLE),
      tr(STR_BT_RECONNECT_BONDED),
      tr(STR_BT_DISCONNECT_DEVICES),
      tr(STR_BT_SCAN_DEVICES),
      tr(STR_BT_SETUP_WIZARD),
#ifdef ENABLE_BT_DEBUG_MONITOR
      btMgr && btMgr->isDebugCaptureEnabled() ? tr(STR_BT_DEBUG_CAPTURE_DISABLE) : tr(STR_BT_DEBUG_CAPTURE_ENABLE),
#endif
      tr(STR_BT_CLEAR_LEARNED),
      tr(STR_BT_FORGET_BONDED)};

  std::vector<std::string> itemLabels;
  for (int i = 0; i < static_cast<int>(sizeof(items) / sizeof(items[0])); i++) {
    itemLabels.push_back(items[i]);
  }

  drawSimpleList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing + listOffsetY,
           pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2 + listOffsetY)},
      static_cast<int>(itemLabels.size()), selectedIndex, [&itemLabels](int index) { return itemLabels[index]; },
      [this](int i) {
        if (i == 0) {
          return std::string(btMgr && btMgr->isEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
        }
        if (i == 1 && SETTINGS.bleBondedDeviceName[0] != '\0') {
          return renderer.truncatedText(
              UI_10_FONT_ID, SETTINGS.bleBondedDeviceName,
              renderer.getScreenWidth() - UITheme::getInstance().getMetrics().contentSidePadding * 4);
        }
        return std::string("");
      });

  if (!lastError.empty()) {
    std::string statusText =
        renderer.truncatedText(UI_10_FONT_ID, lastError.c_str(), pageWidth - metrics.contentSidePadding * 2);
    const int statusY =
        pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, statusY, statusText.c_str(), true);
  }

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderDeviceList() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (!btMgr) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_BT_ERROR));
    return;
  }

  const auto& devices = btMgr->getDiscoveredDevices();
  const auto& connectedDevices = btMgr->getConnectedDevices();

  // Header with device count
  char countStr[32];
  snprintf(countStr, sizeof(countStr), btMgr->isScanning() ? tr(STR_SCANNING) : tr(STR_BT_FOUND_COUNT_FMT),
           devices.size());
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH), countStr);

  // Subheader with scan status
  std::string subheaderText;
  if (btMgr->isScanning()) {
    subheaderText = tr(STR_BT_SEARCHING);
  } else {
    if (devices.empty()) {
      subheaderText = tr(STR_BT_NO_DEVICES_FOUND);
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), tr(STR_BT_DEVICES_AVAILABLE_FMT), (int)devices.size());
      subheaderText = buf;
    }
  }

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    subheaderText.c_str());

  // Build device list labels. `GUI.drawList()` already paginates based on
  // `selectedIndex`, so keep the full device list here and let the user scroll
  // through every discovered device instead of truncating after the first page.
  std::vector<std::string> deviceLabels;
  std::vector<std::string> deviceValues;
  char buf[128];

  if (!devices.empty()) {
    for (const auto& device : devices) {
      const bool connected = btMgr->isConnected(device.address);

      // Device name with indicators
      const char* connSymbol = connected ? "[*] " : "";
      const char* hidSymbol = device.isHID ? "[HID] " : "";
      snprintf(buf, sizeof(buf), "%s%s%s", connSymbol, hidSymbol, device.name.c_str());
      deviceLabels.push_back(buf);

      // RSSI/signal strength
      const std::string signalBars = getSignalStrengthIndicator(device.rssi);
      snprintf(buf, sizeof(buf), "%s (%d dBm)", signalBars.c_str(), device.rssi);
      deviceValues.push_back(buf);
    }
  }

  // Add action buttons after the full device list.
  deviceLabels.push_back(tr(STR_BT_RESCAN));
  deviceValues.push_back("");

  if (!connectedDevices.empty()) {
    deviceLabels.push_back(tr(STR_BT_DISCONNECT_ALL));
    deviceValues.push_back("");
  }

  // Render the list using the local drawSimpleList helper (see top of file)
  drawSimpleList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      static_cast<int>(deviceLabels.size()), selectedIndex, [&deviceLabels](int index) { return deviceLabels[index]; },
      [&deviceValues](int i) { return i < (int)deviceValues.size() ? deviceValues[i] : std::string(""); });

  // Help text
  GUI.drawHelpText(renderer,
                   Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - 15, pageWidth, 20},
                   tr(STR_BT_SCROLL_HINT));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), tr(STR_DIR_LEFT), tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::string BluetoothSettingsActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // BLE RSSI tends to be lower than WiFi at similar distance.
  // Use BLE-friendly thresholds so nearby remotes are not shown as always weak.
  if (rssi >= -60) {
    return "||||";  // Excellent
  }
  if (rssi >= -70) {
    return " |||";  // Good
  }
  if (rssi >= -80) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}

void BluetoothSettingsActivity::renderLearnKeys() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BT_SETUP_WIZARD));

  const char* stepText = tr(STR_BT_STEP_PRESS_FORWARD);
  if (learnStep == LearnStep::WAIT_NEXT) {
    stepText = tr(STR_BT_STEP_PRESS_BACK);
  } else if (learnStep == LearnStep::WAIT_TEST) {
    stepText = tr(STR_BT_STEP_TEST_BOTH);
  } else if (learnStep == LearnStep::DONE) {
    stepText = tr(STR_BT_STEP_COMPLETE);
  }

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    stepText);

  char line1[64];
  char line2[64];
  snprintf(line1, sizeof(line1), tr(STR_BT_FORWARD_KEY_FMT), learnedNextKey ? tr(STR_BT_CAPTURED) : tr(STR_BT_WAITING));
  snprintf(line2, sizeof(line2), tr(STR_BT_BACK_KEY_FMT), learnedPrevKey ? tr(STR_BT_CAPTURED) : tr(STR_BT_WAITING));

  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 32,
                            line1);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 56,
                            line2);

  if (learnedNextKey || learnedPrevKey) {
    char line3[48];
    char line4[64];
    if (learnStep == LearnStep::WAIT_TEST) {
      unsigned int remaining = (learnTestDeadlineMs > millis()) ? (learnTestDeadlineMs - millis()) / 1000 : 0;
      snprintf(line3, sizeof(line3), tr(STR_BT_TIME_LEFT_FMT), remaining);
      snprintf(line4, sizeof(line4), tr(STR_BT_FWD_BACK_COUNT_FMT), static_cast<unsigned>(learnTestForwardCount),
               static_cast<unsigned>(learnTestBackCount));
      renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 100,
                                line4);
    } else {
      snprintf(line3, sizeof(line3), tr(STR_BT_REPORT_BYTE_FMT), static_cast<unsigned>(learnedReportIndex));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 80,
                              line3);
  }

  if (!lastError.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - 16, lastError.c_str());
  }

  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), (learnStep == LearnStep::DONE || learnStep == LearnStep::WAIT_TEST) ? tr(STR_SELECT) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderDebugMonitor() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BT_DEBUG_HEADER));

  std::string sub = btMgr && btMgr->isDebugCaptureEnabled() ? tr(STR_BT_CAPTURE_ON) : tr(STR_BT_CAPTURE_OFF);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    sub.c_str());

  char line1[64];
  char line2[64];
  char line3[64];
  char line4[64];

  unsigned int connectedCount = btMgr ? static_cast<unsigned int>(btMgr->getConnectedDevices().size()) : 0;
  snprintf(line1, sizeof(line1), tr(STR_BT_CONNECTED_FMT), connectedCount);
  snprintf(line2, sizeof(line2), tr(STR_BT_KEY_EVENTS_FMT), static_cast<unsigned>(debugEventCount));
  snprintf(line3, sizeof(line3), tr(STR_BT_UNIQUE_KEYS_FMT), static_cast<unsigned>(debugUniqueCount));
  snprintf(line4, sizeof(line4), tr(STR_BT_LAST_KEY_FMT), static_cast<unsigned>(debugLastKeycode & 0xFF));

  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 24,
                            line1);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 48,
                            line2);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 72,
                            line3);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 96,
                            line4);

  if (debugLastEventMs > 0) {
    char eventAgeLine[64];
    snprintf(eventAgeLine, sizeof(eventAgeLine), tr(STR_BT_LAST_EVENT_FMT), (millis() - debugLastEventMs) / 1000);
    renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 114,
                              eventAgeLine);
  }

  const int uniqueStartY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 132;
  if (debugUniqueCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY, tr(STR_BT_NO_KEYPRESSES));
  } else {
    uint8_t sortedIndices[kDebugUniqueKeyMax] = {0};
    for (uint8_t i = 0; i < debugUniqueCount; i++) {
      sortedIndices[i] = i;
    }

    for (uint8_t i = 0; i + 1 < debugUniqueCount; i++) {
      uint8_t best = i;
      for (uint8_t j = i + 1; j < debugUniqueCount; j++) {
        const uint16_t bestCount = debugUniqueCounts[sortedIndices[best]];
        const uint16_t candidateCount = debugUniqueCounts[sortedIndices[j]];
        if (candidateCount > bestCount) {
          best = j;
        }
      }
      if (best != i) {
        const uint8_t tmp = sortedIndices[i];
        sortedIndices[i] = sortedIndices[best];
        sortedIndices[best] = tmp;
      }
    }

    const uint8_t renderCount = (debugUniqueCount < 4) ? debugUniqueCount : 4;
    for (uint8_t i = 0; i < renderCount; i++) {
      const uint8_t idx = sortedIndices[i];
      char keyLine[64];
      snprintf(keyLine, sizeof(keyLine), tr(STR_BT_KEY_COUNT_FMT), static_cast<unsigned>(debugUniqueKeys[idx]),
               static_cast<unsigned>(debugUniqueCounts[idx]));
      renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY + static_cast<int>(i) * 16, keyLine);
    }

    if (debugUniqueCount > renderCount) {
      char moreLine[48];
      snprintf(moreLine, sizeof(moreLine), tr(STR_BT_MORE_KEYS_FMT),
               static_cast<unsigned>(debugUniqueCount - renderCount));
      renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY + static_cast<int>(renderCount) * 16, moreLine);
    }
  }

  if (!lastError.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - 16, lastError.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // BLE_ENABLED