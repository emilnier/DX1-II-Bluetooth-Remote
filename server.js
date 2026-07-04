"use strict";

/*
  DX1 II Bluetooth Bridge
  ========================

  Runs as a background process on the PC. It does two things at once:

  1. Opens the TOPPING DX1 II directly over USB HID (via node-hid) and owns
     that connection - no browser tab required.
  2. Listens for the Ctrl+Alt+<key> combos sent by the Cardputer-Adv's BLE
     keyboard using a GLOBAL keyboard hook (node-global-key-listener), which
     works regardless of which window currently has focus.

  The dashboard in public/index.html is just a convenience UI for manual
  control - it talks to this process over a tiny local HTTP API instead of
  using WebHID itself, so there's only ever one thing holding the USB
  connection open.

  Media keys (play/pause/next/previous) are NOT handled here - the
  Cardputer sends those as real HID Consumer Control keys, which every OS
  already routes globally on its own.

  Install once:
    npm install
  Run:
    npm start
*/

const http = require("http");
const fs = require("fs");
const path = require("path");
const HID = require("node-hid");
const { GlobalKeyboardListener } = require("node-global-key-listener");

const PORT = Number(process.env.PORT || 8787);
const PUBLIC_DIR = path.join(__dirname, "public");
const VOLUME_STEP_DB = Number(process.env.VOLUME_STEP_DB || 1);

// If commands silently do nothing on real hardware, this is the first
// thing to flip - see the "HID report id" note in README.md.
const PREFIX_REPORT_ID_BYTE = true;

const TOPPING_VENDOR_ID = 0x152a;
const DX1II_PRODUCT_ID = 0x8750; // shared by DX1 II / E50 II family

const ProtocolType = {
  writeNack: 0x20,
};

const Command = {
  dx1Mute: 0x7200,
  dx1OutputSwitch: 0x7400,
  dx1Volume: 0x7600,
  dx1InputSwitch: 0x7b00,
};

const VolumeTarget = { headphone: 0, lineOut: 1, all: 2 };

function dx1VolumeCommand(target) {
  return (Command.dx1Volume | (target + 1)) >>> 0;
}

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

// Mirrors TOPPING Home's dx1DbToRaw() helper (see dx1_web/index.html).
function dx1DbToRaw(db) {
  const safeDb = Number.isFinite(db) ? clamp(db, -99, 0) : -30;
  const roundedDb = safeDb <= -10 ? Math.round(safeDb) : Math.round(safeDb * 2) / 2;
  return Math.round((roundedDb + 99) * 10) >>> 0;
}

/*
  TOPPING HID frame, 16 bytes - identical layout to the browser version:
  [0]    report prefix used by the web implementation
  [1-2]  header 0x22 0x33
  [3]    protocol type
  [4]    total frame length
  [5]    current frame index
  [6-7]  command, big-endian uint16
  [8-11] data, big-endian uint32
  [12-13] reserved
  [14-15] tail 0x66 0x77
*/
function buildHidFrame({ protocolType, command, data, totalFrameLen = 1, curFrame = 1 }) {
  const frame = Buffer.alloc(16);
  const value = data >>> 0;
  frame[0] = 0x00;
  frame[1] = 0x22;
  frame[2] = 0x33;
  frame[3] = protocolType & 0xff;
  frame[4] = totalFrameLen & 0xff;
  frame[5] = curFrame & 0xff;
  frame[6] = (command >> 8) & 0xff;
  frame[7] = command & 0xff;
  frame[8] = (value >> 24) & 0xff;
  frame[9] = (value >> 16) & 0xff;
  frame[10] = (value >> 8) & 0xff;
  frame[11] = value & 0xff;
  frame[12] = 0x00;
  frame[13] = 0x00;
  frame[14] = 0x66;
  frame[15] = 0x77;
  return frame;
}

/*
  DX1 II / E50 II family (productId 0x8750) wants the first byte shifted
  out before sending - same quirk the browser's makeOutputReportPayload()
  works around. node-hid's write() additionally wants the report ID as an
  explicit leading byte (WebHID takes it as a separate sendReport(id, ...)
  argument instead), so we prepend it here.
*/
function makeOutputReport(frame) {
  const shifted = Buffer.alloc(16);
  for (let i = 0; i < 15; i += 1) shifted[i] = frame[i + 1];
  shifted[15] = 0x00;
  return PREFIX_REPORT_ID_BYTE ? Buffer.concat([Buffer.from([0x00]), shifted]) : shifted;
}

// ---- Device state ---------------------------------------------------

let device = null;

const state = {
  connected: false,
  volumeDb: -30,
  volumeTarget: VolumeTarget.headphone,
  muted: false,
  input: 0, // 0 = USB, 1 = OPT
  output: 0, // 0 = 耳放, 1 = LO, 2 = ALL
  lastAction: "尚未执行任何操作",
};

function openDevice() {
  if (device) return true;
  try {
    device = new HID.HID(TOPPING_VENDOR_ID, DX1II_PRODUCT_ID);
    state.connected = true;
    state.lastAction = "已连接 DX1 II";
    console.log("[dx1-bridge] DX1 II opened via node-hid");
    return true;
  } catch (error) {
    device = null;
    state.connected = false;
    state.lastAction = `连接失败: ${error.message}`;
    console.warn("[dx1-bridge] could not open DX1 II:", error.message);
    return false;
  }
}

function closeDevice() {
  if (device) {
    try {
      device.close();
    } catch (error) {
      // ignore
    }
  }
  device = null;
  state.connected = false;
  state.lastAction = "已断开";
}

