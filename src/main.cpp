/*
  DX1 II Bluetooth Remote for M5Stack Cardputer-Adv
  =================================================

  Goals of this revision:
  - Low idle power without sacrificing BLE keyboard reliability.
  - No blocking delay in HID key transmission.
  - Bounded memory usage; avoid long-lived Arduino String fragmentation.
  - Debounced NVS writes to reduce flash wear.
  - Coalesced display refreshes to reduce SPI traffic and flicker.
  - Useful local settings and diagnostics.

  PC bridge hotkeys (Ctrl+Alt+key):
    C/X       connect/disconnect DX1 II
    1/2       volume down/up (supports accelerated hold repeat)
    3         cycle volume presets
    0/R       safe volume / restore prior volume
    M         mute toggle
    I         input toggle
    O         output cycle
    U/T       USB / optical input directly
    7/8/9     headphone / line-out / all outputs directly

  System media keys:
    P/N/B     play-pause / next / previous

  Local Fn settings:
    Fn+S      auto screen-off on/off
    Fn+,/.    shorter/longer screen timeout
    Fn+L      brightness preset
    Fn+E      eco/responsive mode
    Fn+W      wake-only first key on/off
    Fn+/      page switch
*/

#include <M5Cardputer.h>

// M5Cardputer and ESP32-BLE-Keyboard expose some overlapping key macros.
#undef KEY_LEFT_CTRL
#undef KEY_LEFT_SHIFT
#undef KEY_LEFT_ALT
#undef KEY_BACKSPACE
#undef KEY_TAB
#undef KEY_DELETE
#undef KEY_F1
#undef KEY_F2
#undef KEY_F3
#undef KEY_F4
#undef KEY_F5
#undef KEY_F6
#undef KEY_F7
#undef KEY_F8
#undef KEY_F9
#undef KEY_F10
#undef KEY_F11
#undef KEY_F12

#include <BleKeyboard.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <stdarg.h>

BleKeyboard bleKeyboard("DX1II Remote", "M5Stack", 100);
Preferences prefs;

namespace {

// -----------------------------------------------------------------------------
// Static configuration
// -----------------------------------------------------------------------------

struct KeyAction {
  char key;
  char sendChar;
  const char* label;
};

struct MediaAction {
  char key;
  const uint8_t* mediaKey;
  const char* label;
};

// Keep this map synchronized with server.js HOTKEY_ACTIONS.
constexpr KeyAction kActions[] = {
    {'c', 'c', "连接"},       {'x', 'x', "断开"},
    {'1', '1', "音量-"},      {'2', '2', "音量+"},
    {'3', '3', "音量预设"},   {'0', '0', "安全音量"},
    {'r', 'r', "恢复音量"},   {'m', 'm', "静音切换"},
    {'i', 'i', "输入切换"},   {'o', 'o', "输出切换"},
    {'u', 'u', "USB输入"},    {'t', 't', "光纤输入"},
    {'7', '7', "耳放输出"},   {'8', '8', "LO输出"},
    {'9', '9', "全部输出"},
};
constexpr size_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);

constexpr MediaAction kMediaActions[] = {
    {'p', KEY_MEDIA_PLAY_PAUSE, "播放/暂停"},
    {'n', KEY_MEDIA_NEXT_TRACK, "下一曲"},
    {'b', KEY_MEDIA_PREVIOUS_TRACK, "上一曲"},
};
constexpr size_t kMediaActionCount = sizeof(kMediaActions) / sizeof(kMediaActions[0]);

constexpr uint16_t kSleepPresetsSec[] = {10, 30, 60, 120, 300, 600};
constexpr size_t kSleepPresetCount = sizeof(kSleepPresetsSec) / sizeof(kSleepPresetsSec[0]);

constexpr uint8_t kBrightnessPresets[] = {18, 32, 50, 75, 100};
constexpr size_t kBrightnessPresetCount = sizeof(kBrightnessPresets) / sizeof(kBrightnessPresets[0]);

