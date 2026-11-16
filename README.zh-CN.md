# M5Stack StopWatch：Friday + Codex Micro

[English](README.md)

当前候选开发线是 `codex/friday-autonomy-portal`，对应
`V0.5-friday-codex.11`。它继承单一活动 Mac、设备手动切换、同机 USB
优先与 BLE 回退、身份助手低后台占用，并加入 Friday 自主行为和 Portal 优化。主机规则、工程验证和
双 Mac 验收矩阵见 [主机切换与低功耗说明](docs/codex-host-switching-low-power.md)。
USB 需要专用构建配置，并在每台 Mac 运行
[身份与额度助手](companion/macos/host_identity/README.md)。`.8` 的部署记录已确认
独立读回、启动和原生 USB RPC 响应。`V0.5-friday-codex.9` 已移除 Friday
语音备忘录，并于 2026-09-07 写入 `ota_1`；独立完整读回和非目标保护区校验通过，
设备随后以同一 Codex Micro 原生 USB 身份恢复并返回有效 RPC。Friday/Codex
实体按键回归与双 Mac 行为仍属于单独的真机验收项。现有 Codex 控制、停用的
GATT 属性占位及原有顺序均保留，以维持已配对主机的句柄兼容。

当前候选集成线为 `codex/friday-autonomy-portal`，固件版本
`V0.5-friday-codex.11`。它继承 `.10` 的手动 Mac 选择、同机 USB/BLE 接续、
Agent 状态一致性和身份助手低功耗调度，并加入 Friday 桌面自主行为与更低延迟的
Portal 交接。2026-09-14，`.11` 应用镜像仅写入 `ota_0`，并更新必要的 OTA 选择
元数据；独立读回逐字节一致，`.9` 回滚槽和全部保护区保持不变。曾观察到一次正常
启动和基础原生 RPC，但设备之后回到下载/串口状态。2026-09-15，普通重启后设备以
同一个 Codex Micro USB HID 身份恢复；停止普通模式身份助手后，原生
`device.status` RPC 连续稳定超过 30 分钟，没有新增 HID 错误。Friday Companion
0.7 随后连接到当前 context/travel 特征，用户观察到 Portal 入场，并通过实体 B 召回；
双向 visibility barrier 握手完整闭环。第一次 Portal 失败确认是 Companion 未运行，
当前开发版仍需显式启动。普通身份/额度助手、其余实体控制、睡眠唤醒与双 Mac 切换
仍未验收；`.11` 目前是候选版本，不是正式发布版本。

所有维护分支的用途、继承关系、验证状态和推荐用法见
[分支与版本说明](docs/branches-and-releases.md)。

## 换一台 Mac 继续开发

新电脑直接克隆远端并切到当前候选分支。这个分支已经继承 Friday/Codex 双 App、
旧 Chat 界面移除、Friday 语音备忘录移除、USB 原生传输、单一活动主机、手动 Mac
切换和低后台占用助手，并加入 Friday 自主行为和 Portal 优化；无需从旧电脑复制源码目录。
`main` 是 `.9` 源码整合基线，不代表完整真机验收。六个历史功能分支正收敛到
`main` 和此候选线，历史提交及验证边界见 [分支与版本说明](docs/branches-and-releases.md)。

```sh
git clone https://github.com/Ryanyuyuyuyu/Fridaybot.git
cd Fridaybot
git switch codex/friday-autonomy-portal
git pull --ff-only
```

不要复制旧 Mac 的构建目录。固件开发使用 ESP-IDF 5.5.4、Python 3 和 ESP32-S3
target；Mac 路由助手需要 macOS 13 或更高版本及 Xcode Command Line Tools。
克隆后先运行离线检查：

```sh
./tests/run_host_tests.sh
make -C companion/macos/host_identity
make -C companion/macos/host_identity test
```

助手安装包会生成在
`companion/macos/host_identity/build/CodexHostIdentity.zip`。每台需要出现在设备
菜单里的 Mac 都安装并运行一份；安装、登录启动、命名、查看日志和卸载命令见
[身份与额度助手说明](companion/macos/host_identity/README.md)。可以把私人电脑设为
`Mac P`、工作电脑设为 `Mac W`。每台 Mac 会在本机生成自己的稳定 UUID；不要把
`~/Library/Application Support/Friday/Codex Host Identity/host-id` 复制到另一台
电脑，也不要提交到 Git。

设备选择遵循以下规则：

| 当前连接 | 选择结果 |
| --- | --- |
| 同一台 Mac 同时有 USB 和 BLE | 身份与原生 Codex RPC 都就绪后使用 USB；拔线后只回到同一台 Mac 的 BLE。 |
| 工作 Mac 用 USB、私人 Mac 用 BLE | 新接入的 USB 只在连接后没有更晚手选覆盖时取得默认优先；手选 `Mac P` 或 `Mac W` 后，即使它暂时离线也会保留选择。 |
| 两台 Mac 都用 BLE | 菜单显示两台已记住的电脑，按键和状态只交给手动选中的一台。 |
| 仅供电，或 USB 只有枚举而没有身份/RPC | 不把它视为可控制的 USB 主机。 |
| 选中的 Mac 暂时不可用 | 显示该主机 `Offline`，不会静默把按键发给另一台电脑。 |

