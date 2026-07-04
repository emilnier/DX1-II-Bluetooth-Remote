# DX1 II Bluetooth Remote (M5Stack Cardputer-Adv)

## 架构（推荐方案：dx1_bridge，不需要浏览器焦点）

```
Cardputer-Adv 按键
      │
      │ DX1控制键: 蓝牙 BLE 键盘发 Ctrl+Alt+<键>
      │ 媒体键(P/N/B): 蓝牙 BLE "Consumer Control" 媒体键
      ▼
   PC 蓝牙配对
      │
      ├─ DX1控制键 ──► dx1_bridge/server.js (Node 后台服务)
      │                 · node-global-key-listener 全局捕获快捷键
      │                 · node-hid 直接写入 DX1 II (USB HID)
      │                 · 不管哪个窗口在前台都能收到
      │
      └─ 媒体键 ──────► 操作系统媒体会话 (系统级, 天然全局)
                          控制当前正在播放的播放器 (Spotify/浏览器等)
```

**为什么之前需要浏览器聚焦：** 旧方案里网页自己监听 `keydown` 事件，浏览器只有在标签页
拥有系统焦点时才能收到键盘事件。

**现在的解法：** 把"接收快捷键 + 发送 HID 命令给 DX1 II"这件事整体搬到一个后台 Node.js
进程（`dx1_bridge/server.js`）里，不再依赖浏览器：

- 用 `node-global-key-listener` 在操作系统层面全局抓取 Ctrl+Alt+<键>（不管当前哪个窗口
  在前台，甚至该终端窗口被最小化也没关系）。
- 用 `node-hid` 直接打开 DX1 II 的 USB HID 设备，跳过浏览器 WebHID，直接发送和网页版
  完全相同的 16 字节协议帧。
- 网页仪表盘 (`dx1_bridge/public/index.html`) 变成纯手动控制的可视化面板，通过本地
  HTTP API (`/api/state`、`/api/command`) 跟同一个后台进程通信，方便的时候点点鼠标也行，
  但不再是必需品。

媒体播放控制（播放/暂停/上一曲/下一曲）完全不需要这个后台服务 —— Cardputer 把它们发送成
真正的蓝牙"媒体键"（HID Consumer Control），操作系统本身就会全局路由给当前的媒体会话，
这是操作系统自带的能力，跟浏览器、跟焦点都没关系。

## 按键映射

| Cardputer 按键 | 发送内容 | 功能 | 是否需要焦点/后台服务 |
|---|---|---|---|
| C | Ctrl+Alt+C | 连接 DX1 II | 需要 dx1_bridge 运行中（无需焦点）|
| X | Ctrl+Alt+X | 断开连接 | 同上 |
| 1 / 2 | Ctrl+Alt+1/2 | 音量 -/+ | 同上 |
| M | Ctrl+Alt+M | 静音切换 | 同上 |
| I | Ctrl+Alt+I | 输入切换 (USB/OPT) | 同上 |
| O | Ctrl+Alt+O | 输出切换 (耳放/LO/ALL) | 同上 |
| P | 媒体键 播放/暂停 | 播放/暂停当前播放器 | 完全不需要 |
| N | 媒体键 下一曲 | 下一曲 | 完全不需要 |
| B | 媒体键 上一曲 | 上一曲 | 完全不需要 |
| Fn+S | (本地设置) | 自动熄屏 开/关 | 仅本机 Cardputer，不涉及蓝牙 |
| Fn+, / Fn+. | (本地设置) | 自动熄屏时间 缩短/延长 | 仅本机 Cardputer，不涉及蓝牙 |

Cardputer 屏幕上会实时显示这份对照表、蓝牙连接状态、当前自动熄屏设置、以及最近一次动作。

## 屏幕显示（v3 更新：修了中文乱码，改成两页布局）

第一版固件里中文用的是 M5GFX 默认字体，而默认字体压根不带中文字形，所以中文全都变成了
残缺的乱码方块——这不是逻辑 bug，是字体问题。现在改用 M5GFX 自带的 `efontCN` 中文点阵字体
（`setFont(&fonts::efontCN_14)` 等），中文正常显示。

因为屏幕只有 240×135，塞不下"连接状态 + 自动熄屏 + 完整按键说明"这么多内容还保持清晰，
所以分成了两页，用 `Fn+/` 切换：

- **状态页（默认）**：标题、蓝牙连接状态、已连接设备的蓝牙地址、自动熄屏 开/关 与时长、
  最近一次操作。
- **按键说明页**：全部按键 + 对应功能，两列布局。

关于"已连接蓝牙设备的名称"：标准 BLE HID 里外设（Cardputer）只能读到已连接中心设备
（你的电脑）的蓝牙地址，读不到它的"设备名"（比如 "DESKTOP-ABC123"）——那个名字存在
对方的 GAP 服务里，外设想读到它得反过来同时以"中心设备"身份连回对方，在单射频的 ESP32
上跟已有的 HID 连接抢同一个连接不太稳定，所以这里选择显示地址而不是折腾这个。地址已经
足够在你有多台电脑时确认配对到了正确的那台。