constexpr bool kUseDisplayControllerSleep = false;
constexpr uint32_t kBatteryPollAwakeMs = 30000;
constexpr uint32_t kBatteryPollScreenOffMs = 120000;
constexpr uint32_t kSettingsWriteDelayMs = 2500;
constexpr uint32_t kUiMinRedrawIntervalMs = 30;
constexpr uint32_t kPeerAddressDelayMs = 250;

constexpr uint16_t kComboHoldMs = 14;
constexpr uint16_t kInterHidEventGapMs = 8;
constexpr size_t kHidQueueCapacity = 16;

constexpr uint32_t kVolumeRepeatInitialDelayMs = 450;
constexpr uint32_t kVolumeRepeatNormalMs = 180;
constexpr uint32_t kVolumeRepeatFastAfterMs = 2200;
constexpr uint32_t kVolumeRepeatFastMs = 90;

// BLE advertising units are 0.625 ms. 160..240 = 100..150 ms.
constexpr uint16_t kAdvertisingMinInterval = 160;
constexpr uint16_t kAdvertisingMaxInterval = 240;

constexpr uint32_t kEcoCpuMhz = 80;
constexpr uint32_t kResponsiveCpuMhz = 160;
constexpr esp_power_level_t kEcoBleTxPower = ESP_PWR_LVL_N3;   // -3 dBm
constexpr esp_power_level_t kResponsiveBleTxPower = ESP_PWR_LVL_P3;  // +3 dBm

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------

bool autoSleepEnabled = true;
uint8_t sleepPresetIndex = 1;
uint8_t brightnessPresetIndex = 1;
bool ecoModeEnabled = true;
bool wakeOnlyFirstKey = false;

bool screenOff = false;
uint8_t currentPage = 0;
uint32_t lastActivityMs = 0;

char lastActionText[96] = "等待按键...";
char peerAddressText[40] = "-";
bool lastConnectedState = false;
bool peerRefreshPending = false;
uint32_t peerRefreshDueMs = 0;

int32_t batteryLevel = -1;
bool batteryCharging = false;
uint32_t lastBatteryPollMs = 0;

bool settingsDirty = false;
uint32_t settingsDirtySinceMs = 0;

bool uiDirty = true;
uint32_t lastUiDrawMs = 0;

bool volDownHeld = false;
bool volUpHeld = false;
uint32_t volDownHoldStartMs = 0;
uint32_t volUpHoldStartMs = 0;
uint32_t lastVolDownRepeatMs = 0;
uint32_t lastVolUpRepeatMs = 0;

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

bool timeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

char normalizeKey(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
  return c;
}

void setLastAction(const char* text) {
  snprintf(lastActionText, sizeof(lastActionText), "%s", text ? text : "");
  uiDirty = true;
}

void setLastActionf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(lastActionText, sizeof(lastActionText), format, args);
  va_end(args);
  uiDirty = true;
}

uint8_t currentBrightness() {
  if (brightnessPresetIndex >= kBrightnessPresetCount) brightnessPresetIndex = 1;
  return kBrightnessPresets[brightnessPresetIndex];
}

uint8_t effectiveBrightness() {
  uint8_t brightness = currentBrightness();
  // A gentle emergency dim preserves usability while extending the final minutes.
  if (!batteryCharging && batteryLevel >= 0 && batteryLevel <= 10) {
    brightness = brightness < 18 ? brightness : 18;
  }
  return brightness;
}

uint32_t batteryPollIntervalMs() {
  return screenOff ? kBatteryPollScreenOffMs : kBatteryPollAwakeMs;
}

void markSettingsDirty(uint32_t now) {
  settingsDirty = true;
  settingsDirtySinceMs = now;
}

void applyDisplayBrightness() {
  if (!screenOff) M5Cardputer.Display.setBrightness(effectiveBrightness());
}

void applyOperatingMode() {
  setCpuFrequencyMhz(ecoModeEnabled ? kEcoCpuMhz : kResponsiveCpuMhz);
  NimBLEDevice::setPower(ecoModeEnabled ? kEcoBleTxPower : kResponsiveBleTxPower);
}

