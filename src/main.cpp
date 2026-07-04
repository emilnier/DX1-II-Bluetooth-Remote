/*
  DX1 II Bluetooth Remote for M5Stack Cardputer-Adv
  ==================================================

  Three independent things happen over the same BLE HID connection:

  1. DX1 II control combos (Ctrl+Alt+<key>) -> caught by a background
     Node.js service on the PC (see dx1_bridge/), which talks to the DX1 II
     directly over USB via node-hBecause that service uses a global
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
    C            Ctrl+Alt+C   Connect (background service opens the DX1 II)
    X            Ctrl+Alt+X   Disconnect
    1 / 2        Ctrl+Alt+1/2 Volume -/+
    M            Ctrl+Alt+M   Mute toggle
    I            Ctrl+Alt+I   Input toggle (USB / OPT)
    O            Ctrl+Alt+O   Output cycle (耳放 / LO / ALL)
    P            (media key)  Play / Pause
    N            (media key)  Next track
    B            (media key)  Previous track
    Fn+S         (local)      Auto screen-off: on/off
    Fn+, / Fn+.  (local)      Auto screen-off timeout: shorter / longer
    Fn+/         (local)      Switch screen page: 状态 <-> 按键说明

  Library: M5Cardputer (official, https://github.com/m5stack/M5Cardputer)
  BLE HID: ESP32-BLE-Keyboard, NimBLE backend (small footprint, ESP32-S3 ok)

  ---------------------------------------------------------------------
  A note on Chinese text: M5GFX's default font has no CJK glyphs at all,
  so any 中文 printed with it silently comes out as garbage/blank glyphs
  (exactly what showed up on the first version of this firmware). M5GFX
  ships a bundled CJK bitmap font family for this - `fonts::efontCN_*` -
  which is what this version uses via setFont().
  ---------------------------------------------------------------------

  Important fix for Cardputer-Adv Fn layer:
  -----------------------------------------
  On recent M5Cardputer library versions, Fn+some keys may be translated by
  the keyboard layer into another logical key, so `status.word` may NOT contain
  the original character such as '/'. Therefore local Fn settings are handled
  with Keyboard.isKeyPressed(KEY_FN) + Keyboard.isKeyPressed('<physical key>')
  instead of looping over status.word.

  ---------------------------------------------------------------------
  Battery display (this revision):
  ---------------------------------------------------------------------
  Cardputer-Adv exposes its fuel-gauge readout through M5Unified's Power
  class, which the M5Cardputer object already inherits - no extra library
  needed. getBatteryLevel() returns 0-100, or -1 if the gauge hasn't
  produced a reading yet (right at boot). isCharging() returns an enum,
  not a plain bool (1 = charging, 0 = discharging, -1 = unknown), so it's
  read into an int8_t and compared explicitly rather than truthiness-cast.

  The reading is polled on a timer (kBatteryPollIntervalMs) instead of
  every loop() iteration, since the ADC/gauge is happy being read
  occasionally and this avoids needless redraws. A poll only triggers a
  redraw if the level or charge state actually changed AND the screen is
  currently on - a battery-only change should never wake the screen, only
  key activity should.
  ---------------------------------------------------------------------
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

BleKeyboard bleKeyboard("DX1II Remote", "M5Stack", 100);
Preferences prefs;

struct KeyAction {
  char key;
  char sendChar;      // sent as Ctrl+Alt+<sendChar>
  const char* label;
};

struct MediaAction {
  char key;
  const uint8_t* mediaKey;  // one of BleKeyboard's KEY_MEDIA_* constants
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
static uint8_t sleepPresetIndex = 1;  // default 30s
static bool screenOff = false;
static unsigned long lastActivityMs = 0;

// 0 = status page (connection / sleep / last action), 1 = key legend page
static uint8_t currentPage = 0;

static String lastActionText = "等待按键...";
static bool lastConnectedState = false;
static String peerAddressText = "";  // filled in on connect, see note below

static const uint8_t kScreenBrightness = 80;

// --- Battery state ---------------------------------------------------
static int32_t batteryLevel = -1;      // 0-100, -1 = unknown/not ready yet
static bool batteryCharging = false;
static unsigned long lastBatteryPollMs = 0;
static const unsigned long kBatteryPollIntervalMs = 10000;  // 10s

/*
  Why an address and not a friendly device name: standard BLE HID only lets
  the peripheral (the Cardputer) read the connected central's Bluetooth
  address, not its human-readable name ("DESKTOP-ABC123" etc.) - that name
  lives in the central's GAP service, and reading it back would require the
  Cardputer to also act as a GATT *client* against its own HID *central* on
  the same radio, which is unreliable enough on a single-radio ESP32 that
  it isn't worth the instability here. The address is enough to confirm
  you're paired with the right PC when you have more than one nearby.
*/
void updatePeerAddress() {
  peerAddressText = "";

  NimBLEServer* server = NimBLEDevice::getServer();
  if (!server || server->getConnectedCount() == 0) return;

  NimBLEConnInfo info = server->getPeerInfo(0);
  peerAddressText = String(info.getAddress().toString().c_str());
}

