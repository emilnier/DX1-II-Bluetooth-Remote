"use strict";

/*
  DX1 II Bluetooth Bridge
  =======================

  - Captures Cardputer Ctrl+Alt+key shortcuts globally.
  - Serializes all USB HID writes with a configurable inter-command gap.
  - Automatically reopens the DX1 II after unplug/replug unless the user
    explicitly disconnected it.
  - Exposes a localhost-only dashboard/API and Server-Sent Events stream.
  - Keeps state changes transactional: UI state is updated only after a
    successful device write.
*/

const http = require("http");
const fs = require("fs");
const path = require("path");

const HOST = "127.0.0.1";
const PORT = parseInteger(process.env.PORT, 8787, 1, 65535);
const PUBLIC_DIR = path.resolve(__dirname, "public");
const VOLUME_STEP_DB = parseNumber(process.env.VOLUME_STEP_DB, 1, 0.5, 10);
const SAFE_VOLUME_DB = parseNumber(process.env.SAFE_VOLUME_DB, -40, -99, 0);
const COMMAND_GAP_MS = parseInteger(process.env.COMMAND_GAP_MS, 18, 0, 1000);
const DEVICE_POLL_MS = parseInteger(process.env.DEVICE_POLL_MS, 3000, 500, 60000);
const PREFIX_REPORT_ID_BYTE = parseBoolean(process.env.PREFIX_REPORT_ID_BYTE, true);
const AUTO_CONNECT = parseBoolean(process.env.AUTO_CONNECT, true);
const DEBUG_HID = parseBoolean(process.env.DEBUG_HID, false);
const MAX_BODY_BYTES = 16 * 1024;
const MAX_LOG_ENTRIES = 80;

const TOPPING_VENDOR_ID = 0x152a;
const DX1II_PRODUCT_ID = 0x8750;

const ProtocolType = Object.freeze({ writeNack: 0x20 });
const Command = Object.freeze({
  dx1Mute: 0x7200,
  dx1OutputSwitch: 0x7400,
  dx1Volume: 0x7600,
  dx1InputSwitch: 0x7b00,
});
const VolumeTarget = Object.freeze({ headphone: 0, lineOut: 1, all: 2 });
const OUTPUT_NAMES = Object.freeze(["耳放", "LO", "ALL"]);
const INPUT_NAMES = Object.freeze(["USB", "OPT"]);
const VOLUME_PRESETS_DB = Object.freeze([-60, -50, -40, -30, -20, -10]);

const HOTKEY_ACTIONS = Object.freeze({
  C: "connect",
  X: "disconnect",
  "1": "volumeDown",
  "2": "volumeUp",
  "3": "volumePreset",
  "0": "safeVolume",
  R: "restoreVolume",
  M: "muteToggle",
  I: "inputToggle",
  O: "outputCycle",
  U: "inputUsb",
  T: "inputOptical",
  "7": "outputHeadphone",
  "8": "outputLineOut",
  "9": "outputAll",
});

const contentTypes = Object.freeze({
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".ico": "image/x-icon",
});

function parseBoolean(value, fallback) {
  if (value === undefined || value === null || value === "") return fallback;
  return /^(1|true|yes|on)$/i.test(String(value));
}

function parseNumber(value, fallback, min, max) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) return fallback;
  return clamp(parsed, min, max);
}

function parseInteger(value, fallback, min, max) {
  const parsed = Number.parseInt(value, 10);
  if (!Number.isFinite(parsed)) return fallback;
  return clamp(parsed, min, max);
}

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function dx1VolumeCommand(target) {
  return (Command.dx1Volume | (target + 1)) >>> 0;
}

function normalizeVolumeDb(db) {
  const safeDb = Number.isFinite(db) ? clamp(db, -99, 0) : -30;
  return safeDb <= -10 ? Math.round(safeDb) : Math.round(safeDb * 2) / 2;
}

// Mirrors TOPPING Home's conversion helper.
function dx1DbToRaw(db) {
  return Math.round((normalizeVolumeDb(db) + 99) * 10) >>> 0;
}

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

function makeOutputReport(frame, prefixReportIdByte = PREFIX_REPORT_ID_BYTE) {
  if (!Buffer.isBuffer(frame) || frame.length !== 16) {
    throw new TypeError("HID frame must be a 16-byte Buffer");
  }
  const shifted = Buffer.alloc(16);
  frame.copy(shifted, 0, 1, 16);
  shifted[15] = 0x00;
  return prefixReportIdByte
    ? Buffer.concat([Buffer.from([0x00]), shifted])
    : shifted;
}

