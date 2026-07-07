/*
  DX1 II Bluetooth Remote for M5Stack Cardputer-Adv
  Power-optimized revision

  Main changes:
  1. Force ESP32-BLE-Keyboard to use NimBLE mode.
  2. Fix BLE TX power setting: use dBm value for NimBLEDevice::setPower().
  3. Disable unused M5 internal peripherals: speaker, mic, IMU.
  4. Keep WiFi fully off.
  5. Lower default brightness and add Fn+L brightness presets.
  6. Report real battery level to BLE HID Battery Service.
  7. Use longer battery polling while screen is off.
  8. Wake screen on any key press, but only redraw when needed.
*/

#include <M5Cardputer.h>

// --- 在引入 BleKeyboard 之前，取消所有冲突的宏定义 ---
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
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>

BleKeyboard bleKeyboard("DX1II Remote", "M5Stack", 100);
Preferences prefs;

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

// Keep these in sync with dx1_bridge's HOTKEY_ACTIONS map.
static const KeyAction kActions[] = {
  {'c', 'c', "连接"},
  {'x', 'x', "断开"},
  {'1', '1', "音量-"},
  {'2', '2', "音量+"},
  {'m', 'm', "静音切换"},
  {'i', 'i', "输入切换"},
  {'o', 'o', "输出切换"},
};
static const size_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);

static const MediaAction kMediaActions[] = {
  {'p', KEY_MEDIA_PLAY_PAUSE, "播放/暂停"},
  {'n', KEY_MEDIA_NEXT_TRACK, "下一曲"},
  {'b', KEY_MEDIA_PREVIOUS_TRACK, "上一曲"},
};
static const size_t kMediaActionCount = sizeof(kMediaActions) / sizeof(kMediaActions[0]);

// Auto screen-off timeout presets, in seconds. Cycle with Fn+, / Fn+.
static const uint16_t kSleepPresetsSec[] = {10, 30, 60, 120, 300};
static const size_t kSleepPresetCount = sizeof(kSleepPresetsSec) / sizeof(kSleepPresetsSec[0]);

// 新增：亮度档位。默认使用 45，续航优先；需要更亮可 Fn+L 切换。
static const uint8_t kBrightnessPresets[] = {25, 45, 70, 100};
static const size_t kBrightnessPresetCount = sizeof(kBrightnessPresets) / sizeof(kBrightnessPresets[0]);

static bool autoSleepEnabled = true;
static uint8_t sleepPresetIndex = 1;
static uint8_t brightnessPresetIndex = 1;
static bool screenOff = false;
static unsigned long lastActivityMs = 0;

// 如果你实测 Display.sleep()/wakeup() 稳定，可改成 true 进一步降低屏幕控制器功耗。
// 默认 false：只关背光，稳定性最好。
static const bool kUseDisplaySleep = false;

// 0 = status page, 1 = key legend page
static uint8_t currentPage = 0;

static String lastActionText = "等待按键...";
static bool lastConnectedState = false;
static String peerAddressText = "";

// ESP32-S3 降频到 80 MHz。继续降低到 40 MHz 可能影响 BLE/I2C/SPI 响应。
static const uint32_t kCpuFrequencyMhz = 80;

// NimBLEDevice::setPower() 严格要求传入 esp_power_level_t 枚举类型，而不是普通的 int8_t 整数
static const esp_power_level_t kBleTxPowerDbm = ESP_PWR_LVL_P3;

// Battery state
static int32_t batteryLevel = -1;
static bool batteryCharging = false;
static unsigned long lastBatteryPollMs = 0;

// 亮屏时 30 秒；熄屏时 120 秒。熄屏时电量显示不可见，没有必要频繁读取。
static const unsigned long kBatteryPollAwakeMs = 30000;
static const unsigned long kBatteryPollScreenOffMs = 120000;

// 自适应 loop 延时。熄屏时适当放慢，但不要太大，避免键盘 FIFO 在连续按键时堆积。
static const uint16_t kLoopDelayConnectedMs = 8;
static const uint16_t kLoopDelayDisconnectedMs = 15;
static const uint16_t kLoopDelayScreenOffConnectedMs = 18;
static const uint16_t kLoopDelayScreenOffDisconnectedMs = 30;