// Reads the fuel gauge. Returns true if level or charge state changed.
bool refreshBatteryStatus() {
  int32_t level = M5Cardputer.Power.getBatteryLevel();  // 0-100, or -1
  int8_t chg = M5Cardputer.Power.isCharging();           // 1/0/-1
  bool charging = (chg == 1);

  bool changed = (level != batteryLevel) || (charging != batteryCharging);
  batteryLevel = level;
  batteryCharging = charging;
  return changed;
}

// Small battery pill icon, top-right anchored at (xRight, y). Draws the
// outline + nub + proportional fill, plus a "+" when charging and a "?"
// when the gauge hasn't produced a reading yet.
void drawBatteryIcon(int xRight, int y, int level, bool charging) {
  auto &lcd = M5Cardputer.Display;
  const int w = 22, h = 11, nub = 4;
  const int x = xRight - w - 3;  // leave room for the nub on the right

  const uint16_t frameColor = 0xC618;  // light gray

  lcd.drawRect(x, y, w, h, frameColor);
  lcd.fillRect(x + w, y + (h - nub) / 2, 2, nub, frameColor);

  // Clear the interior before filling / drawing "?" so old fill doesn't
  // show through on redraw.
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
    M5Cardputer.Display.setBrightness(kScreenBrightness);
  }
}

void drawStatusPage() {
  auto &lcd = M5Cardputer.Display;
  int y = 2;

  lcd.setFont(&fonts::efontCN_16);
  lcd.setTextColor(WHITE, BLACK);
  lcd.setCursor(4, y);
  lcd.print("DX1 II 蓝牙遥控");

  // Battery pill, top-right corner of the title row.
  drawBatteryIcon(lcd.width() - 4, y + 2, batteryLevel, batteryCharging);

  y += 20;

  lcd.setFont(&fonts::efontCN_14);
  bool connected = bleKeyboard.isConnected();

  lcd.setTextColor(connected ? GREEN : RED, BLACK);
  lcd.setCursor(4, y);
  lcd.print(connected ? "蓝牙: 已连接" : "蓝牙: 等待PC连接...");

  // Battery percentage text, right-aligned on the same row as BLE status.
  lcd.setTextColor(0xC618, BLACK);
  char battText[16];
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

  lcd.setTextColor(0xC618, BLACK);  // light gray
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
  y += 20;

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

  // Keep the battery pill visible on this page too, so it's always
  // glanceable regardless of which page you're on.
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
  delay(15);
  bleKeyboard.releaseAll();
}

// Returns true if `c` matched a DX1 control key and was handled.
bool handleDx1Key(char c) {
  // 统一转小写，避免 Shift 或某些输入状态下拿到大写字母后不匹配
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

// Returns true if `c` matched a media key and was handled.
bool handleMediaKey(char c) {
  // 统一转小写，避免 Shift 或某些输入状态下拿到大写字母后不匹配
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

// Fn-layer local settings. Nothing is sent over Bluetooth here.
bool handleSettingsKey(char c) {
  if (c == 's' || c == 'S') {
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

  M5Cardputer.begin(cfg, true);  // true = enable keyboard
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(kScreenBrightness);

  prefs.begin("dx1remote", false);
  autoSleepEnabled = prefs.getBool("autoSleep", true);
  sleepPresetIndex = prefs.getUChar("sleepIdx", 1);

  if (sleepPresetIndex >= kSleepPresetCount) {
    sleepPresetIndex = 1;
  }

  bleKeyboard.begin();

  // Prime the battery reading once before the first draw so the icon
  // doesn't start life on the "?" placeholder if a reading is already
  // available at boot.
  refreshBatteryStatus();
  lastBatteryPollMs = millis();

  lastActivityMs = millis();
  drawUI();
}

void loop() {
  M5Cardputer.update();

  bool connected = bleKeyboard.isConnected();

  if (connected != lastConnectedState) {
    lastConnectedState = connected;

    if (connected) {
      delay(200);  // let the connection settle before reading peer info
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

  // Poll the battery gauge on its own timer, independent of key/BLE
  // activity. Only redraw if something actually changed, and only if the
  // screen is already on - a battery-only change must never wake it.
  if (millis() - lastBatteryPollMs > kBatteryPollIntervalMs) {
    lastBatteryPollMs = millis();

    if (refreshBatteryStatus() && !screenOff) {
      drawUI();
    }
  }

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    bool handled = false;

    /*
      原来的写法是：

        if (status.fn) {
          for (auto c : status.word) {
            if (handleSettingsKey(c)) handled = true;
          }
        }

      这会导致 Fn+/ 失效，因为 Fn+/ 可能不会以 '/' 出现在 status.word。
      现在改为物理按键检测。
    */
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
      screenOff = true;
      M5Cardputer.Display.setBrightness(0);
    }
  }

  delay(5);  // keep the TCA8418 FIFO from overflowing
}