void configureAdvertisingForBatteryLife() {
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  if (!advertising) return;
  advertising->setMinInterval(kAdvertisingMinInterval);
  advertising->setMaxInterval(kAdvertisingMaxInterval);
}

void applyPowerSavingConfigBeforeBle() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  setCpuFrequencyMhz(ecoModeEnabled ? kEcoCpuMhz : kResponsiveCpuMhz);
}

// -----------------------------------------------------------------------------
// Non-blocking HID transmission queue
// -----------------------------------------------------------------------------

enum class HidEventType : uint8_t { Combo, Media };

struct HidEvent {
  HidEventType type;
  char comboKey;
  const uint8_t* mediaKey;
};

HidEvent hidQueue[kHidQueueCapacity];
size_t hidQueueHead = 0;
size_t hidQueueTail = 0;
size_t hidQueueCount = 0;

enum class HidTxPhase : uint8_t { Idle, ComboReleasePending };
HidTxPhase hidTxPhase = HidTxPhase::Idle;
uint32_t hidTxDeadlineMs = 0;
uint32_t nextHidStartMs = 0;

bool enqueueHidEvent(const HidEvent& event) {
  if (hidQueueCount >= kHidQueueCapacity) {
    setLastAction("发送队列已满，请稍后重试");
    return false;
  }
  hidQueue[hidQueueTail] = event;
  hidQueueTail = (hidQueueTail + 1) % kHidQueueCapacity;
  ++hidQueueCount;
  return true;
}

bool dequeueHidEvent(HidEvent& event) {
  if (hidQueueCount == 0) return false;
  event = hidQueue[hidQueueHead];
  hidQueueHead = (hidQueueHead + 1) % kHidQueueCapacity;
  --hidQueueCount;
  return true;
}

void clearHidQueue() {
  hidQueueHead = 0;
  hidQueueTail = 0;
  hidQueueCount = 0;
  hidTxPhase = HidTxPhase::Idle;
  hidTxDeadlineMs = 0;
  nextHidStartMs = 0;
}

void serviceHidTx(uint32_t now, bool connected) {
  if (!connected) {
    if (hidTxPhase == HidTxPhase::ComboReleasePending) bleKeyboard.releaseAll();
    clearHidQueue();
    return;
  }

  if (hidTxPhase == HidTxPhase::ComboReleasePending) {
    if (!timeReached(now, hidTxDeadlineMs)) return;
    bleKeyboard.releaseAll();
    hidTxPhase = HidTxPhase::Idle;
    nextHidStartMs = now + kInterHidEventGapMs;
  }

  if (hidTxPhase != HidTxPhase::Idle || !timeReached(now, nextHidStartMs)) return;

  HidEvent event{};
  if (!dequeueHidEvent(event)) return;

  if (event.type == HidEventType::Combo) {
    bleKeyboard.press(KEY_LEFT_CTRL);
    bleKeyboard.press(KEY_LEFT_ALT);
    bleKeyboard.press(event.comboKey);
    hidTxPhase = HidTxPhase::ComboReleasePending;
    hidTxDeadlineMs = now + kComboHoldMs;
  } else if (event.mediaKey) {
    bleKeyboard.write(event.mediaKey);
    nextHidStartMs = now + kInterHidEventGapMs;
  }
}

bool enqueueCombo(char key) {
  return enqueueHidEvent({HidEventType::Combo, key, nullptr});
}

bool enqueueMedia(const uint8_t* mediaKey) {
  return enqueueHidEvent({HidEventType::Media, 0, mediaKey});
}

// -----------------------------------------------------------------------------
// Battery and BLE status
// -----------------------------------------------------------------------------

void pushBleBatteryLevel() {
  if (batteryLevel < 0) return;
  bleKeyboard.setBatteryLevel(static_cast<uint8_t>(constrain(static_cast<int>(batteryLevel), 0, 100)));
}