function sanitizeDeviceInfo(info) {
  if (!info) return null;
  return {
    manufacturer: info.manufacturer || "TOPPING",
    product: info.product || "DX1 II",
    serialNumber: info.serialNumber || "",
    interface: Number.isInteger(info.interface) ? info.interface : null,
    usagePage: Number.isInteger(info.usagePage) ? info.usagePage : null,
    usage: Number.isInteger(info.usage) ? info.usage : null,
  };
}

function normalizeHotkeyName(name) {
  let key = String(name || "").trim().toUpperCase();
  key = key.replace(/^NUMPAD\s+/, "");
  const aliases = {
    ZERO: "0",
    ONE: "1",
    TWO: "2",
    THREE: "3",
    SEVEN: "7",
    EIGHT: "8",
    NINE: "9",
  };
  return aliases[key] || key;
}

function validateCommandPayload(payload) {
  if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
    throw new TypeError("请求体必须是 JSON 对象");
  }
  const action = typeof payload.action === "string" ? payload.action.trim() : "";
  if (!action) throw new TypeError("缺少 action");

  const noValueActions = new Set([
    "connect", "disconnect", "volumeUp", "volumeDown", "volumePreset",
    "safeVolume", "restoreVolume", "muteToggle", "inputToggle", "outputCycle",
    "inputUsb", "inputOptical", "outputHeadphone", "outputLineOut", "outputAll",
  ]);

  if (noValueActions.has(action)) return { action };

  if (action === "setVolume") {
    const value = Number(payload.value);
    if (!Number.isFinite(value) || value < -99 || value > 0) {
      throw new RangeError("音量必须在 -99 到 0 dB 之间");
    }
    return { action, value };
  }

  if (action === "setInput") {
    const value = Number(payload.value);
    if (!Number.isInteger(value) || ![0, 1].includes(value)) {
      throw new RangeError("输入值必须是 0 或 1");
    }
    return { action, value };
  }

  if (action === "setOutput") {
    const value = Number(payload.value);
    if (!Number.isInteger(value) || ![0, 1, 2].includes(value)) {
      throw new RangeError("输出值必须是 0、1 或 2");
    }
    return { action, value };
  }

  throw new RangeError(`未知 action: ${action}`);
}

class Bridge {
  constructor({ HID, logger = console }) {
    if (!HID) throw new TypeError("HID module is required");
    this.HID = HID;
    this.logger = logger;
    this.device = null;
    this.devicePath = null;
    this.deviceInfo = null;
    this.manualDisconnected = !AUTO_CONNECT;
    this.commandTail = Promise.resolve();
    this.lastWriteAt = 0;
    this.sseClients = new Set();
    this.logs = [];
    this.volumePresetIndex = VOLUME_PRESETS_DB.indexOf(-30);
    this.lastHotkeyAt = new Map();
    this.lastOpenFailureLogAt = 0;
    this.lastOpenFailureMessage = "";

    this.state = {
      revision: 0,
      connected: false,
      autoConnect: AUTO_CONNECT,
      manualDisconnected: this.manualDisconnected,
      volumeDb: -30,
      volumeTarget: VolumeTarget.headphone,
      restoreVolumeDb: null,
      muted: false,
      input: 0,
      output: 0,
      lastAction: "尚未执行任何操作",
      lastError: "",
      lastActionAt: new Date().toISOString(),
      commandCount: 0,
      failedCommandCount: 0,
      reconnectAttempts: 0,
      queueDepth: 0,
      device: null,
      assumedState: true,
      config: {
        volumeStepDb: VOLUME_STEP_DB,
        safeVolumeDb: SAFE_VOLUME_DB,
        commandGapMs: COMMAND_GAP_MS,
        prefixReportIdByte: PREFIX_REPORT_ID_BYTE,
      },
    };
  }

  log(level, message, meta) {
    const entry = {
      time: new Date().toISOString(),
      level,
      message,
      ...(meta === undefined ? {} : { meta }),
    };
    this.logs.push(entry);
    if (this.logs.length > MAX_LOG_ENTRIES) this.logs.shift();

    const fn = level === "error" ? "error" : level === "warn" ? "warn" : "log";
    this.logger[fn](`[dx1-bridge] ${message}`, meta === undefined ? "" : meta);
    this.broadcastState();
  }