// ---------- 音量长按连续调节相关变量 ----------
static bool volDownHeld = false;
static bool volUpHeld = false;
static unsigned long lastVolDownMs = 0;
static unsigned long lastVolUpMs = 0;
static const unsigned long kVolRepeatIntervalMs = 400; // 重复发送间隔 ms
// --------------------------------------------

uint8_t currentBrightness() {
  if (brightnessPresetIndex >= kBrightnessPresetCount) {
    brightnessPresetIndex = 1;
  }
  return kBrightnessPresets[brightnessPresetIndex];
}

void applyPowerSavingConfigBeforeBle() {
  // 本固件不使用 WiFi。disconnect + WIFI_OFF + esp_wifi_stop 都执行，忽略未初始化时的返回错误。
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();

  // 降低 CPU 频率。
  setCpuFrequencyMhz(kCpuFrequencyMhz);
}

void applyPowerSavingConfigAfterBleBegin() {
  // NimBLE 当前 API 使用 dBm 数值。-3 dBm 适合近距离桌面遥控。
  // 若连接不稳，改成 0 或 3。
  NimBLEDevice::setPower(kBleTxPowerDbm);
}

uint16_t getLoopDelayMs(bool connected) {
  if (screenOff) {
    return connected ? kLoopDelayScreenOffConnectedMs : kLoopDelayScreenOffDisconnectedMs;
  }
  return connected ? kLoopDelayConnectedMs : kLoopDelayDisconnectedMs;
}

unsigned long getBatteryPollIntervalMs() {
  return screenOff ? kBatteryPollScreenOffMs : kBatteryPollAwakeMs;
}

void pushBleBatteryLevel() {
  if (batteryLevel < 0) return;

  uint8_t level = (uint8_t)constrain((int)batteryLevel, 0, 100);
  bleKeyboard.setBatteryLevel(level);
}

void updatePeerAddress() {
  peerAddressText = "";

  NimBLEServer* server = NimBLEDevice::getServer();
  if (!server || server->getConnectedCount() == 0) return;

  NimBLEConnInfo info = server->getPeerInfo(0);
  peerAddressText = String(info.getAddress().toString().c_str());
}

bool refreshBatteryStatus() {
  int32_t level = M5Cardputer.Power.getBatteryLevel();
  int8_t chg = M5Cardputer.Power.isCharging();
  bool charging = (chg == 1);

  if (level >= 0) {
    level = constrain((int)level, 0, 100);
  }

  bool changed = (level != batteryLevel) || (charging != batteryCharging);
  batteryLevel = level;
  batteryCharging = charging;

  if (changed) {
    pushBleBatteryLevel();
  }

  return changed;
}

void drawBatteryIcon(int xRight, int y, int level, bool charging) {
  auto &lcd = M5Cardputer.Display;
  const int w = 22, h = 11, nub = 4;
  const int x = xRight - w - 3;

  const uint16_t frameColor = 0xC618;

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

  uint16_t fillColor;
  if (level <= 15) {
    fillColor = RED;
  } else if (level <= 40) {
    fillColor = YELLOW;
  } else {
    fillColor = GREEN;
  }

  int innerW = w - 4;
  int fillW = (innerW * level) / 100;
  if (fillW > 0) {
    lcd.fillRect(x + 2, y + 2, fillW, h - 4, fillColor);
  }

  if (charging) {
    lcd.setFont(&fonts::efontCN_12);
    lcd.setTextColor(YELLOW, BLACK);
    lcd.setCursor(x - 12, y - 1);
    lcd.print("+");
  }
}

void wakeScreen() {
  if (!screenOff) return;

  screenOff = false;

  if (kUseDisplaySleep) {
    M5Cardputer.Display.wakeup();
  }

  M5Cardputer.Display.setBrightness(currentBrightness());
}

void turnScreenOff() {
  if (screenOff) return;

  screenOff = true;
  M5Cardputer.Display.setBrightness(0);

  if (kUseDisplaySleep) {
    M5Cardputer.Display.sleep();
  }
}

