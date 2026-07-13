"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const {
  ProtocolType,
  buildHidFrame,
  dx1DbToRaw,
  dx1VolumeCommand,
  makeOutputReport,
  normalizeHotkeyName,
  normalizeVolumeDb,
  validateCommandPayload,
} = require("../server");

test("volume normalization follows DX1 half-dB rule above -10 dB", () => {
  assert.equal(normalizeVolumeDb(-9.74), -9.5);
  assert.equal(normalizeVolumeDb(-9.76), -10);
  assert.equal(normalizeVolumeDb(-30.4), -30);
  assert.equal(normalizeVolumeDb(5), 0);
  assert.equal(normalizeVolumeDb(-120), -99);
});

test("dB to raw conversion", () => {
  assert.equal(dx1DbToRaw(-99), 0);
  assert.equal(dx1DbToRaw(-30), 690);
  assert.equal(dx1DbToRaw(0), 990);
});

test("volume command encodes target", () => {
  assert.equal(dx1VolumeCommand(0), 0x7601);
  assert.equal(dx1VolumeCommand(1), 0x7602);
  assert.equal(dx1VolumeCommand(2), 0x7603);
});

test("HID frame and shifted output report layout", () => {
  const frame = buildHidFrame({
    protocolType: ProtocolType.writeNack,
    command: 0x7601,
    data: 690,
  });
  assert.equal(frame.length, 16);
  assert.deepEqual([...frame.slice(0, 8)], [0, 0x22, 0x33, 0x20, 1, 1, 0x76, 1]);
  assert.deepEqual([...frame.slice(8, 12)], [0, 0, 2, 0xb2]);
  assert.deepEqual([...frame.slice(14, 16)], [0x66, 0x77]);

  const report = makeOutputReport(frame, true);
  assert.equal(report.length, 17);
  assert.equal(report[0], 0);
  assert.deepEqual([...report.slice(1, 5)], [0x22, 0x33, 0x20, 1]);
  assert.equal(report[16], 0);
});

test("hotkey aliases normalize", () => {
  assert.equal(normalizeHotkeyName("numpad 1"), "1");
  assert.equal(normalizeHotkeyName("ONE"), "1");
  assert.equal(normalizeHotkeyName("m"), "M");
});

test("command payload validation", () => {
  assert.deepEqual(validateCommandPayload({ action: "connect" }), { action: "connect" });
  assert.deepEqual(validateCommandPayload({ action: "setVolume", value: -24.5 }), {
    action: "setVolume",
    value: -24.5,
  });
  assert.throws(() => validateCommandPayload({ action: "setInput", value: 2 }), /0 或 1/);
  assert.throws(() => validateCommandPayload({ action: "unknown" }), /未知 action/);
});

function createMockHid({ failWrite = false } = {}) {
  const writes = [];
  const closes = [];
  class FakeDevice {
    constructor(devicePath) {
      this.path = devicePath;
    }
    write(bytes) {
      if (failWrite) throw new Error("mock write failure");
      writes.push([...bytes]);
      return bytes.length;
    }
    close() {
      closes.push(this.path);
    }
  }
  return {
    module: {
      devices: () => [{
        vendorId: 0x152a,
        productId: 0x8750,
        path: "mock-dx1",
        manufacturer: "TOPPING",
        product: "DX1 II",
        interface: 2,
        usagePage: 0xff00,
      }],
      HID: FakeDevice,
    },
    writes,
    closes,
  };
}

const silentLogger = { log() {}, warn() {}, error() {} };

test("bridge commits volume state only after a successful HID write", async () => {
  const { Bridge } = require("../server");
  const mock = createMockHid();
  const bridge = new Bridge({ HID: mock.module, logger: silentLogger });

  const ok = await bridge.performAction("setVolume", -24);
  assert.equal(ok, true);
  assert.equal(bridge.state.connected, true);
  assert.equal(bridge.state.volumeDb, -24);
  assert.equal(bridge.state.commandCount, 1);
  assert.equal(mock.writes.length, 1);
});

test("bridge preserves prior volume when a HID write fails", async () => {
  const { Bridge } = require("../server");
  const mock = createMockHid({ failWrite: true });
  const bridge = new Bridge({ HID: mock.module, logger: silentLogger });

  const ok = await bridge.performAction("safeVolume");
  assert.equal(ok, false);
  assert.equal(bridge.state.volumeDb, -30);
  assert.equal(bridge.state.restoreVolumeDb, null);
  assert.equal(bridge.state.connected, false);
  assert.equal(bridge.state.failedCommandCount, 1);
});

test("safe-volume and restore-volume form a reversible macro", async () => {
  const { Bridge } = require("../server");
  const mock = createMockHid();
  const bridge = new Bridge({ HID: mock.module, logger: silentLogger });

  await bridge.performAction("setVolume", -18);
  await bridge.performAction("safeVolume");
  assert.equal(bridge.state.volumeDb, -40);
  assert.equal(bridge.state.restoreVolumeDb, -18);

  await bridge.performAction("restoreVolume");
  assert.equal(bridge.state.volumeDb, -18);
  assert.equal(mock.writes.length, 3);
});

test("HTTP server serves dashboard and validates command content type", async (t) => {
  const { Bridge, createHttpServer } = require("../server");
  const mock = createMockHid();
  const bridge = new Bridge({ HID: mock.module, logger: silentLogger });
  const server = createHttpServer(bridge);

  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  t.after(() => new Promise((resolve) => server.close(resolve)));

  const address = server.address();
  const base = `http://127.0.0.1:${address.port}`;

  const pageResponse = await fetch(`${base}/`);
  assert.equal(pageResponse.status, 200);
  assert.match(await pageResponse.text(), /DX1 II 控制中心/);

  const invalidResponse = await fetch(`${base}/api/command`, {
    method: "POST",
    headers: { "Content-Type": "text/plain" },
    body: "{}",
  });
  assert.equal(invalidResponse.status, 415);

  const commandResponse = await fetch(`${base}/api/command`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ action: "setVolume", value: -20 }),
  });
  assert.equal(commandResponse.status, 200);
  const commandPayload = await commandResponse.json();
  assert.equal(commandPayload.ok, true);
  assert.equal(commandPayload.state.volumeDb, -20);
});


test("firmware and bridge hotkey maps stay synchronized", () => {
  const fs = require("node:fs");
  const path = require("node:path");
  const { HOTKEY_ACTIONS } = require("../server");
  const firmware = fs.readFileSync(path.join(__dirname, "..", "src", "main.cpp"), "utf8");
  const block = firmware.match(/constexpr KeyAction kActions\[\]\s*=\s*\{([\s\S]*?)\n\};/);
  assert.ok(block, "firmware kActions block not found");
  const firmwareKeys = [...block[1].matchAll(/\{'(.)',\s*'(.?)',/g)].map((match) => match[1].toUpperCase()).sort();
  const bridgeKeys = Object.keys(HOTKEY_ACTIONS).sort();
  assert.deepEqual(firmwareKeys, bridgeKeys);
});