  snapshot() {
    return {
      ...this.state,
      logs: this.logs.slice(-30),
    };
  }

  touchState({ action, error } = {}) {
    if (action !== undefined) this.state.lastAction = action;
    if (error !== undefined) this.state.lastError = error;
    this.state.manualDisconnected = this.manualDisconnected;
    this.state.revision += 1;
    this.state.lastActionAt = new Date().toISOString();
    this.broadcastState();
  }

  broadcastState() {
    if (this.sseClients.size === 0) return;
    const packet = `event: state\ndata: ${JSON.stringify(this.snapshot())}\n\n`;
    for (const res of [...this.sseClients]) {
      try {
        res.write(packet);
      } catch {
        this.sseClients.delete(res);
      }
    }
  }

  addSseClient(res) {
    this.sseClients.add(res);
    res.write(`event: state\ndata: ${JSON.stringify(this.snapshot())}\n\n`);
    return () => this.sseClients.delete(res);
  }

  listCandidates() {
    const requestedPath = process.env.DX1_HID_PATH;
    const devices = this.HID.devices();
    return devices
      .filter((item) => {
        if (requestedPath) return item.path === requestedPath;
        return item.vendorId === TOPPING_VENDOR_ID && item.productId === DX1II_PRODUCT_ID;
      })
      .sort((a, b) => {
        // Prefer vendor-defined HID interfaces over generic keyboard/control pages.
        const score = (item) => {
          let value = 0;
          if (item.usagePage >= 0xff00) value += 20;
          if (item.interface !== undefined && item.interface !== null) value += 2;
          if (item.path) value += 1;
          return value;
        };
        return score(b) - score(a);
      });
  }

  openDevice() {
    if (this.device) return true;

    let candidates;
    try {
      candidates = this.listCandidates();
    } catch (error) {
      this.handleOpenFailure(error);
      return false;
    }

    if (candidates.length === 0) {
      this.handleOpenFailure(new Error("未发现 VID 0x152A / PID 0x8750 的 HID 设备"), false);
      return false;
    }

    for (const candidate of candidates) {
      try {
        this.device = candidate.path
          ? new this.HID.HID(candidate.path)
          : new this.HID.HID(TOPPING_VENDOR_ID, DX1II_PRODUCT_ID);
        this.devicePath = candidate.path || null;
        this.deviceInfo = candidate;
        this.state.connected = true;
        this.state.device = sanitizeDeviceInfo(candidate);
        this.state.reconnectAttempts = 0;
        this.touchState({ action: "已连接 DX1 II", error: "" });
        this.log("info", "DX1 II 已通过 node-hid 打开", this.state.device);
        return true;
      } catch (error) {
        this.device = null;
        this.devicePath = null;
        this.deviceInfo = null;
        this.log("warn", "尝试打开 HID 接口失败", {
          interface: candidate.interface,
          usagePage: candidate.usagePage,
          message: error.message,
        });
      }
    }

    this.handleOpenFailure(new Error("找到设备，但所有 HID 接口均无法打开"));
    return false;
  }

  handleOpenFailure(error, countAttempt = true) {
    const message = error.message || String(error);
    const now = Date.now();
    const stateChanged = this.state.connected || this.state.lastError !== message;
    const shouldLog = message !== this.lastOpenFailureMessage || now - this.lastOpenFailureLogAt >= 30000;

    this.state.connected = false;
    this.state.device = null;
    if (countAttempt) this.state.reconnectAttempts += 1;

    if (stateChanged) {
      this.touchState({ action: "DX1 II 未连接", error: message });
    }
    if (shouldLog) {
      this.lastOpenFailureMessage = message;
      this.lastOpenFailureLogAt = now;
      this.log("warn", "无法打开 DX1 II", message);
    }
  }

  closeDevice({ manual = false, action = "已断开" } = {}) {
    if (manual) this.manualDisconnected = true;
    if (this.device) {
      try {
        this.device.close();
      } catch (error) {
        this.log("warn", "关闭 HID 设备时出现异常", error.message);
      }
    }
    this.device = null;
    this.devicePath = null;
    this.deviceInfo = null;
    this.state.connected = false;
    this.state.device = null;
    this.touchState({ action, error: "" });
  }

