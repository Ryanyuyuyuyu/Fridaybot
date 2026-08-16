# M5Stack StopWatch + Codex Micro

[English](README.md)

这个分支把兼容 Codex Micro 的控制界面作为一个普通 App 集成进 M5Stack
原厂风格的 StopWatch launcher。原有 App 会保留；Codex 页面也能在设备本地
退出并回到 launcher。

> [!WARNING]
> 这是仅面向 **M5Stack StopWatch Dev Kit C152** 的实验性、非官方兼容层，
> 未获 M5Stack、OpenAI 或 Work Louder 背书。Codex Micro 协议不是公开、稳定的
> API，后续可能变化。编译成功不等于已经在真机上验证了蓝牙、按键、电源或恢复流程。

当前版本已在一台 C152 真机上验证冷启动、原厂风格 launcher、Codex
Micro 界面与控制、BLE 连接、中央 Send，以及 A+B 返回 launcher。未来
ChatGPT Desktop 或协议更新仍可能影响兼容性。

## 按键与触控

设备只上报控制编号；最终功能由电脑端的 Codex Micro 设置决定。建议对应关系如下：

| StopWatch 操作 | 上报控制 | 行为 / 建议电脑端设置 |
| --- | --- | --- |
| 按住实体 **A** | `ACT10` | 按住说话，松开停止 |
| 按下再松开实体 **B** | `ACT09` | 一次短命令脉冲；可设为语音对话 |
| 点击 **A1-A6** | `AG00-AG05` | 打开或聚焦电脑端指定的六个 Agent 对话 |
| 点击中间配额 / **SEND** | `ACT12` | 发送当前输入框消息 |
| 向上、右、下、左滑动 | 四个模拟摇杆方向 | 四项功能分别由电脑端配置 |
| 同时按住实体 **A+B** 约 500 ms | 本地 launcher 操作 | 释放所有控制，只关闭本 App，并回到原厂 launcher |

如需让 A1-A6 固定对应某些对话，请在 ChatGPT Desktop 中选择
**Settings > Codex Micro > Agent keys > Custom assignments** 后逐一绑定。
A1-A6 是槽位编号；当前蓝牙数据不包含项目名或对话名，所以设备不能自动显示这些名称。

A+B 完全由 StopWatch 本地处理：它不会向 ChatGPT 发送“返回”，也不会退出 Mac
上的 ChatGPT。BLE 服务属于整个设备进程，因此离开 Codex App 后仍会在后台保持；
此时可以停留在 launcher 或打开其他原厂 App。

## 蓝牙重新配对

设备名称为 **Codex Micro**。如果这台 Mac 曾配对旧版 StopWatch 固件，macOS
可能继续缓存旧的 HID/GATT 描述。安装新版后若控制项没有出现，请只在 macOS
蓝牙设置中忽略 **Codex Micro**，重启 StopWatch，再重新配对；不要删除无关设备。

兼容层使用 BLE 绑定，并用额外的项目自有 GATT 服务接收少量配额数据。
StopWatch 本身不会保存 OpenAI access token。

## 只编译，不刷写

使用 ESP-IDF v5.5.4 与 ESP32-S3 target。以下命令只获取依赖并生成固件，
不会写入设备：

本分支会给上游 M5GFX 构建文件应用一个补丁，使含空格的源码路径也能正常处理。

```bash
python3 ./fetch_repos.py
source "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
```

若目标只是准备并检查固件，请在 `idf.py build` 后停止。任何后续刷写前都必须
重新识别当前连接的准确 `/dev/cu.*` 端口，并让用户再次明确确认该端口；不能沿用
上一次的端口判断或确认。

## 刷写风险与恢复原厂

刷写本固件会改变固件与分区布局。原有 NVS、原厂设置、FAT 内容，或其他固件
布局下的数据可能被格式化或无法继续读取。请先备份重要内容，并把设备写入视为
破坏性操作。

恢复原厂请使用 M5Stack 官方的
[StopWatch 工厂固件恢复指南](https://docs.m5stack.com/en/guide/restore_factory/stopwatch)，
按页面说明通过 M5Burner 安装当前原厂镜像。恢复同样会覆盖当前固件，也可能清除
设备数据。第一次真机刷写前，应先保存好这个恢复入口。

日常操作可参考 M5Stack 的
[StopWatch 官方使用说明](https://docs.m5stack.com/en/guide/display_device/stopwatch/usage)。

## 实现结构

- Codex UI 是一个标准 Mooncake `AppAbility`，与原厂 App 一起注册。
- 原生 ESP-IDF Bluedroid 服务独立于 UI 生命周期，在后台维持 BLE。
- LVGL 回调只把触摸意图加入队列，由 App 主循环发送 BLE，并为每次按下配对松开。
- A+B 沿用原厂 `KeyManager` 的 Home 手势，通过本地 App `close()` 返回 launcher。

## 来源与许可证

原厂 launcher 基于
[M5Stack `M5StopWatch-UserDemo`](https://github.com/m5stack/M5StopWatch-UserDemo)，
版权归 M5Stack Technology CO LTD，采用 MIT 许可证。Codex 控制界面与兼容工作
改编自 [`digitsisyph/codex-micro-stopwatch`](https://github.com/digitsisyph/codex-micro-stopwatch)，
版权归 imliubo 与 codex-micro-4-stopwatch contributors，同样采用 MIT 许可证。

详见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。`components/` 中下载的组件保留各自
的许可证文件。所有产品名称与商标归各自权利人所有。
