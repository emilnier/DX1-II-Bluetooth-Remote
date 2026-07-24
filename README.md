# DX1 II Bluetooth Remote v2.1

M5Stack Cardputer-Adv 通过 BLE 键盘快捷键控制 PC 端后台桥接服务，再由桥接服务通过 USB HID 直接控制 TOPPING DX1 II。媒体键由操作系统直接处理，不依赖网页焦点。

## 项目结构

```text
.
├── platformio.ini            # Cardputer-Adv 固件构建配置
├── src/main.cpp              # 低功耗 BLE 遥控固件
├── server.js                 # PC 端全局热键 + USB HID 桥接服务
├── public/
│   ├── index.html            # 本地控制面板
│   ├── app.js                # SSE 实时状态与控制逻辑
│   ├── styles.css            # 响应式界面样式
│   └── storage-audit.html    # 中文存储寿命审计与固件优化页面
├── test/server.test.js       # HID 协议和桥接事务测试
├── OPTIMIZATION_NOTES.md     # 优化审计、设计取舍和后续脑暴
├── package.json
└── package-lock.json
```

## v2 已完成的核心改进

### Cardputer 固件

- 用固定容量 HID 事件队列替代阻塞式 `delay(15)` 组合键发送。
- 组合键按下、释放和事件间隔均由非阻塞状态机管理。
- 音量长按支持分阶段加速，快速调节时仍保持有界队列。
- 长期状态文本改用固定字符数组，减少堆碎片风险。
- 屏幕刷新合并到最多约 30 ms 一次，避免连续全屏 SPI 重绘。
- 设置停止变化 2.5 秒后统一写入 NVS，降低频繁切换造成的 Flash 写放大。
- 默认省电模式：80 MHz CPU、BLE -3 dBm；可用 `Fn+E` 切换 160 MHz / +3 dBm 响应模式。
- BLE 广播间隔设为 100–150 ms，在可接受的重连速度下减少持续广播负担。
- 关闭 Wi-Fi、扬声器、麦克风、IMU、LED 和未使用外设。
- 屏幕关闭后降低主循环和电池轮询频率；正在发送 HID 时自动临时提高循环频率。
- 低于 10% 且未充电时自动限制背光亮度。
- 新增三页界面：状态、按键说明、运行诊断。
- 新增“熄屏首键仅唤醒”保护模式，避免摸黑误操作。

### PC 桥接服务

- 修复原工程页面位于根目录、服务却固定读取 `public/` 导致的 404 问题。
- 通过 `HID.devices()` 枚举候选接口，优先尝试厂商自定义 usage page。
- USB 拔出、写入失败或重新插入后自动检测并重连。
- 用户手动断开后暂停自动重连，重新连接即可恢复。
- 所有命令进入串行 Promise 队列，保证命令顺序和最小 HID 写间隔。
- 只有 HID 写入成功后才提交音量、静音、输入和输出状态。
- 增加请求体大小限制、JSON 校验、动作白名单和数值范围校验。
- 服务固定监听 `127.0.0.1`，不会默认暴露到局域网。
- 使用 Server-Sent Events 实时推送状态，代替每秒轮询；断线时自动降级为低频轮询。
- 增加设备信息、命令计数、失败计数、队列深度、错误和日志展示。
- 增加安全音量、恢复音量、音量预设、输入直选和输出直选。
- 协议与事务逻辑可在无硬件环境中测试。

## v2.1 存储寿命审计与固件优化

本版本新增完整的「中文存储寿命审计与固件优化页面」（`public/storage-audit.html`），并据此完成一轮存储保护强化。

### 核心结果

- 确认原固件没有挂载或写入 TF 卡，运行期 TF 写入为零。
- 将 5 个 Preferences 键合并为单个 `uint32_t` NVS 条目。
- 加入写前比较、30 秒去抖、5 分钟最大延后、60 秒最小写入间隔。
- 电量不高于 5% 且未充电时延后 NVS 写入。
- 支持旧设置键只读迁移，并增加保存成功 / 失败诊断。
- 校正了「普通 esptool 上传会自动差分写入」的说法，明确需要 `--diff-with`。
- 提供内部 Flash、TF 卡、开发刷写和 FRAM 硬件升级的完整保护清单。
- 支持源码复制、完整 `.ino` 下载、清单筛选和写入量估算。

## 数据路径

```text
Cardputer-Adv
  ├─ Ctrl+Alt+<键> ── BLE Keyboard ──► PC 全局键盘钩子
  │                                      │
  │                                      ▼
  │                              server.js 命令队列
  │                                      │ USB HID
  │                                      ▼
  │                                  DX1 II
  │
  └─ P / N / B ───── BLE Consumer Control ──► 操作系统媒体会话
```

## Cardputer 按键

### DX1 II 控制键