  async sendRawCommand(command, data) {
    if (!this.device && !this.openDevice()) return false;

    const elapsed = Date.now() - this.lastWriteAt;
    if (elapsed < COMMAND_GAP_MS) await delay(COMMAND_GAP_MS - elapsed);

    const frame = buildHidFrame({
      protocolType: ProtocolType.writeNack,
      command,
      data,
    });
    const report = makeOutputReport(frame);

    try {
      this.device.write([...report]);
      this.lastWriteAt = Date.now();
      this.state.commandCount += 1;
      this.state.lastError = "";
      if (DEBUG_HID) {
        this.log("info", "HID TX", {
          command: `0x${command.toString(16).padStart(4, "0")}`,
          data,
          bytes: [...report],
        });
      }
      return true;
    } catch (error) {
      this.state.failedCommandCount += 1;
      this.log("error", "HID 写入失败", error.message);
      this.closeDevice({ manual: false, action: "设备连接丢失，等待自动重连" });
      this.state.lastError = error.message;
      return false;
    }
  }

  enqueueAction(action, value) {
    this.state.queueDepth += 1;
    this.touchState();

    const job = this.commandTail.then(() => this.performAction(action, value));
    this.commandTail = job.catch(() => undefined);

    return job.finally(() => {
      this.state.queueDepth = Math.max(0, this.state.queueDepth - 1);
      this.touchState();
    });
  }

  async setVolumeDb(db, { rememberForRestore = false } = {}) {
    const next = normalizeVolumeDb(db);
    const restoreCandidate = this.state.volumeDb;
    const ok = await this.sendRawCommand(dx1VolumeCommand(this.state.volumeTarget), dx1DbToRaw(next));
    if (!ok) return false;
    if (rememberForRestore) this.state.restoreVolumeDb = restoreCandidate;
    this.state.volumeDb = next;
    this.touchState({ action: `音量 ${next.toFixed(1)} dB`, error: "" });
    return true;
  }

  async setInput(value) {
    const ok = await this.sendRawCommand(Command.dx1InputSwitch, value);
    if (!ok) return false;
    this.state.input = value;
    this.touchState({ action: `输入: ${INPUT_NAMES[value]}`, error: "" });
    return true;
  }

  async setOutput(value) {
    const ok = await this.sendRawCommand(Command.dx1OutputSwitch, value);
    if (!ok) return false;
    this.state.output = value;
    this.state.volumeTarget = value;
    this.touchState({ action: `输出: ${OUTPUT_NAMES[value]}`, error: "" });
    return true;
  }

  async performAction(action, value) {
    switch (action) {
      case "connect":
        this.manualDisconnected = false;
        this.state.manualDisconnected = false;
        return this.openDevice();
      case "disconnect":
        this.closeDevice({ manual: true, action: "已手动断开；自动重连暂停" });
        return true;
      case "volumeUp":
        return this.setVolumeDb(this.state.volumeDb + VOLUME_STEP_DB);
      case "volumeDown":
        return this.setVolumeDb(this.state.volumeDb - VOLUME_STEP_DB);
      case "setVolume":
        return this.setVolumeDb(value);
      case "volumePreset": {
        const nextIndex = (this.volumePresetIndex + 1) % VOLUME_PRESETS_DB.length;
        const ok = await this.setVolumeDb(VOLUME_PRESETS_DB[nextIndex]);
        if (ok) this.volumePresetIndex = nextIndex;
        return ok;
      }
      case "safeVolume":
        return this.setVolumeDb(SAFE_VOLUME_DB, { rememberForRestore: true });
      case "restoreVolume":
        if (this.state.restoreVolumeDb === null) {
          this.touchState({ action: "没有可恢复的音量记录" });
          return true;
        }
        return this.setVolumeDb(this.state.restoreVolumeDb);
      case "muteToggle": {
        const nextMuted = !this.state.muted;
        const ok = await this.sendRawCommand(Command.dx1Mute, nextMuted ? 1 : 0);
        if (!ok) return false;
        this.state.muted = nextMuted;
        this.touchState({ action: nextMuted ? "已静音" : "已取消静音", error: "" });
        return true;
      }
      case "inputToggle":
        return this.setInput(this.state.input === 0 ? 1 : 0);
      case "setInput":
        return this.setInput(value);
      case "inputUsb":
        return this.setInput(0);
      case "inputOptical":
        return this.setInput(1);
      case "outputCycle":
        return this.setOutput((this.state.output + 1) % 3);
      case "setOutput":
        return this.setOutput(value);
      case "outputHeadphone":
        return this.setOutput(0);
      case "outputLineOut":
        return this.setOutput(1);
      case "outputAll":
        return this.setOutput(2);
      default:
        throw new RangeError(`未知 action: ${action}`);
    }
  }