bool refreshBatteryStatus() {
  int32_t level = M5Cardputer.Power.getBatteryLevel();
  const bool charging = (M5Cardputer.Power.isCharging() == 1);
  if (level >= 0) level = constrain(static_cast<int>(level), 0, 100);

  const bool changed = (level != batteryLevel) || (charging != batteryCharging);
  batteryLevel = level;
  batteryCharging = charging;

  if (changed) {
    pushBleBatteryLevel();
    applyDisplayBrightness();
    uiDirty = true;
  }
  return changed;
}

void updatePeerAddress() {
  snprintf(peerAddressText, sizeof(peerAddressText), "-");
  NimBLEServer* server = NimBLEDevice::getServer();
  if (!server || server->getConnectedCount() == 0) return;

  const NimBLEConnInfo info = server->getPeerInfo(0);
  snprintf(peerAddressText, sizeof(peerAddressText), "%s", const_cast<NimBLEConnInfo&>(info).getAddress().toString().c_str());
}

// -----------------------------------------------------------------------------
// Display
// -----------------------------------------------------------------------------

void wakeScreen() {
  if (!screenOff) return;
  screenOff = false;
  if (kUseDisplayControllerSleep) M5Cardputer.Display.wakeup();
  M5Cardputer.Display.setBrightness(effectiveBrightness());
  uiDirty = true;
}

void turnScreenOff() {
  if (screenOff) return;
  screenOff = true;
  M5Cardputer.Display.setBrightness(0);
  if (kUseDisplayControllerSleep) M5Cardputer.Display.sleep();
}

void drawBatteryIcon(int xRight, int y, int level, bool charging) {
  auto& lcd = M5Cardputer.Display;
  constexpr int w = 22;
  constexpr int h = 11;
  constexpr int nub = 4;
  const int x = xRight - w - 3;
  constexpr uint16_t frameColor = 0xC618;

  lcd.drawRect(x, y, w, h, frameColor);
  lcd.fillRect(x + w, y + (h - nub) / 2, 2, nub, frameColor);
  lcd.fillRect(x + 2, y + 2, w - 4, h - 4, BLACK);

  if (level < 0) {
    lcd.setFont(&fonts::efontCN_12);
    lcd.setTextColor(0x7BEF, BLACK);
    lcd.setCursor(x + w / 2 - 3, y - 1);
    lcd.print("?");
    return;
  }

  const uint16_t fillColor = level <= 15 ? RED : (level <= 40 ? YELLOW : GREEN);
  const int fillW = ((w - 4) * level) / 100;
  if (fillW > 0) lcd.fillRect(x + 2, y + 2, fillW, h - 4, fillColor);

  if (charging) {
    lcd.setFont(&fonts::efontCN_12);
    lcd.setTextColor(YELLOW, BLACK);
    lcd.setCursor(x - 12, y - 1);
    lcd.print("+");
  }
}