## 文件结构

```
dx1_bridge/                 # 【推荐】后台桥接服务：全局热键 + 直连 DX1 II
  server.js
  package.json
  public/index.html         # 手动控制仪表盘（可选）
dx1_web/                    # 旧方案：纯浏览器 WebHID，需要标签页保持焦点
  index.html
  server.js
cardputer_remote/           # Cardputer-Adv 固件 (PlatformIO)
  platformio.ini
  src/main.cpp
```

`dx1_web/` 保留作为不想跑后台服务时的备用手动控制方式（仍然需要浏览器标签页聚焦才能响应
Cardputer 快捷键）；日常使用建议直接用 `dx1_bridge/`。

## 部署 dx1_bridge（PC 端后台服务）

```bash
cd dx1_bridge
npm install
npm start
```

- `node-hid` 需要原生编译，多数平台有预编译二进制，正常 `npm install` 即可；如果失败，
  按提示安装对应平台的编译工具链（Windows 需要 `windows-build-tools`/VS Build Tools，
  macOS 需要 Xcode Command Line Tools）。
- `node-global-key-listener` 在 Windows 上一般开箱即用；**macOS** 需要在
  "系统设置 → 隐私与安全性 → 输入监控/辅助功能" 里给终端 App（或打包后的可执行文件）授权，
  否则收不到全局按键；**Linux** 通常需要 root 权限或给当前用户加 `input` 组权限访问
  `/dev/input`。
- 想开机自启可以用 `pm2`、Windows 任务计划程序，或 macOS 的 `launchd`，把
  `node server.js` 设为开机/登录后自动运行。
- 仪表盘地址：`http://127.0.0.1:8787/`（纯手动控制用，日常可以不打开）。

## 关于 HID 报告 ID 的一个提示

`node-hid` 的 `write()` 和浏览器 WebHID 的 `sendReport(reportId, payload)` 在"报告 ID
怎么传"这件事上不完全一样，`server.js` 里的 `PREFIX_REPORT_ID_BYTE` 常量控制是否在写入
数据前手动加一个 `0x00` 前缀字节。协议帧本身已经和网页版验证过可用的版本完全一致，如果
连接后命令发送了但 DX1 II 没反应，先把这个常量改成 `false` 试试，两种都不行的话，对照
`dx1_web/index.html` 连接时打印的 TX 日志字节，跟 `dx1_bridge` 控制台打印的 TX 字节做
个比对，确认是否一致。

## 烧录 Cardputer 固件（PlatformIO）

1. 安装 [PlatformIO](https://platformio.org/)（VS Code 插件或 CLI 均可）。
2. 用 VS Code 打开 `cardputer_remote/` 文件夹。
3. Cardputer-Adv 进入下载模式：侧面电源开关拨到 `OFF`，按住 `G0` 键的同时插入 USB-C 供电，
   然后松开 `G0`。
4. 点击 PlatformIO 的 Upload（或运行 `pio run -t upload`）。
5. 首次编译会自动下载依赖：`M5Cardputer`、`NimBLE-Arduino`、`ESP32 BLE Keyboard`。

## 配对与使用

1. 固件启动后屏幕显示 "蓝牙: 等待PC连接..."。
2. 在电脑蓝牙设置里添加新设备，选择 `DX1II Remote`，完成标准蓝牙键盘配对（无需 PIN）。
3. 配对成功后屏幕状态变为绿色 "蓝牙: 已连接"。
4. 启动 `dx1_bridge`（见上）；首次用 Ctrl+Alt+C 或仪表盘的"连接"按钮让它打开 DX1 II。
5. 之后随时按 Cardputer 上的键即可控制，不需要打开任何浏览器窗口，也不需要它处于焦点。
6. 播放/暂停/上一曲/下一曲随时可用，只要电脑上有播放器在响应系统媒体键即可。

## 自动熄屏

- `Fn+S` 切换开/关（默认开）。
- `Fn+,` / `Fn+.` 在预设时长 10s / 30s / 60s / 120s / 300s 之间循环切换。
- 设置会保存在 Cardputer 的 NVS 存储里，断电重启后依然生效。
- 熄屏只是关闭背光，蓝牙连接和按键监听不受影响，按任意键即可立刻唤醒并执行对应动作。

## 注意事项

- 只有一个进程能独占打开 DX1 II 的 USB HID 设备：用 `dx1_bridge` 时就不要同时用
  `dx1_web` 里的"连接"按钮，两边会抢设备。
- `cardputer_remote/src/main.cpp` 里的 `kActions` 表要和 `dx1_bridge/server.js` 里的
  `HOTKEY_ACTIONS` 保持一致，以后加新功能（比如滤波器/高增益）两边都要同步改。
- WebHID 版本 (`dx1_web/`) 首次 `requestDevice()` 仍需要一次鼠标点击完成设备选择弹窗，
  这是浏览器安全限制；`dx1_bridge` 完全绕开了这个限制，因为它不经过浏览器。
