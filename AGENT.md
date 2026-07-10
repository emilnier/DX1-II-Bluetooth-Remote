# AGENT.md

## 项目概览

这是一个围绕 DX1 II 的三部分工程：

- [src/main.cpp](src/main.cpp) 是 Cardputer-Adv 固件，负责 BLE 键盘/媒体键发送、屏幕状态页、Fn 层设置、自动熄屏和 Preferences 持久化。
- [server.js](server.js) 是 PC 端 Node 桥接服务，负责全局热键捕获和通过 USB HID 直连 DX1 II。
- [index.html](index.html) 是浏览器手动控制页，作为辅助面板，不是主路径。

项目根目录已扁平化；[README.md](README.md) 里仍保留了旧的子目录描述，优先以根目录文件为准。

## 构建与运行

- 固件：在仓库根目录运行 PlatformIO，常用命令是 `pio run`、`pio run -t upload`、`pio device monitor`。
- PC 端桥接：先在根目录运行 `npm install`，然后 `npm start`。
- 这个仓库没有配置自动化测试；主要验证方式是固件编译、设备烧录和桥接服务启动。

## 关键约定

- `src/main.cpp` 里的热键动作表必须和 `server.js` 里的 `HOTKEY_ACTIONS` 保持同步，新增功能时两边一起改。
- DX1 II 的 USB HID 设备一次只能被一个进程独占打开；不要同时运行 Node 桥接和浏览器 WebHID 连接。
- 如果命令发送了但设备没反应，优先检查 `server.js` 里的 `PREFIX_REPORT_ID_BYTE`。
- 固件的本地设置保存在 Preferences 的 `dx1remote` 命名空间里，重启后仍然有效。

## 修改时的优先参考

- 协议、按键映射和使用方式：先看 [README.md](README.md)。
- 固件逻辑：先看 [src/main.cpp](src/main.cpp)。
- PC 端桥接和 HID 协议细节：先看 [server.js](server.js)。

## 常见风险

- Windows 上 `node-hid` 可能需要原生编译工具链。
- Cardputer 屏幕显示依赖 M5GFX 字体，修改 UI 时要注意中文显示和 240x135 的尺寸限制。
- 旧的浏览器控制页需要焦点，而 Node 桥接不需要；新改动应默认围绕桥接服务设计。