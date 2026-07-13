# 验证记录

## 已完成

### PC 桥接与网页

执行命令：

```bash
npm run check
```

结果：

- `server.js` Node.js 语法检查通过。
- `public/app.js` Node.js 语法检查通过。
- 11/11 项自动测试通过，0 失败。
- 测试覆盖协议帧、报告 ID 布局、音量换算、输入校验、事务提交、失败回滚、安全音量恢复、HTTP 页面、内容类型和固件/桥接热键映射同步。

### 固件主体

在本地伪硬件接口头文件下执行 `g++ -fsyntax-only`，用于检查 C++ 语法、类型、声明和主要调用关系；结果通过，无编译错误。

同时按 `platformio.ini` 锁定版本核对了以下关键接口：

- M5Cardputer 键盘状态、按键查询和显示接口。
- NimBLE 1.4.1 发射功率枚举与广播间隔接口。
- ESP32 BLE Keyboard 媒体键报告接口。

## 尚需实机完成

沙箱无法稳定下载完整 PlatformIO/ESP32 外部依赖，因此未完成真实 ESP32-S3 工具链的最终链接和烧录。交付后应在可联网的本机项目根目录执行：

```bash
pio run
pio run -t upload
```

随后按 `OPTIMIZATION_NOTES.md` 的验证矩阵测试：

1. BLE 配对、断连与主机睡眠恢复。
2. DX1 II 多 HID 接口选择及 USB 热插拔。
3. 亮屏、熄屏、连接、广播等待等状态的实际电流。
4. 连续长按、快速切换、24–72 小时稳定性。
5. `PREFIX_REPORT_ID_BYTE=true/false` 对当前设备固件的实际适配。

## 结论边界

自动测试与语法级检查说明软件逻辑和协议构造在当前测试范围内一致，但不能替代真实 Cardputer、电脑蓝牙栈和 DX1 II 硬件上的最终验证。