void drawStatusPage() {
  auto& lcd = M5Cardputer.Display;
  int y = 2;

  lcd.setFont(&fonts::efontCN_16);
  lcd.setTextColor(WHITE, BLACK);
  lcd.setCursor(4, y);
  lcd.print("DX1 II 蓝牙遥控");
  drawBatteryIcon(lcd.width() - 4, y + 2, batteryLevel, batteryCharging);

  y += 20;
  lcd.setFont(&fonts::efontCN_14);
  const bool connected = bleKeyboard.isConnected();
  lcd.setTextColor(connected ? GREEN : RED, BLACK);
  lcd.setCursor(4, y);
  lcd.print(connected ? "蓝牙: 已连接" : "蓝牙: 等待PC连接...");

  char batteryText[32];
  if (batteryLevel < 0) {
    snprintf(batteryText, sizeof(batteryText), "电量: --");
  } else {
    snprintf(batteryText, sizeof(batteryText), "电量: %ld%%%s", static_cast<long>(batteryLevel),
             batteryCharging ? " 充电" : "");
  }
  lcd.setTextColor(0xC618, BLACK);
  lcd.setCursor(lcd.width() - lcd.textWidth(batteryText) - 4, y);
  lcd.print(batteryText);

  y += 17;
  lcd.setCursor(4, y);
  lcd.printf("设备地址: %s", connected ? peerAddressText : "-");

  y += 17;
  lcd.setCursor(4, y);
  lcd.printf("熄屏: %s %us  亮度:%u", autoSleepEnabled ? "开" : "关",
             kSleepPresetsSec[sleepPresetIndex], currentBrightness());

  y += 17;
  lcd.setCursor(4, y);
  lcd.printf("模式: %s  唤醒键:%s", ecoModeEnabled ? "省电" : "响应",
             wakeOnlyFirstKey ? "仅唤醒" : "执行");

  y += 19;
  lcd.drawFastHLine(0, y, lcd.width(), 0x39C7);
  y += 5;

  const bool lowBattery = batteryLevel >= 0 && batteryLevel <= 10 && !batteryCharging;
  lcd.setTextColor(lowBattery ? RED : YELLOW, BLACK);
  lcd.setCursor(4, y);
  lcd.print(lowBattery ? "低电量：背光受限" : lastActionText);

  lcd.setFont(&fonts::efontCN_12);
  lcd.setTextColor(0x7BEF, BLACK);
  lcd.setCursor(4, lcd.height() - 14);
  lcd.print("Fn+/ 翻页  Fn+E 省电模式");
}

void drawLegendPage() {
  auto& lcd = M5Cardputer.Display;
  constexpr int colX[2] = {4, 123};
  int y = 2;

  lcd.setFont(&fonts::efontCN_14);
  lcd.setTextColor(WHITE, BLACK);
  lcd.setCursor(4, y);
  lcd.print("控制按键 (Fn+/ 翻页)");
  drawBatteryIcon(lcd.width() - 4, y + 2, batteryLevel, batteryCharging);

  y += 20;
  lcd.setFont(&fonts::efontCN_12);
  lcd.setTextColor(0xC618, BLACK);

  constexpr const char* rows[][2] = {
      {"C/X 连接/断开", "1/2 音量-/+"},
      {"3 音量预设", "0/R 安全/恢复"},
      {"M 静音", "I/O 输入/输出"},
      {"U/T USB/光纤", "7/8/9 三种输出"},
      {"P 播放暂停", "N/B 下一/上一曲"},
      {"Fn+S 熄屏开关", "Fn+,/. 熄屏时长"},
      {"Fn+L 亮度", "Fn+E/W 模式/唤醒"},
  };

  for (const auto& row : rows) {
    lcd.setCursor(colX[0], y);
    lcd.print(row[0]);
    lcd.setCursor(colX[1], y);
    lcd.print(row[1]);
    y += 15;
  }
}

void drawDiagnosticsPage() {
  auto& lcd = M5Cardputer.Display;
  int y = 2;

  lcd.setFont(&fonts::efontCN_14);
  lcd.setTextColor(WHITE, BLACK);
  lcd.setCursor(4, y);
  lcd.print("运行诊断 (Fn+/ 返回)");
  drawBatteryIcon(lcd.width() - 4, y + 2, batteryLevel, batteryCharging);

  y += 22;
  lcd.setFont(&fonts::efontCN_12);
  lcd.setTextColor(0xC618, BLACK);

  const uint32_t uptimeSec = millis() / 1000UL;
  lcd.setCursor(4, y);
  lcd.printf("运行: %luh %02lum %02lus", static_cast<unsigned long>(uptimeSec / 3600UL),
             static_cast<unsigned long>((uptimeSec / 60UL) % 60UL),
             static_cast<unsigned long>(uptimeSec % 60UL));
  y += 16;

  lcd.setCursor(4, y);
  lcd.printf("CPU: %uMHz  空闲堆: %uKB", getCpuFrequencyMhz(), ESP.getFreeHeap() / 1024U);
  y += 16;

  lcd.setCursor(4, y);
  lcd.printf("BLE功率: %s  队列: %u/%u", ecoModeEnabled ? "-3dBm" : "+3dBm",
             static_cast<unsigned>(hidQueueCount), static_cast<unsigned>(kHidQueueCapacity));
  y += 16;

  lcd.setCursor(4, y);
  lcd.printf("屏幕: %s  NVS待写: %s", screenOff ? "关" : "开", settingsDirty ? "是" : "否");
  y += 16;

  lcd.setCursor(4, y);
  lcd.printf("电量轮询: %lus", static_cast<unsigned long>(batteryPollIntervalMs() / 1000UL));
  y += 16;

  lcd.setTextColor(0x7BEF, BLACK);
  lcd.setCursor(4, y + 4);
  lcd.print("固定内存队列 / 非阻塞组合键");
}