function sendRawCommand(command, data) {
  if (!device && !openDevice()) return false;
  const frame = buildHidFrame({ protocolType: ProtocolType.writeNack, command, data });
  const report = makeOutputReport(frame);
  try {
    device.write([...report]);
    console.log("[dx1-bridge] TX", {
      command: `0x${command.toString(16)}`,
      data,
      bytes: [...report],
    });
    return true;
  } catch (error) {
    console.error("[dx1-bridge] HID write failed:", error.message);
    closeDevice();
    return false;
  }
}

function setVolumeDb(db) {
  state.volumeDb = clamp(Math.round(db * 2) / 2, -99, 0);
  sendRawCommand(dx1VolumeCommand(state.volumeTarget), dx1DbToRaw(state.volumeDb));
  state.lastAction = `音量 ${state.volumeDb.toFixed(1)} dB`;
}

function performAction(action) {
  switch (action) {
    case "connect":
      openDevice();
      break;
    case "disconnect":
      closeDevice();
      break;
    case "volumeUp":
      setVolumeDb(state.volumeDb + VOLUME_STEP_DB);
      break;
    case "volumeDown":
      setVolumeDb(state.volumeDb - VOLUME_STEP_DB);
      break;
    case "muteToggle":
      state.muted = !state.muted;
      sendRawCommand(Command.dx1Mute, state.muted ? 1 : 0);
      state.lastAction = state.muted ? "已静音" : "已取消静音";
      break;
    case "inputToggle":
      state.input = state.input === 0 ? 1 : 0;
      sendRawCommand(Command.dx1InputSwitch, state.input);
      state.lastAction = `输入: ${state.input === 0 ? "USB" : "OPT"}`;
      break;
    case "outputCycle": {
      state.output = (state.output + 1) % 3;
      state.volumeTarget = state.output;
      sendRawCommand(Command.dx1OutputSwitch, state.output);
      const names = ["耳放", "LO", "ALL"];
      state.lastAction = `输出: ${names[state.output]}`;
      break;
    }
    case "setInput":
      break; // reserved, handled via inline payload in the HTTP handler
    case "setOutput":
      break; // reserved, handled via inline payload in the HTTP handler
    default:
      console.warn("[dx1-bridge] unknown action:", action);
  }
}

// ---- Global hotkeys (work no matter which window has focus) ---------

const HOTKEY_ACTIONS = {
  C: "connect",
  X: "disconnect",
  "1": "volumeDown",
  "2": "volumeUp",
  M: "muteToggle",
  I: "inputToggle",
  O: "outputCycle",
};

const keyboardListener = new GlobalKeyboardListener();
keyboardListener.addListener((event, down) => {
  if (event.state !== "DOWN") return;
  const ctrl = down["LEFT CTRL"] || down["RIGHT CTRL"];
  const alt = down["LEFT ALT"] || down["RIGHT ALT"];
  if (!ctrl || !alt) return;

  const action = HOTKEY_ACTIONS[(event.name || "").toUpperCase()];
  if (!action) return;

  console.log("[dx1-bridge] global hotkey ->", action);
  performAction(action);

  // Returning true tells node-global-key-listener to swallow the combo so
  // it doesn't also land in whatever app currently has focus.
  return true;
});

// ---- HTTP: static dashboard + tiny JSON API --------------------------

const contentTypes = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
};

function sendJson(res, status, payload) {
  res.writeHead(status, { "Content-Type": "application/json; charset=utf-8" });
  res.end(JSON.stringify(payload));
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);

  if (url.pathname === "/api/state" && req.method === "GET") {
    sendJson(res, 200, state);
    return;
  }

  if (url.pathname === "/api/command" && req.method === "POST") {
    let body = "";
    req.on("data", (chunk) => (body += chunk));
    req.on("end", () => {
      try {
        const payload = body ? JSON.parse(body) : {};
        if (payload.action === "setVolume" && typeof payload.value === "number") {
          setVolumeDb(payload.value);
        } else if (payload.action === "setInput" && typeof payload.value === "number") {
          state.input = payload.value;
          sendRawCommand(Command.dx1InputSwitch, state.input);
          state.lastAction = `输入: ${state.input === 0 ? "USB" : "OPT"}`;
        } else if (payload.action === "setOutput" && typeof payload.value === "number") {
          state.output = payload.value;
          state.volumeTarget = payload.value;
          sendRawCommand(Command.dx1OutputSwitch, state.output);
          const names = ["耳放", "LO", "ALL"];
          state.lastAction = `输出: ${names[state.output]}`;
        } else {
          performAction(payload.action);
        }
        sendJson(res, 200, state);
      } catch (error) {
        sendJson(res, 400, { error: error.message });
      }
    });
    return;
  }

  const requestedPath = url.pathname === "/" ? "/index.html" : url.pathname;
  const filePath = path.normalize(path.join(PUBLIC_DIR, requestedPath));
  if (!filePath.startsWith(PUBLIC_DIR)) {
    res.writeHead(403);
    res.end("Forbidden");
    return;
  }

  fs.readFile(filePath, (error, content) => {
    if (error) {
      res.writeHead(404);
      res.end("Not found");
      return;
    }
    res.writeHead(200, {
      "Content-Type": contentTypes[path.extname(filePath)] || "application/octet-stream",
      "Cache-Control": "no-store",
    });
    res.end(content);
  });
});

server.listen(PORT, "127.0.0.1", () => {
  console.log(`[dx1-bridge] dashboard: http://127.0.0.1:${PORT}/`);
  console.log("[dx1-bridge] global hotkeys active (Ctrl+Alt+C/X/1/2/M/I/O)");
  openDevice();
});

process.on("SIGINT", () => {
  closeDevice();
  process.exit(0);
});
