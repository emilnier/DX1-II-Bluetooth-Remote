/*
  DX1 II Bluetooth Remote for M5Stack Cardputer-Adv
  ==================================================

  Three independent things happen over the same BLE HID connection:

  1. DX1 II control combos (Ctrl+Alt+<key>) -> caught by a background
     Node.js service on the PC (see dx1_bridge/), which talks to the DX1 II
     directly over USB via node-hid. Because that service uses a global
     keyboard hook (node-global-key-listener) instead of a browser keydown
     listener, these work NO MATTER which window has focus on the PC.

  2. Media transport keys (P/N/B) are sent as real HID "Consumer Control"
     media keys (Play/Pause, Next, Previous). Every OS already routes those
     globally to whatever app owns the current media session - no companion
     software needed at all, and no focus requirement either.

  3. Fn+<key> combos are LOCAL settings for the Cardputer itself (auto
     screen-off, page switching) - nothing is sent over Bluetooth for these,
     they just change how the Cardputer's own screen behaves.

  Key map:
    C            Ctrl+Alt+C   Connect
    X            Ctrl+Alt+X   Disconnect
    1 / 2        Ctrl+Alt+1/2 Volume -/+
    M            Ctrl+Alt+M   Mute toggle
    I            Ctrl+Alt+I   Input toggle
    O            Ctrl+Alt+O   Output cycle
    P            Media key    Play / Pause
    N            Media key    Next track
    B            Media key    Previous track
    Fn+S         Local        Auto screen-off: on/off
    Fn+, / Fn+.  Local        Auto screen-off timeout: shorter / longer
    Fn+/         Local        Switch screen page

  Power-saving revision:
  ----------------------
  本版在不改变原功能的基础上，主要做了以下省电优化：

  1. 关闭未使用的 WiFi。
  2. 将 ESP32-S3 CPU 频率降到 80 MHz，保留 BLE HID 与键盘扫描稳定性。
  3. 降低 BLE 发射功率，适合近距离遥控场景。
  4. 降低默认屏幕亮度。
  5. 电量轮询从 10 秒延长到 30 秒，减少 I2C/电源管理读取频率。
  6. loop 空闲延时改为自适应：亮屏/连接/熄屏时使用不同轮询间隔，减少 CPU 空转。
  7. 保留原有自动熄屏逻辑，电量变化不会主动唤醒屏幕。
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
#include <WiFi.h>   // 省电优化：用于关闭未使用的 WiFi 射频。

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

static bool autoSleepEnabled = true;
static uint8_t sleepPresetIndex = 1;
static bool screenOff = false;
static unsigned long lastActivityMs = 0;

// 0 = status page, 1 = key legend page
static uint8_t currentPage = 0;

static String lastActionText = "等待按键...";
static bool lastConnectedState = false;
static String peerAddressText = "";

// 省电优化：原值为 80。降低亮度能明显减少屏幕背光耗电。
// 如果你觉得屏幕偏暗，可以改回 70 或 80。
static const uint8_t kScreenBrightness = 55;

// 省电优化：ESP32-S3 降频到 80 MHz。
// 这个频率对 BLE HID、按键扫描、屏幕刷新通常足够，且比默认高频更省电。
// 不建议降到 40 MHz，可能影响 BLE 稳定性或屏幕/I2C响应。
static const uint32_t kCpuFrequencyMhz = 80;

// 省电优化：BLE 发射功率降低，适合 Cardputer 与电脑近距离使用。
// 如果连接距离较远或断连，把 ESP_PWR_LVL_N3 改成 ESP_PWR_LVL_P3 或 ESP_PWR_LVL_P6。
static const esp_power_level_t kBleTxPower = ESP_PWR_LVL_N3;

// --- Battery state ---------------------------------------------------
static int32_t batteryLevel = -1;
static bool batteryCharging = false;
static unsigned long lastBatteryPollMs = 0;

// 省电优化：原来 10 秒轮询一次电量。
// 电量显示不需要很高实时性，改成 30 秒可减少不必要的电源读取和刷新。
static const unsigned long kBatteryPollIntervalMs = 30000;

// 省电优化：自适应 loop 延时，减少 CPU 空转。
// 亮屏且已连接时保持较快响应；未连接或熄屏时降低轮询频率。
// 如果你感觉按键响应变慢，可把这些数值适当调小。
static const uint16_t kLoopDelayConnectedMs = 8;
static const uint16_t kLoopDelayDisconnectedMs = 12;
static const uint16_t kLoopDelayScreenOffMs = 20;

void applyPowerSavingConfigBeforeBle() {
  // 省电优化：本固件不使用 WiFi，关闭 WiFi 射频。
  WiFi.mode(WIFI_OFF);

  // 省电优化：降低 CPU 频率，减少基础功耗。
  setCpuFrequencyMhz(kCpuFrequencyMhz);
}

void applyPowerSavingConfigAfterBleBegin() {
  // 省电优化：BLE HID 已启动后再设置 NimBLE 发射功率。
  // 低发射功率可降低无线部分耗电，但会缩短有效距离。
  NimBLEDevice::setPower(kBleTxPower);
}

uint16_t getLoopDelayMs(bool connected) {
  if (screenOff) {
    return kLoopDelayScreenOffMs;
  }

  return connected ? kLoopDelayConnectedMs : kLoopDelayDisconnectedMs;
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

  bool changed = (level != batteryLevel) || (charging != batteryCharging);
  batteryLevel = level;
  batteryCharging = charging;
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
  if (screenOff) {
    screenOff = false;

    // 省电优化：熄屏时只关闭背光，唤醒时恢复到较低亮度。
    // 这样比重启屏幕控制器更稳，不影响原有 UI 显示逻辑。
    M5Cardputer.Display.setBrightness(kScreenBrightness);
  }
}

void turnScreenOff() {
  if (!screenOff) {
    screenOff = true;

    // 省电优化：关闭背光是 Cardputer 上最直接、最稳定的省电方式。
    // 不调用 Display.sleep()，避免部分库版本下唤醒后花屏或需要重绘初始化。
    M5Cardputer.Display.setBrightness(0);
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
  char battText[20];
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

  y += 20;

  lcd.drawFastHLine(0, y, lcd.width(), 0x39C7);
  y += 6;

  lcd.setTextColor(YELLOW, BLACK);
  lcd.setCursor(4, y);
  lcd.print(lastActionText);

  lcd.setTextColor(0x7BEF, BLACK);
  lcd.setCursor(4, lcd.height() - 16);
  lcd.print("Fn+/ 查看按键说明");
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

  const char* rows[6][2] = {
    {"C 连接", "X 断开"},
    {"1 音量-", "2 音量+"},
    {"M 静音", "I 输入"},
    {"O 输出", "P 播放暂停"},
    {"N 下一曲", "B 上一曲"},
    {"Fn+S 熄屏开关", "Fn+,/. 熄屏时长"},
  };

  for (int row = 0; row < 6; row++) {
    lcd.setCursor(colX[0], y);
    lcd.print(rows[row][0]);

    lcd.setCursor(colX[1], y);
    lcd.print(rows[row][1]);

    y += 17;
  }
}

void drawUI() {
  // 省电优化：屏幕熄灭时不主动重绘，避免无意义 SPI 刷屏。
  // 需要显示时会先 wakeScreen()，再 drawUI()。
  if (screenOff) {
    return;
  }

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
  if (c == 's' || c == 'S') {
    autoSleepEnabled = !autoSleepEnabled;

    // 省电说明：NVS 写入只在用户切换设置时发生，频率很低，保留原逻辑。
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

  return false;
}

/*
  Cardputer-Adv / newer M5Cardputer Fn-layer fix:

  不再依赖 status.word 判断 Fn+/、Fn+,、Fn+.。
  因为 Fn 层可能把这些键转换成方向键或其他逻辑键，
  导致 status.word 里没有原始字符 '/'、','、'.'。

  这里直接用 isKeyPressed(KEY_FN) + isKeyPressed('<physical key>')
  判断当前物理按键组合。
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

  return false;
}

void setup() {
  auto cfg = M5.config();

  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(kScreenBrightness);

  // 省电优化：M5 初始化完成后、BLE 启动前，关闭 WiFi 并降低 CPU 频率。
  applyPowerSavingConfigBeforeBle();

  prefs.begin("dx1remote", false);
  autoSleepEnabled = prefs.getBool("autoSleep", true);
  sleepPresetIndex = prefs.getUChar("sleepIdx", 1);

  if (sleepPresetIndex >= kSleepPresetCount) {
    sleepPresetIndex = 1;
  }

  bleKeyboard.begin();

  // 省电优化：BLE HID 启动后降低 BLE 发射功率。
  applyPowerSavingConfigAfterBleBegin();

  refreshBatteryStatus();
  lastBatteryPollMs = millis();

  lastActivityMs = millis();
  drawUI();
}

void loop() {
  M5Cardputer.update();

  unsigned long now = millis();
  bool connected = bleKeyboard.isConnected();

  if (connected != lastConnectedState) {
    lastConnectedState = connected;

    if (connected) {
      delay(200);
      updatePeerAddress();
      lastActionText = "蓝牙已连接";
    } else {
      peerAddressText = "";
      lastActionText = "蓝牙已断开";
    }

    wakeScreen();
    lastActivityMs = millis();
    drawUI();
  }

  // 省电优化：电量低频轮询；电量变化不会唤醒屏幕。
  if (now - lastBatteryPollMs > kBatteryPollIntervalMs) {
    lastBatteryPollMs = now;

    if (refreshBatteryStatus() && !screenOff) {
      drawUI();
    }
  }

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    bool handled = false;

    if (M5Cardputer.Keyboard.isKeyPressed(KEY_FN) || status.fn) {
      handled = handleFnSettingsPhysical();
    } else {
      for (auto c : status.word) {
        if (handleDx1Key(c)) {
          handled = true;
        }

        if (handleMediaKey(c)) {
          handled = true;
        }
      }
    }

    if (handled) {
      wakeScreen();
      lastActivityMs = millis();
      drawUI();
    }
  }

  if (autoSleepEnabled && !screenOff) {
    unsigned long timeoutMs = (unsigned long)kSleepPresetsSec[sleepPresetIndex] * 1000UL;

    if (millis() - lastActivityMs > timeoutMs) {
      turnScreenOff();
    }
  }

  // 省电优化：原来固定 delay(5)。
  // 现在按状态自适应延时，减少空转耗电，同时仍保持 TCA8418 键盘 FIFO 不易溢出。
  delay(getLoopDelayMs(connected));
}