| 按键 | 功能 |
|---|---|
| `C` | 连接 DX1 II，并恢复自动重连 |
| `X` | 手动断开，并暂停自动重连 |
| `1` / `2` | 音量降低 / 提高；长按逐步加速 |
| `3` | 在 -60、-50、-40、-30、-20、-10 dB 之间循环 |
| `0` | 切换到安全音量，默认 -40 dB，并记录此前音量 |
| `R` | 恢复使用 `0` 前记录的音量 |
| `M` | 静音切换 |
| `I` | USB / OPT 输入切换 |
| `O` | 耳放 / LO / ALL 输出循环 |
| `U` / `T` | 直接选择 USB / OPT |
| `7` / `8` / `9` | 直接选择耳放 / LO / ALL |

### 系统媒体键

| 按键 | 功能 |
|---|---|
| `P` | 播放 / 暂停 |
| `N` | 下一曲 |
| `B` | 上一曲 |

### Cardputer 本地设置

| 组合键 | 功能 |
|---|---|
| `Fn+S` | 自动熄屏开 / 关 |
| `Fn+,` / `Fn+.` | 缩短 / 延长熄屏时间 |
| `Fn+L` | 18、32、50、75、100 亮度档位循环 |
| `Fn+E` | 省电 / 高响应模式切换 |
| `Fn+W` | 熄屏首键“仅唤醒”或“唤醒并执行” |
| `Fn+/` | 状态页、按键页、诊断页循环 |

本地设置保存在 `dx1remote` NVS 命名空间。为了降低 Flash 磨损，连续调整期间不会每次按键都立即写入，而是在最后一次变化约 2.5 秒后统一保存。

## 安装与运行 PC 桥接

建议使用当前 LTS 版 Node.js。

```bash
npm install
npm test
npm start
```

浏览器打开：

```text
http://127.0.0.1:8787/
```

控制面板只是可选入口。Cardputer 的全局快捷键不要求浏览器打开或处于前台。

### 常用环境变量

| 变量 | 默认值 | 说明 |
|---|---:|---|
| `PORT` | `8787` | 本地面板端口 |
| `VOLUME_STEP_DB` | `1` | 每次音量加减步长，范围 0.5–10 dB |
| `SAFE_VOLUME_DB` | `-40` | 安全音量，范围 -99–0 dB |
| `COMMAND_GAP_MS` | `18` | 两次 USB HID 写入之间的最小间隔 |
| `DEVICE_POLL_MS` | `3000` | USB 在线检测与重连周期 |
| `PREFIX_REPORT_ID_BYTE` | `true` | 是否为 `node-hid.write()` 添加报告 ID 前缀 |
| `AUTO_CONNECT` | `true` | 启动时是否自动打开 DX1 II |
| `DX1_HID_PATH` | 空 | 多接口环境下强制使用指定 HID path |
| `DEBUG_HID` | `false` | 是否在日志中输出完整 TX 字节 |

Windows PowerShell 示例：

```powershell
$env:SAFE_VOLUME_DB = "-45"
$env:VOLUME_STEP_DB = "0.5"
npm start
```

macOS/Linux 示例：

```bash
SAFE_VOLUME_DB=-45 VOLUME_STEP_DB=0.5 npm start
```

## 烧录 Cardputer 固件

1. 安装 VS Code + PlatformIO，或安装 PlatformIO Core。
2. 在项目根目录执行：

```bash
pio run
pio run -t upload
```

3. 首次烧录后，在电脑蓝牙设置中配对 `DX1II Remote`。
4. 启动 PC 桥接服务。

`platformio.ini` 当前锁定：

- `espressif32@6.7.0`
- `NimBLE-Arduino@1.4.1`
- `ESP32 BLE Keyboard@0.3.2`
- `M5Cardputer@1.1.1`

## 本地 API

### 查询状态

```http
GET /api/state
```

### 健康检查

```http
GET /api/health
```

连接 DX1 II 时返回 200，否则返回 503。

### 实时状态

```http
GET /api/events
Accept: text/event-stream
```

### 发送命令

```http
POST /api/command
Content-Type: application/json
```

示例：

```json
{"action":"setVolume","value":-25}
```

支持动作：

```text
connect, disconnect
volumeUp, volumeDown, volumePreset, safeVolume, restoreVolume, setVolume
muteToggle
inputToggle, inputUsb, inputOptical, setInput
outputCycle, outputHeadphone, outputLineOut, outputAll, setOutput
```

## 测试

```bash
npm test
```

当前测试覆盖：

- dB 取整规则与 DX1 原始值换算。
- 16 字节协议帧和 `node-hid` 输出报告布局。
- 音量命令目标编码。
- 热键名称归一化。
- API 请求动作与范围校验。
- HID 成功写入后的事务提交。
- HID 写入失败时不污染音量和恢复记录。
- 安全音量与恢复音量宏。
- 本地 HTTP 页面、内容类型校验和命令接口。
- 固件与 PC 桥接热键映射一致性。

当前版本在无硬件环境中通过 11 项自动测试；完整验证记录见 `VALIDATION.md`。

## 重要限制

