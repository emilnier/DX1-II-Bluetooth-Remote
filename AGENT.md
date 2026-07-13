# Project Notes

## Components

- `src/main.cpp`: M5Stack Cardputer-Adv BLE remote firmware.
- `server.js`: localhost Node.js bridge, global hotkeys, USB HID, reconnect and SSE.
- `public/`: dashboard assets.
- `test/server.test.js`: hardware-independent protocol and transaction tests.
- `OPTIMIZATION_NOTES.md`: design rationale and future ideas.

## Commands

```bash
npm install
npm test
npm start
pio run
```

## Invariants

- Firmware `kActions` must stay synchronized with `server.js` `HOTKEY_ACTIONS`.
- Only commit UI state after a successful HID write.
- Keep HTTP bound to `127.0.0.1` unless a separate authenticated remote-control design is added.
- Avoid direct Preferences writes inside keyboard event handling; use deferred persistence.
- Keep firmware queues bounded and avoid long-lived dynamic `String` state.
- Do not enable manual ESP light sleep or display-controller sleep by default without hardware testing.