屏幕上的主机名、`USB` / `BLE` / `Offline`、Agent 状态、额度和按键接收电脑必须
属于同一个已选主机。助手只负责稳定身份和少量额度快照，控制 RPC 仍由原生 Codex
处理。它在空闲时会降低扫描与刷新频率，也不会阻止 Mac 睡眠。源码测试不能替代
真实双 Mac 的按键、拔插、睡眠/唤醒和功耗验收。

需要 USB 的固件使用独立配置构建，并把产物留在 Git 已忽略的本地目录：

```sh
python3 ./fetch_repos.py
source "$IDF_PATH/export.sh"
idf.py -B build-usb \
  -D SDKCONFIG=build-usb/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.usb.defaults' build
python3 tests/usb_configuration_descriptor_test.py build-usb/StopWatch-UserDemo.elf
```

以上命令只构建和检查，不会写入 StopWatch。`build*` 产物、助手安装包、硬件备份、
flash dump、设备 NVS、Mac 身份文件和登录启动设置都不会进入 Git 交接。远端仓库交接
的是完整源码。最近记录的 2026-09-15 真机检查覆盖 `.11` 的 USB RPC 和一次 Portal
往返，`.9` 保留为回滚槽；这些历史记录不确认设备当前状态，也不代表完整矩阵通过。
剩余真机矩阵完成前，`.11` 不合入 `main`，不打正式发布 tag。

这个分支把 Friday 与兼容 Codex Micro 的控制界面做成 M5Stack StopWatch
launcher 中两个相邻、互相独立的 App。冷启动先进入 launcher，可以直接选择
Friday 或 Codex；Codex Micro 只保留 Codex 控制界面，Friday 与其他原厂 App
继续使用各自独立的界面。

> [!WARNING]
> 这是仅面向 **M5Stack StopWatch Dev Kit C152** 的实验性、非官方兼容层，
> 未获 M5Stack、OpenAI 或 Work Louder 背书。Codex Micro 协议不是公开、稳定的
> API，后续可能变化。编译成功不等于已经在真机上验证了蓝牙、按键、电源或恢复流程。

Friday 与纯 Codex 基线此前都已分别在 C152 真机上验证。本次统一版本已通过
主机协议/UI 测试、完整 ESP-IDF 编译、稳定 USB RPC 观察和一次完整 Friday
跨屏往返；仍需真机验证重新配对、两个 launcher 入口、其余 Codex 控制、睡眠唤醒
与功耗。

## Codex App 按键与触控

设备只上报控制编号；最终功能由电脑端的 Codex Micro 设置决定。建议对应关系如下：

| StopWatch 操作 | 上报控制 | 行为 / 建议电脑端设置 |
| --- | --- | --- |
| 按下或按住实体 **A** | `ACT10` | 按下即开始按住说话，松开停止 |
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

## Friday 交互

- 轻点、长按、拖动和滑动屏幕，以及倾斜或拿起设备，会触发不同的表情与动作；
- 实体 A 键单独按下或按住均无操作；
- 实体 B 键播放追逐和跳跃动画；Friday 在 Mac 上时，B 用于召回；
- 连接 Friday Companion 后，将 Friday 拖到朝向 Mac 的屏幕边缘，即可让它前往桌面；
- 同时按住 A+B 返回原厂 launcher。

Friday Companion 提供粗粒度在场状态与跨屏移动功能，详情见
[Mac 伴侣说明](companion/macos/README.md)。

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

若设备已经使用本仓库的双 OTA 分区表，为保留回撤能力，不要直接运行通用的
`idf.py flash`：它还会写 bootloader、分区表和 OTA 元数据。应先保留已校验的
整机备份与已知可用分区，再根据设备当前分区表只写未激活的 OTA App 分区。

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

- Friday 与 Codex Micro 是两个独立的 Mooncake `AppAbility`；冷启动停留在
  launcher，并把它们相邻排列。
- Codex Micro 只包含一个 Codex Dashboard，与原厂 App 一起注册，不再包含隐藏
  的 Chat 本地预览模式。
- 在 Codex App 中，实体 A 键会在按下和松开时直接上报 `ACT10`。程序会优先判断 A+B Home 组合键，
  因此返回 launcher 时不会遗留按住状态。
- 两个 UI 共用唯一的 ESP-IDF Bluedroid 服务；同一份 GATT 数据库同时提供
  Codex HID/额度与 Friday context/travel 通道，避免启动两套互斥的 BLE Host。
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