void drawUI() {
  if (screenOff) return;
  auto& lcd = M5Cardputer.Display;

  lcd.startWrite();
  lcd.fillScreen(BLACK);
  lcd.setTextSize(1);
  if (currentPage == 0) {
    drawStatusPage();
  } else if (currentPage == 1) {
    drawLegendPage();
  } else {
    drawDiagnosticsPage();
  }
  lcd.endWrite();
}

void serviceUi(uint32_t now) {
  if (!uiDirty || screenOff) return;
  if ((now - lastUiDrawMs) < kUiMinRedrawIntervalMs) return;
  drawUI();
  lastUiDrawMs = now;
  uiDirty = false;
}

// -----------------------------------------------------------------------------
// Input handling
// -----------------------------------------------------------------------------

bool handleDx1Key(char c) {
  c = normalizeKey(c);
  for (size_t i = 0; i < kActionCount; ++i) {
    if (c != kActions[i].key) continue;
    if (!bleKeyboard.isConnected()) {
      setLastAction("未连接蓝牙，无法发送");
    } else if (enqueueCombo(kActions[i].sendChar)) {
      setLastActionf("已发送: %s", kActions[i].label);
    }
    return true;
  }
  return false;
}

bool handleMediaKey(char c) {
  c = normalizeKey(c);
  for (size_t i = 0; i < kMediaActionCount; ++i) {
    if (c != kMediaActions[i].key) continue;
    if (!bleKeyboard.isConnected()) {
      setLastAction("未连接蓝牙，无法发送");
    } else if (enqueueMedia(kMediaActions[i].mediaKey)) {
      setLastActionf("媒体: %s", kMediaActions[i].label);
    }
    return true;
  }
  return false;
}

bool handleSettingsKey(char c, uint32_t now) {
  c = normalizeKey(c);

  if (c == 's') {
    autoSleepEnabled = !autoSleepEnabled;
    markSettingsDirty(now);
    setLastAction(autoSleepEnabled ? "自动熄屏: 开" : "自动熄屏: 关");
    return true;
  }

  if (c == ',') {
    sleepPresetIndex = sleepPresetIndex == 0 ? kSleepPresetCount - 1 : sleepPresetIndex - 1;
    markSettingsDirty(now);
    setLastActionf("熄屏时间: %us", kSleepPresetsSec[sleepPresetIndex]);
    return true;
  }

  if (c == '.') {
    sleepPresetIndex = (sleepPresetIndex + 1) % kSleepPresetCount;
    markSettingsDirty(now);
    setLastActionf("熄屏时间: %us", kSleepPresetsSec[sleepPresetIndex]);
    return true;
  }

  if (c == '/') {
    currentPage = (currentPage + 1) % 3;
    setLastAction(currentPage == 0 ? "状态页" : (currentPage == 1 ? "按键页" : "诊断页"));
    return true;
  }

  if (c == 'l') {
    brightnessPresetIndex = (brightnessPresetIndex + 1) % kBrightnessPresetCount;
    applyDisplayBrightness();
    markSettingsDirty(now);
    setLastActionf("屏幕亮度: %u", currentBrightness());
    return true;
  }

  if (c == 'e') {
    ecoModeEnabled = !ecoModeEnabled;
    applyOperatingMode();
    markSettingsDirty(now);
    setLastAction(ecoModeEnabled ? "运行模式: 省电" : "运行模式: 高响应");
    return true;
  }

  if (c == 'w') {
    wakeOnlyFirstKey = !wakeOnlyFirstKey;
    markSettingsDirty(now);
    setLastAction(wakeOnlyFirstKey ? "熄屏首键: 仅唤醒" : "熄屏首键: 唤醒并执行");
    return true;
  }

  return false;
}