  shouldAcceptHotkey(key) {
    const now = Date.now();
    const previous = this.lastHotkeyAt.get(key) || 0;
    // Debounce duplicate global-hook DOWN events while preserving intentional
    // firmware repeats (the fastest firmware repeat is 90 ms).
    if (now - previous < 35) return false;
    this.lastHotkeyAt.set(key, now);
    return true;
  }

  pollDevicePresence() {
    if (this.manualDisconnected) return;

    if (!this.device) {
      this.openDevice();
      return;
    }

    if (!this.devicePath) return;
    try {
      const stillPresent = this.HID.devices().some((item) => item.path === this.devicePath);
      if (!stillPresent) {
        this.log("warn", "检测到 DX1 II 已拔出");
        this.closeDevice({ manual: false, action: "设备已拔出，等待自动重连" });
      }
    } catch (error) {
      this.log("warn", "设备在线检测失败", error.message);
    }
  }

  shutdown() {
    this.manualDisconnected = true;
    this.closeDevice({ manual: false, action: "服务已停止" });
    for (const res of this.sseClients) {
      try { res.end(); } catch { /* no-op */ }
    }
    this.sseClients.clear();
  }
}

function setCommonHeaders(res) {
  res.setHeader("X-Content-Type-Options", "nosniff");
  res.setHeader("Referrer-Policy", "no-referrer");
  res.setHeader("X-Frame-Options", "DENY");
  res.setHeader("Cache-Control", "no-store");
}

function sendJson(res, status, payload) {
  setCommonHeaders(res);
  res.writeHead(status, { "Content-Type": "application/json; charset=utf-8" });
  res.end(JSON.stringify(payload));
}

function readJsonBody(req) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];

    req.on("data", (chunk) => {
      size += chunk.length;
      if (size > MAX_BODY_BYTES) {
        reject(Object.assign(new Error("请求体过大"), { statusCode: 413 }));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });

    req.on("end", () => {
      try {
        const text = Buffer.concat(chunks).toString("utf8");
        resolve(text ? JSON.parse(text) : {});
      } catch {
        reject(Object.assign(new Error("JSON 格式无效"), { statusCode: 400 }));
      }
    });
    req.on("error", reject);
  });
}

function createHttpServer(bridge) {
  return http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url || "/", "http://127.0.0.1");

      if (url.pathname === "/api/state" && req.method === "GET") {
        sendJson(res, 200, bridge.snapshot());
        return;
      }

      if (url.pathname === "/api/health" && req.method === "GET") {
        sendJson(res, bridge.state.connected ? 200 : 503, {
          ok: bridge.state.connected,
          connected: bridge.state.connected,
          revision: bridge.state.revision,
        });
        return;
      }

      if (url.pathname === "/api/events" && req.method === "GET") {
        setCommonHeaders(res);
        res.writeHead(200, {
          "Content-Type": "text/event-stream; charset=utf-8",
          Connection: "keep-alive",
          "Cache-Control": "no-cache, no-transform",
        });
        res.write(": connected\n\n");
        const remove = bridge.addSseClient(res);
        const heartbeat = setInterval(() => {
          try { res.write(": heartbeat\n\n"); } catch { /* closed */ }
        }, 20000);
        req.on("close", () => {
          clearInterval(heartbeat);
          remove();
        });
        return;
      }

      if (url.pathname === "/api/command" && req.method === "POST") {
        const contentType = String(req.headers["content-type"] || "");
        if (!contentType.toLowerCase().startsWith("application/json")) {
          sendJson(res, 415, { error: "Content-Type 必须是 application/json" });
          return;
        }
        const payload = validateCommandPayload(await readJsonBody(req));
        const ok = await bridge.enqueueAction(payload.action, payload.value);
        sendJson(res, ok ? 200 : 503, {
          ok,
          state: bridge.snapshot(),
          ...(ok ? {} : { error: bridge.state.lastError || "命令发送失败" }),
        });
        return;
      }

      if (url.pathname.startsWith("/api/")) {
        sendJson(res, 404, { error: "API 不存在" });
        return;
      }

      if (!["GET", "HEAD"].includes(req.method)) {
        sendJson(res, 405, { error: "Method Not Allowed" });
        return;
      }

      if (url.pathname === "/favicon.ico") {
        res.writeHead(204);
        res.end();
        return;
      }

      let relativePath;
      try {
        relativePath = decodeURIComponent(url.pathname === "/" ? "index.html" : url.pathname.slice(1));
      } catch {
        sendJson(res, 400, { error: "无效路径" });
        return;
      }

      const filePath = path.resolve(PUBLIC_DIR, relativePath);
      if (filePath !== PUBLIC_DIR && !filePath.startsWith(`${PUBLIC_DIR}${path.sep}`)) {
        sendJson(res, 403, { error: "Forbidden" });
        return;
      }

      fs.readFile(filePath, (error, content) => {
        if (error) {
          sendJson(res, 404, { error: "Not found" });
          return;
        }
        setCommonHeaders(res);
        res.writeHead(200, {
          "Content-Type": contentTypes[path.extname(filePath).toLowerCase()] || "application/octet-stream",
          "Content-Length": content.length,
        });
        if (req.method === "HEAD") res.end();
        else res.end(content);
      });
    } catch (error) {
      const status = error.statusCode || (error instanceof RangeError || error instanceof TypeError ? 400 : 500);
      bridge.log(status >= 500 ? "error" : "warn", "HTTP 请求失败", error.message);
      if (!res.headersSent) sendJson(res, status, { error: error.message });
      else res.end();
    }
  });
}