void drawStatusPage() {
  auto &lcd = M5Cardputer.Display;
  int y = 2;

  lcd.setFont(&fonts::efontCN_16);
  lcd.setTextColor(WHITE, BLACK);
  lcd.setCursor(4, y);
  lcd.print("DX1 II 蓝牙遥控");

  drawBatteryIcon(lcd.width() - 4, y + 2, batteryLevel, batteryCharging);

  y += 20;

  lcd.setFont(&fonts::efontCN_14);
  bool connected = bleKeyboard.isConnected();

  lcd.setTextColor(connected ? GREEN : RED, BLACK);
  lcd.setCursor(4, y);
  lcd.print(connected ? "蓝牙: 已连接" : "蓝牙: 等待PC连接...");

  lcd.setTextColor(0xC618, BLACK);
  char battText[24];
  if (batteryLevel < 0) {
    snprintf(battText, sizeof(battText), "电量: --");
  } else {
    snprintf(battText, sizeof(battText), "电量: %ld%%%s",
             (long)batteryLevel, batteryCharging ? " 充电中" : "");
  }

  int battTextW = lcd.textWidth(battText);
  lcd.setCursor(lcd.width() - battTextW - 4, y);
  lcd.print(battText);

  y += 17;

  lcd.setTextColor(0xC618, BLACK);
  lcd.setCursor(4, y);

  if (connected && peerAddressText.length() > 0) {
    lcd.print("设备地址: " + peerAddressText);
  } else {
    lcd.print("设备地址: -");
  }

  y += 17;

  lcd.setCursor(4, y);
  lcd.printf("自动熄屏: %s (%us)",
             autoSleepEnabled ? "开" : "关",
             kSleepPresetsSec[sleepPresetIndex]);

  y += 17;

  lcd.setCursor(4, y);
  lcd.printf("亮度: %u / 100", currentBrightness());

  y += 20;

  lcd.drawFastHLine(0, y, lcd.width(), 0x39C7);
  y += 6;

  if (batteryLevel >= 0 && batteryLevel <= 15 && !batteryCharging) {
    lcd.setTextColor(RED, BLACK);
    lcd.setCursor(4, y);
    lcd.print("低电量，请尽快充电");
    y += 16;
  }

  lcd.setTextColor(YELLOW, BLACK);
  lcd.setCursor(4, y);
  lcd.print(lastActionText);

  lcd.setTextColor(0x7BEF, BLACK);
  lcd.setCursor(4, lcd.height() - 16);
  lcd.print("Fn+/ 说明  Fn+L 亮度");
}

void drawLegendPage() {
  auto &lcd = M5Cardputer.Display;
  const int colX[2] = {4, 128};
  int y = 2;

  lcd.setFont(&fonts::efontCN_14);
  lcd.setTextColor(WHITE, BLACK);
  lcd.setCursor(4, y);
  lcd.print("按键说明 (Fn+/ 返回)");

  drawBatteryIcon(lcd.width() - 4, y + 2, batteryLevel, batteryCharging);

  y += 20;

  lcd.setFont(&fonts::efontCN_12);
  lcd.setTextColor(0xC618, BLACK);

  const char* rows[7][2] = {
    {"C 连接", "X 断开"},
    {"1 音量-", "2 音量+"},
    {"M 静音", "I 输入"},
    {"O 输出", "P 播放暂停"},
    {"N 下一曲", "B 上一曲"},
    {"Fn+S 熄屏开关", "Fn+,/. 熄屏时长"},
    {"Fn+L 亮度档位", "Fn+/ 页面切换"},
  };

  for (int row = 0; row < 7; row++) {
    lcd.setCursor(colX[0], y);
    lcd.print(rows[row][0]);

    lcd.setCursor(colX[1], y);
    lcd.print(rows[row][1]);

    y += 15;
  }
}

void drawUI() {
  if (screenOff) return;

  auto &lcd = M5Cardputer.Display;

  lcd.startWrite();
  lcd.fillScreen(BLACK);
  lcd.setTextSize(1);

  if (currentPage == 0) {
    drawStatusPage();
  } else {
    drawLegendPage();
  }

  lcd.endWrite();
}