bool handleFnSettingsPhysical(uint32_t now) {
  if (!M5Cardputer.Keyboard.isKeyPressed(KEY_FN)) return false;

  constexpr char settingsKeys[] = {'s', ',', '.', '/', 'l', 'e', 'w'};
  for (char key : settingsKeys) {
    if (M5Cardputer.Keyboard.isKeyPressed(key) ||
        ((key >= 'a' && key <= 'z') && M5Cardputer.Keyboard.isKeyPressed(key - 'a' + 'A'))) {
      return handleSettingsKey(key, now);
    }
  }
  return false;
}

void beginVolumeHold(char key, uint32_t now) {
  if (key == '1') {
    volDownHeld = true;
    volDownHoldStartMs = now;
    lastVolDownRepeatMs = now;
  } else if (key == '2') {
    volUpHeld = true;
    volUpHoldStartMs = now;
    lastVolUpRepeatMs = now;
  }
}

void serviceVolumeRepeat(uint32_t now, bool connected) {
  const bool fnHeld = M5Cardputer.Keyboard.isKeyPressed(KEY_FN);
  const bool downPhysical = !fnHeld && M5Cardputer.Keyboard.isKeyPressed('1');
  const bool upPhysical = !fnHeld && M5Cardputer.Keyboard.isKeyPressed('2');

  if (!downPhysical) volDownHeld = false;
  if (!upPhysical) volUpHeld = false;
  if (!connected) return;

  if (volDownHeld && downPhysical && (now - volDownHoldStartMs) >= kVolumeRepeatInitialDelayMs) {
    const uint32_t interval = (now - volDownHoldStartMs) >= kVolumeRepeatFastAfterMs
                                  ? kVolumeRepeatFastMs
                                  : kVolumeRepeatNormalMs;
    if ((now - lastVolDownRepeatMs) >= interval && enqueueCombo('1')) {
      lastVolDownRepeatMs = now;
      lastActivityMs = now;
      setLastAction("长按: 音量-");
    }
  }

  if (volUpHeld && upPhysical && (now - volUpHoldStartMs) >= kVolumeRepeatInitialDelayMs) {
    const uint32_t interval = (now - volUpHoldStartMs) >= kVolumeRepeatFastAfterMs
                                  ? kVolumeRepeatFastMs
                                  : kVolumeRepeatNormalMs;
    if ((now - lastVolUpRepeatMs) >= interval && enqueueCombo('2')) {
      lastVolUpRepeatMs = now;
      lastActivityMs = now;
      setLastAction("长按: 音量+");
    }
  }
}

// -----------------------------------------------------------------------------
// Deferred services
// -----------------------------------------------------------------------------

void flushSettingsIfDue(uint32_t now) {
  if (!settingsDirty || (now - settingsDirtySinceMs) < kSettingsWriteDelayMs) return;

  prefs.putBool("autoSleep", autoSleepEnabled);
  prefs.putUChar("sleepIdx", sleepPresetIndex);
  prefs.putUChar("brightIdx", brightnessPresetIndex);
  prefs.putBool("ecoMode", ecoModeEnabled);
  prefs.putBool("wakeOnly", wakeOnlyFirstKey);
  settingsDirty = false;
  uiDirty = true;
}

uint16_t loopDelayMs(bool connected) {
  // While a key is being transmitted, poll quickly enough to release it on time.
  if (hidTxPhase != HidTxPhase::Idle || hidQueueCount > 0) return 2;

  if (screenOff) {
    if (ecoModeEnabled) return connected ? 18 : 35;
    return connected ? 10 : 20;
  }
  if (ecoModeEnabled) return connected ? 7 : 14;
  return connected ? 4 : 8;
}

}  // namespace

