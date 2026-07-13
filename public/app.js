"use strict";

const $ = (selector) => document.querySelector(selector);
const stateEls = {
  statusDot: $("#statusDot"),
  statusText: $("#statusText"),
  deviceText: $("#deviceText"),
  lastAction: $("#lastAction"),
  commandCount: $("#commandCount"),
  queueDepth: $("#queueDepth"),
  reconnectMode: $("#reconnectMode"),
  errorBanner: $("#errorBanner"),
  volumeReadout: $("#volumeReadout"),
  volumeSlider: $("#volumeSlider"),
  volumeInput: $("#volumeInput"),
  muteButton: $("#muteButton"),
  log: $("#log"),
};

let latestState = null;
let eventSource = null;
let fallbackTimer = null;
let locallyClearedLogRevision = -1;

function setBusy(isBusy) {
  document.querySelectorAll("button[data-action], #applyVolume").forEach((button) => {
    button.disabled = isBusy;
  });
}

function formatDevice(device) {
  if (!device) return "未打开 USB HID 设备";
  const base = [device.manufacturer, device.product].filter(Boolean).join(" ");
  const detail = device.interface === null ? "" : ` · 接口 ${device.interface}`;
  return `${base || "DX1 II"}${detail}`;
}

function renderLogs(logs, revision) {
  if (revision <= locallyClearedLogRevision) return;
  stateEls.log.replaceChildren();
  for (const entry of logs || []) {
    const line = document.createElement("div");
    line.className = `log-entry ${entry.level || "info"}`;
    const time = new Date(entry.time).toLocaleTimeString();
    const meta = entry.meta === undefined ? "" : ` · ${typeof entry.meta === "string" ? entry.meta : JSON.stringify(entry.meta)}`;
    line.textContent = `[${time}] ${entry.message}${meta}`;
    stateEls.log.append(line);
  }
  stateEls.log.scrollTop = stateEls.log.scrollHeight;
}

function applyState(state) {
  latestState = state;
  stateEls.statusDot.classList.toggle("connected", !!state.connected);
  stateEls.statusDot.classList.toggle("error", !state.connected && !!state.lastError);
  stateEls.statusText.textContent = state.connected ? "DX1 II 已连接" : "DX1 II 未连接";
  stateEls.deviceText.textContent = formatDevice(state.device);
  stateEls.lastAction.textContent = state.lastAction || "—";
  stateEls.commandCount.textContent = String(state.commandCount ?? 0);
  stateEls.queueDepth.textContent = String(state.queueDepth ?? 0);
  stateEls.reconnectMode.textContent = state.manualDisconnected ? "已暂停" : "自动";

  const db = Number(state.volumeDb ?? -30);
  stateEls.volumeReadout.textContent = db.toFixed(1);
  if (document.activeElement !== stateEls.volumeInput) stateEls.volumeInput.value = db.toFixed(1);
  if (document.activeElement !== stateEls.volumeSlider) stateEls.volumeSlider.value = String(db);

  $("#input0").classList.toggle("active", state.input === 0);
  $("#input1").classList.toggle("active", state.input === 1);
  $("#output0").classList.toggle("active", state.output === 0);
  $("#output1").classList.toggle("active", state.output === 1);
  $("#output2").classList.toggle("active", state.output === 2);
  stateEls.muteButton.classList.toggle("active", !!state.muted);
  stateEls.muteButton.textContent = state.muted ? "取消静音" : "静音切换";

  stateEls.errorBanner.hidden = !state.lastError;
  stateEls.errorBanner.textContent = state.lastError || "";
  renderLogs(state.logs, state.revision ?? 0);
}

async function fetchState() {
  const response = await fetch("/api/state", { cache: "no-store" });
  if (!response.ok) throw new Error(`状态请求失败 (${response.status})`);
  applyState(await response.json());
}

async function sendCommand(action, value) {
  setBusy(true);
  try {
    const response = await fetch("/api/command", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(value === undefined ? { action } : { action, value }),
    });
    const payload = await response.json();
    if (payload.state) applyState(payload.state);
    if (!response.ok || !payload.ok) throw new Error(payload.error || `命令失败 (${response.status})`);
  } catch (error) {
    stateEls.errorBanner.hidden = false;
    stateEls.errorBanner.textContent = error.message;
  } finally {
    setBusy(false);
  }
}

function startFallbackPolling() {
  if (fallbackTimer) return;
  fetchState().catch(() => undefined);
  fallbackTimer = setInterval(() => fetchState().catch(() => undefined), 3000);
}

function stopFallbackPolling() {
  if (!fallbackTimer) return;
  clearInterval(fallbackTimer);
  fallbackTimer = null;
}

function connectEventStream() {
  eventSource?.close();
  eventSource = new EventSource("/api/events");
  eventSource.addEventListener("state", (event) => {
    stopFallbackPolling();
    try { applyState(JSON.parse(event.data)); } catch { /* ignore malformed event */ }
  });
  eventSource.onerror = () => startFallbackPolling();
}

document.addEventListener("click", (event) => {
  const button = event.target.closest("button[data-action]");
  if (!button) return;
  const value = button.dataset.value === undefined ? undefined : Number(button.dataset.value);
  sendCommand(button.dataset.action, value);
});

stateEls.volumeSlider.addEventListener("input", () => {
  const value = Number(stateEls.volumeSlider.value);
  stateEls.volumeReadout.textContent = value.toFixed(1);
  stateEls.volumeInput.value = value.toFixed(1);
});
stateEls.volumeSlider.addEventListener("change", () => sendCommand("setVolume", Number(stateEls.volumeSlider.value)));
stateEls.volumeInput.addEventListener("input", () => {
  const value = Number(stateEls.volumeInput.value);
  if (Number.isFinite(value)) stateEls.volumeReadout.textContent = value.toFixed(1);
});
stateEls.volumeInput.addEventListener("keydown", (event) => {
  if (event.key === "Enter") sendCommand("setVolume", Number(stateEls.volumeInput.value));
});
$("#applyVolume").addEventListener("click", () => sendCommand("setVolume", Number(stateEls.volumeInput.value)));
$("#clearLog").addEventListener("click", () => {
  locallyClearedLogRevision = latestState?.revision ?? 0;
  stateEls.log.replaceChildren();
});

connectEventStream();
fetchState().catch(() => startFallbackPolling());