void sendComboKey(char c) {
  bleKeyboard.press(KEY_LEFT_CTRL);
  bleKeyboard.press(KEY_LEFT_ALT);
  bleKeyboard.press(c);

  // 保留短延时，保证主机端稳定识别组合键。
  delay(15);

  bleKeyboard.releaseAll();
}

bool handleDx1Key(char c) {
  if (c >= 'A' && c <= 'Z') {
    c = c - 'A' + 'a';
  }

  for (size_t i = 0; i < kActionCount; i++) {
    if (c != kActions[i].key) continue;

    if (!bleKeyboard.isConnected()) {
      lastActionText = "未连接蓝牙, 无法发送";
    } else {
      sendComboKey(kActions[i].sendChar);
      lastActionText = String("已发送: ") + kActions[i].label;
    }

    return true;
  }

  return false;
}

bool handleMediaKey(char c) {
  if (c >= 'A' && c <= 'Z') {
    c = c - 'A' + 'a';
  }

  for (size_t i = 0; i < kMediaActionCount; i++) {
    if (c != kMediaActions[i].key) continue;

    if (!bleKeyboard.isConnected()) {
      lastActionText = "未连接蓝牙, 无法发送";
    } else {
      bleKeyboard.write(kMediaActions[i].mediaKey);
      lastActionText = String("媒体: ") + kMediaActions[i].label;
    }

    return true;
  }

  return false;
}

bool handleSettingsKey(char c) {
  if (c >= 'A' && c <= 'Z') {
    c = c - 'A' + 'a';
  }

  if (c == 's') {
    autoSleepEnabled = !autoSleepEnabled;
    prefs.putBool("autoSleep", autoSleepEnabled);

    lastActionText = autoSleepEnabled ? "自动熄屏: 开" : "自动熄屏: 关";
    return true;
  }

  if (c == ',') {
    sleepPresetIndex = (sleepPresetIndex == 0)
                         ? (kSleepPresetCount - 1)
                         : (sleepPresetIndex - 1);

    prefs.putUChar("sleepIdx", sleepPresetIndex);
    lastActionText = String("熄屏时间: ") + kSleepPresetsSec[sleepPresetIndex] + "s";
    return true;
  }

  if (c == '.') {
    sleepPresetIndex = (sleepPresetIndex + 1) % kSleepPresetCount;

    prefs.putUChar("sleepIdx", sleepPresetIndex);
    lastActionText = String("熄屏时间: ") + kSleepPresetsSec[sleepPresetIndex] + "s";
    return true;
  }

  if (c == '/') {
    currentPage = currentPage == 0 ? 1 : 0;
    lastActionText = currentPage == 0 ? "已切换到状态页" : "已切换到按键说明页";
    return true;
  }

  if (c == 'l') {
    brightnessPresetIndex = (brightnessPresetIndex + 1) % kBrightnessPresetCount;
    prefs.putUChar("brightIdx", brightnessPresetIndex);

    if (!screenOff) {
      M5Cardputer.Display.setBrightness(currentBrightness());
    }

    lastActionText = String("屏幕亮度: ") + currentBrightness();
    return true;
  }

  return false;
}

/*
  Cardputer-Adv / newer M5Cardputer Fn-layer fix:
  不依赖 status.word 判断 Fn+/、Fn+,、Fn+.。
*/
bool handleFnSettingsPhysical() {
  if (!M5Cardputer.Keyboard.isKeyPressed(KEY_FN)) {
    return false;
  }

  if (M5Cardputer.Keyboard.isKeyPressed('s') ||
      M5Cardputer.Keyboard.isKeyPressed('S')) {
    return handleSettingsKey('s');
  }

  if (M5Cardputer.Keyboard.isKeyPressed(',')) {
    return handleSettingsKey(',');
  }

  if (M5Cardputer.Keyboard.isKeyPressed('.')) {
    return handleSettingsKey('.');
  }

  if (M5Cardputer.Keyboard.isKeyPressed('/')) {
    return handleSettingsKey('/');
  }

  if (M5Cardputer.Keyboard.isKeyPressed('l') ||
      M5Cardputer.Keyboard.isKeyPressed('L')) {
    return handleSettingsKey('l');
  }

  return false;
}