// -----------------------------------------------------------------------------
// Arduino lifecycle
// -----------------------------------------------------------------------------

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.internal_imu = false;
  cfg.external_speaker_value = 0;
  cfg.external_display_value = 0;
  cfg.led_brightness = 0;

  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);

  prefs.begin("dx1remote", false);
  autoSleepEnabled = prefs.getBool("autoSleep", true);
  sleepPresetIndex = prefs.getUChar("sleepIdx", 1);
  brightnessPresetIndex = prefs.getUChar("brightIdx", 1);
  ecoModeEnabled = prefs.getBool("ecoMode", true);
  wakeOnlyFirstKey = prefs.getBool("wakeOnly", false);

  if (sleepPresetIndex >= kSleepPresetCount) sleepPresetIndex = 1;
  if (brightnessPresetIndex >= kBrightnessPresetCount) brightnessPresetIndex = 1;

  M5Cardputer.Display.setBrightness(currentBrightness());
  M5Cardputer.Speaker.end();
  M5Cardputer.Mic.end();

  applyPowerSavingConfigBeforeBle();
  bleKeyboard.begin();
  applyOperatingMode();
  configureAdvertisingForBatteryLife();

  refreshBatteryStatus();
  pushBleBatteryLevel();

  const uint32_t now = millis();
  lastBatteryPollMs = now;
  lastActivityMs = now;
  lastUiDrawMs = now;
  drawUI();
  uiDirty = false;
}

void loop() {
  M5Cardputer.update();
  const uint32_t now = millis();
  const bool connected = bleKeyboard.isConnected();

  serviceHidTx(now, connected);
  serviceVolumeRepeat(now, connected);

  if (connected != lastConnectedState) {
    lastConnectedState = connected;
    volDownHeld = false;
    volUpHeld = false;

    if (connected) {
      applyOperatingMode();
      pushBleBatteryLevel();
      peerRefreshPending = true;
      peerRefreshDueMs = now + kPeerAddressDelayMs;
      setLastAction("蓝牙已连接");
    } else {
      snprintf(peerAddressText, sizeof(peerAddressText), "-");
      peerRefreshPending = false;
      clearHidQueue();
      setLastAction("蓝牙已断开");
    }

    wakeScreen();
    lastActivityMs = now;
    uiDirty = true;
  }

  if (peerRefreshPending && connected && timeReached(now, peerRefreshDueMs)) {
    peerRefreshPending = false;
    updatePeerAddress();
    uiDirty = true;
  }

  if ((now - lastBatteryPollMs) >= batteryPollIntervalMs()) {
    lastBatteryPollMs = now;
    refreshBatteryStatus();
  }

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    const Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    const bool wasScreenOff = screenOff;
    wakeScreen();
    lastActivityMs = now;

    // Optional accidental-press guard: the first key only wakes the display.
    if (!(wasScreenOff && wakeOnlyFirstKey)) {
      bool handled = false;
      const bool fnHeld = M5Cardputer.Keyboard.isKeyPressed(KEY_FN) || status.fn;

      if (fnHeld) {
        handled = handleFnSettingsPhysical(now);
      } else {
        for (char rawKey : status.word) {
          const char key = normalizeKey(rawKey);
          if (handleDx1Key(key)) {
            handled = true;
            if ((key == '1' || key == '2') && connected) beginVolumeHold(key, now);
            continue;
          }
          if (handleMediaKey(key)) handled = true;
        }
      }

      if (!handled && wasScreenOff) setLastAction("屏幕已唤醒");
    } else {
      setLastAction("屏幕已唤醒，首键未发送");
    }

    uiDirty = true;
  }

  if (autoSleepEnabled && !screenOff) {
    const uint32_t timeoutMs = static_cast<uint32_t>(kSleepPresetsSec[sleepPresetIndex]) * 1000UL;
    if ((now - lastActivityMs) >= timeoutMs) turnScreenOff();
  }

  flushSettingsIfDue(now);
  serviceUi(now);
  delay(loopDelayMs(connected));
}