function startRuntime() {
  // Lazy imports keep protocol/unit tests hardware-independent.
  // eslint-disable-next-line global-require
  const HID = require("node-hid");
  // eslint-disable-next-line global-require
  const { GlobalKeyboardListener } = require("node-global-key-listener");

  const bridge = new Bridge({ HID });
  const keyboardListener = new GlobalKeyboardListener();
  const pressedKeys = new Set();

  keyboardListener.addListener((event, down) => {
    const key = normalizeHotkeyName(event.name);
    if (event.state === "UP") {
      pressedKeys.delete(key);
      return undefined;
    }
    if (event.state !== "DOWN") return undefined;

    const ctrl = down["LEFT CTRL"] || down["RIGHT CTRL"];
    const alt = down["LEFT ALT"] || down["RIGHT ALT"];
    const action = HOTKEY_ACTIONS[key];
    if (!ctrl || !alt || !action) return undefined;

    if (pressedKeys.has(key) || !bridge.shouldAcceptHotkey(key)) return true;
    pressedKeys.add(key);
    bridge.log("info", `全局快捷键: ${key} -> ${action}`);
    bridge.enqueueAction(action).catch((error) => {
      bridge.log("error", "快捷键命令失败", error.message);
    });
    return true;
  });

  const server = createHttpServer(bridge);
  server.keepAliveTimeout = 65000;
  server.headersTimeout = 66000;

  server.listen(PORT, HOST, () => {
    bridge.log("info", `控制面板: http://${HOST}:${PORT}/`);
    bridge.log("info", "全局快捷键已启用: Ctrl+Alt+C/X/0-3/7-9/M/I/O/U/T/R");
    if (AUTO_CONNECT) bridge.openDevice();
  });

  const deviceTimer = setInterval(() => bridge.pollDevicePresence(), DEVICE_POLL_MS);
  deviceTimer.unref?.();

  let shuttingDown = false;
  const shutdown = (signal) => {
    if (shuttingDown) return;
    shuttingDown = true;
    bridge.log("info", `收到 ${signal}，正在关闭服务`);
    clearInterval(deviceTimer);
    if (typeof keyboardListener.kill === "function") {
      try { keyboardListener.kill(); } catch { /* no-op */ }
    }
    bridge.shutdown();
    server.close(() => process.exit(0));
    setTimeout(() => process.exit(1), 1500).unref();
  };

  process.on("SIGINT", () => shutdown("SIGINT"));
  process.on("SIGTERM", () => shutdown("SIGTERM"));
  process.on("uncaughtException", (error) => {
    bridge.log("error", "未捕获异常", error.stack || error.message);
    shutdown("uncaughtException");
  });
  process.on("unhandledRejection", (error) => {
    bridge.log("error", "未处理 Promise 拒绝", error?.stack || String(error));
  });

  return { bridge, server, keyboardListener };
}

if (require.main === module) startRuntime();

module.exports = {
  Bridge,
  Command,
  HOTKEY_ACTIONS,
  ProtocolType,
  VolumeTarget,
  buildHidFrame,
  createHttpServer,
  dx1DbToRaw,
  dx1VolumeCommand,
  makeOutputReport,
  normalizeHotkeyName,
  normalizeVolumeDb,
  validateCommandPayload,
};