void setup() {
  auto cfg = M5.config();

  // 省电：不用的内部硬件直接不初始化。
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

  if (sleepPresetIndex >= kSleepPresetCount) {
    sleepPresetIndex = 1;
  }

  if (brightnessPresetIndex >= kBrightnessPresetCount) {
    brightnessPresetIndex = 1;
  }

  M5Cardputer.Display.setBrightness(currentBrightness());

  // 双保险：即使库内部默认初始化过，也主动关闭不用的音频任务。
  M5Cardputer.Speaker.end();
  M5Cardputer.Mic.end();

  applyPowerSavingConfigBeforeBle();

  bleKeyboard.begin();
  applyPowerSavingConfigAfterBleBegin();

  refreshBatteryStatus();
  pushBleBatteryLevel();
  lastBatteryPollMs = millis();

  lastActivityMs = millis();
  drawUI();
}

void loop() {
  M5Cardputer.update();

  unsigned long now = millis();
  bool connected = bleKeyboard.isConnected();

  // ---------- 长按音量键连续调节 ----------
  // 检测音量键是否被按住，实现连续发送组合键
  bool volDownNow = M5Cardputer.Keyboard.isKeyPressed('1');
  bool volUpNow = M5Cardputer.Keyboard.isKeyPressed('2');

  if (volDownNow) {
    if (volDownHeld && bleKeyboard.isConnected() && (now - lastVolDownMs >= kVolRepeatIntervalMs)) {
      sendComboKey('1');
      lastVolDownMs = now;
      lastActivityMs = now;  // 长按期间保持屏幕唤醒，避免自动熄屏
    }
  } else {
    volDownHeld = false;
  }

  if (volUpNow) {
    if (volUpHeld && bleKeyboard.isConnected() && (now - lastVolUpMs >= kVolRepeatIntervalMs)) {
      sendComboKey('2');
      lastVolUpMs = now;
      lastActivityMs = now;
    }
  } else {
    volUpHeld = false;
  }
  // ---------------------------------------

  if (connected != lastConnectedState) {
    lastConnectedState = connected;

    if (connected) {
      delay(150);
      applyPowerSavingConfigAfterBleBegin();
      updatePeerAddress();
      pushBleBatteryLevel();
      lastActionText = "蓝牙已连接";
    } else {
      peerAddressText = "";
      lastActionText = "蓝牙已断开";
    }

    wakeScreen();
    lastActivityMs = millis();
    drawUI();
  }

  // 电量低频轮询；变化会同步 BLE HID 电池服务，但不会主动唤醒屏幕。
  if (now - lastBatteryPollMs >= getBatteryPollIntervalMs()) {
    lastBatteryPollMs = now;

    if (refreshBatteryStatus() && !screenOff) {
      drawUI();
    }
  }

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    bool handled = false;
    bool wasScreenOff = screenOff;

    // 任意按键先唤醒屏幕并刷新活动时间；无效键不发送蓝牙。
    wakeScreen();
    lastActivityMs = millis();

    if (M5Cardputer.Keyboard.isKeyPressed(KEY_FN) || status.fn) {
      handled = handleFnSettingsPhysical();
    } else {
      for (auto c : status.word) {
        if (handleDx1Key(c)) {
          handled = true;

          // 记录音量键长按状态，首次按下后启动连续发送计时
          if (c == '1') {
            volDownHeld = true;
            lastVolDownMs = now;
          } else if (c == '2') {
            volUpHeld = true;
            lastVolUpMs = now;
          }

          continue;
        }

        if (handleMediaKey(c)) {
          handled = true;
          continue;
        }
      }
    }

    // 熄屏状态下任意键唤醒后需要重绘；正常亮屏时只有处理过的键才重绘。
    if (handled || wasScreenOff) {
      drawUI();
    }
  }

  if (autoSleepEnabled && !screenOff) {
    unsigned long timeoutMs = (unsigned long)kSleepPresetsSec[sleepPresetIndex] * 1000UL;

    if (millis() - lastActivityMs >= timeoutMs) {
      turnScreenOff();
    }
  }

  delay(getLoopDelayMs(connected));
}