1. **桥接状态是推定状态。** 当前协议实现只写命令，没有从设备读取实际音量、静音、输入和输出。若在 DX1 II 旋钮或其他程序中修改，面板可能暂时不同步。
2. **USB HID 独占。** 不要同时运行其他会打开同一 HID 接口的工具。
3. **BLE 发射功率。** 省电模式使用 -3 dBm，适合桌面近距离；隔墙或距离较远时使用 `Fn+E` 切到 +3 dBm。
4. **屏幕控制器深度休眠默认关闭。** 当前只关闭背光，优先稳定性。`Display.sleep()` 在不同批次屏幕控制器上的唤醒表现应由实机验证后再开启。
5. **硬件最终验证仍必需。** 无硬件环境可验证协议、Node 逻辑和固件语法，但无法替代实际配对、功耗、USB 接口选择和长时间稳定性测试。

## 故障排查

### 面板打开后显示未连接

- 检查 DX1 II 是否通过 USB 连接。
- 检查是否有其他程序占用 HID。
- 在终端观察 `npm start` 日志。
- Linux 检查 udev 权限；macOS 检查输入监控/辅助功能权限。

### 日志显示 TX，但设备不响应

先切换报告 ID 前缀：

```bash
PREFIX_REPORT_ID_BYTE=false npm start
```

如果设备存在多个 HID 接口，可先用 `node-hid` 的设备枚举工具找到正确 path，再设置 `DX1_HID_PATH`。

### Cardputer 近距离稳定，远距离断连

按 `Fn+E` 切换到高响应模式，BLE 发射功率由 -3 dBm 提高到 +3 dBm，CPU 由 80 MHz 提高到 160 MHz。

### 熄屏时误触发操作

按 `Fn+W` 开启“熄屏首键仅唤醒”。

## 硬件存储寿命极大化保护方案

### ESP32-S3FN8

#### 1. 内部 Flash 保护策略 (NVS & 文件系统)

- **NVS 用途限制：** 仅用于存放小的配置类数据。严禁使用 NVS 存储大字符串或 Blob 数据。对于较大的二进制块，请改用基于磨损均衡库的 FAT 或 LittleFS。
- **使用 LittleFS 替代 SPIFFS：** 官方已停止维护 SPIFFS。LittleFS 提供更好的掉电恢复能力、动态磨损均衡，以及更低的 RAM 占用，是 2024+ 年新项目的默认推荐。
- **分区冗余：** 划分分区时切忌 “刚刚好”。分区必须留有足够的冗余空间。一旦剩余空间耗尽，磨损均衡将失去空闲扇区进行轮换，导致磨损均衡失效，反而加速损坏。
- **高频写入 RAM 缓冲 (Debounce)：** 高频变化的小值（如秒级计数器或 UI 高频调节的值）切勿直接写入 NVS 的同一个 key。需在内存中设置脏标记 (dirty flag) 进行缓冲，待空闲或经过特定阈值时间（如 2.5 秒）后再一并落盘持久化。

#### 2. TF 卡 (MicroSD) 读写保护策略

- **RAM 缓冲攒批写入：** 运行时写入（如采集传感器或红外码存储 JSON）应先攒到 RAM 缓冲区，达到数量或时间阈值后再一次性 flush（刷入），避免每次小事件发生时立即落盘。这样还能利用双缓冲降低断电数据损坏风险。
- **避免全量重写索引：** 对于索引或清单类文件，优先使用追加写入 (Append Mode)，或在攒够一批改动后在内存重建后再写入，绝不可改动一条数据就整体覆写一个巨大的 JSON 文件。
- **保留 10%~15% 空闲空间：** SD 卡主控的磨损均衡算法在空间极度紧张时效果会大打折扣。始终保持 15% 以上的可用空间。
- **连续大块写入：** 尽量将数据攒成较大的连续块写入，减少零碎的随机小写入，防止因频繁更新 FAT 表而产生写入放大。

#### 3. 硬件选型与开发刷写习惯

- **高耐久 TF 卡：** 长期高频写入场景（如日志）应抛弃普通 TLC/QLC 消费卡（3000~5000 次擦写，QLC 更低），改用高耐久卡（High Endurance、PRO Endurance 等工业级 MLC/pSLC/SLC 存储卡），寿命提升至 3 万到 10 万次以上。
- **外置 FRAM 模组：** 若确实存在需要 “秒级持久化” 的数据，最好的方案是通过 I2C/SPI 挂载一颗 FRAM (铁电存储器) 模块，它几乎没有擦写次数上限，可以彻底解决闪存磨损焦虑。
- **克制 “整片擦除”：** 开发阶段的日常编译上传 (build+upload) 仅重写 App 分区即可。避免习惯性使用 erase_flash 全盘擦除，除非必须清空 NVS 或更改分区表。
- **谨慎刷写数据分区：** 调试时若顺手把文件系统镜像 (SPIFFS/LittleFS data) 一起烧录，会导致整个数据分区被擦写。如非必要，切勿频繁刷写数据分区镜像。

## 附录

生成 `merged.bin` 文件：

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 merge_bin -o merged.bin 0x0000 .pio/build/m5stack-cardputer/bootloader.bin 0x8000 .pio/build/m5stack-cardputer/partitions.bin 0x10000 .pio/build/m5stack-cardputer/firmware.bin
```
