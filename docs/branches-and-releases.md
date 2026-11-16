# Fridaybot 分支与版本说明

## 当前维护线（2026-11-17）

本次按用户要求整合源码并清理历史分支，只保留以下两条维护线。

| 分支 | 内容 | 验证边界 |
| --- | --- | --- |
| `main` | `.9` 无语音源码基线，继承至 `1ad846b` | 历史镜像刷写、独立读回、保护区、启动及 USB RPC 有记录；实体按键、双 Mac 和完整交互仍需验收 |
| `codex/friday-autonomy-portal` | `.11` 唯一候选开发线，包含 `.10` 主机切换/低功耗与 `.11` 桌面自主行为/Portal | 保留候选；完整真机矩阵闭环前不合入 main、不标正式发布 |

main 的收敛是源码整理，不代表完整硬件验收或新固件发布。本次不刷写设备，
也不根据历史记录推断当前设备运行版本。

`.11` 在 2026-09-15 的记录中已通过镜像读回、30 分钟以上 USB RPC 和一次 Portal
往返；普通身份/额度助手仍有 BLE 缓存/读取超时，实体交互、睡眠唤醒与双 Mac 矩阵待验收。
完整记录见[候选线说明](https://github.com/Ryanyuyuyuyu/Fridaybot/blob/codex/friday-autonomy-portal/docs/branches-and-releases.md)。

## 已收敛的历史分支

这些分支指针在本次清理中删除；提交历史由 main 或候选线保留，不恢复已撤销的录音功能。

| 历史分支 | 原 tip | 用途 | 保存位置 |
| --- | --- | --- | --- |
| `codex/friday-codex-dual-app` | `a0cf16e` | Friday 与 Codex 独立 App | main 祖先链 |
| `codex/remove-legacy-chat-mode` | `23e48fa` | 移除旧 Chat 仪表盘 | main 祖先链 |
| `codex/friday-flash-capsule` | `1e67fbd` | 已撤销的闪念录音实验 | main 历史；当前功能已删除 |
| `codex/single-host-usb-priority` | `dc350c3` | 单主机、原生 USB 与身份助手 | main 祖先链 |
| `codex/remove-voice-memo` | `1ad846b` | `.9` 删除 Friday 语音备忘录 | 已整合进 main |
| `codex/host-switching-low-power` | `199111d` | `.10` 手选主机、状态隔离、低后台助手和跨 Mac 交接 | 候选线继承代码并保留交接文档 |

## main：`.9` 无语音源码基线

- 保留 Friday 本体、Mac Companion、跨屏移动与原厂 launcher。
- Friday 与 Codex Micro 是两个独立 App，共用一个 BLE 服务。
- 包含单一活动 Mac、USB 原生传输、身份助手和同机 BLE 回退。
- 删除旧 Chat 仪表盘和 Friday 语音备忘录；历史录音文件不被删除。
- `.10` 的手选优先修复、低功耗助手与 `.11` 自主行为仍只在候选线。

以下章节描述历史演进，不代表各旧分支仍在维护。

## `codex/friday-codex-dual-app`：Friday 与 Codex 分离

- 在原厂风格 launcher 中注册 Friday 和 Codex Micro 两个相邻、独立的 App。
- 冷启动停留在 launcher，由用户选择进入 Friday 或 Codex。
- Codex 控制面与 Friday 的界面生命周期分离，避免把两个项目合成一个重型全局入口。
- 共用单一底层 BLE 服务，避免并行启动互斥的蓝牙 Host。
- 保留 A1—A6、SEND、方向手势、实体 A/B 与 A+B 返回 launcher 的控制基础。

## `codex/remove-legacy-chat-mode`：Codex 控制面精简

- 移除紫色本地 Chat dashboard、聊天预览模型和 A 键双击模式切换。
- Codex Micro 只保留原生 Codex 控制界面，不再猜测或展示私有会话标题。
- 实体 A 在 Codex App 中直接发送 `ACT10` 按下/释放，用于电脑端配置的 PTT。
- 保留实体 B=`ACT09`、SEND=`ACT12`、A1—A6=`AG00`—`AG05`、方向手势和 A+B Home。
- Friday 和其他原厂 App 的入口、交互与数据边界不变。

## `codex/friday-flash-capsule`：闪念胶囊实验

- 曾实现按住 Friday A 键录音，并将音频边录边传给连接的 Mac。
- Mac 端原型包含流式接收、转录，以及待办/备忘统一收纳入口。
- 录音限制、成功反馈和 BLE/GATT 兼容均在该实验线上迭代。
- 真机测试中出现严重杂音和 A 键红灯但未形成可用语音输入的问题。
- 产品决定已经撤销：该实现仅保留在 Git 历史中，不应作为当前固件部署目标。

## `codex/single-host-usb-priority`：单主机与原生 USB

- 一次只允许一台 Mac 接收 Codex 按键、摇杆、任务状态和额度状态。
- 圆屏顶部显示短主机名及 `USB` / `BLE` / `Offline`，并提供设备选择菜单。
- 同一 Mac 身份确认且原生 RPC 就绪后，USB 优先；仅供电或只枚举不会取得控制权。
- USB 拔出后只回退到同一 Mac 的已就绪 BLE，避免控制误发给另一台电脑。
- 增加 ESP32-S3 原生 USB HID：interrupt-IN `0x81`，Output 6 与 Feature 7/8 经 EP0。
- Mac 身份助手以稳定 UUID 将 USB 与 BLE 识别为同一主机，并同步小型额度快照。
- 最后提交让身份助手在 Mac 登录后受管理启动，解决重启后 USB 身份缺失导致控制不可用。
- `.6`—`.8` 分别覆盖主机选择、USB 发现/恢复和 EP0 接收修复；`.8` 已完成独立读回、启动及原生 RPC 验证，但实体双 Mac 场景仍是独立验收项。

## `codex/remove-voice-memo`：当前 `.9` 无语音版

- 完整移除 Friday 录音采集、音频编码/流式发送、ACK/错误状态和录音成功动画。
- 移除 Mac Companion 的音频接收、转录、闪念胶囊存储与收件箱界面。
- Friday 中实体 A 单独按下或按住均无操作；短按动画也不恢复。
- 保留 Friday 的触摸、表情、B 键互动/召回、跨屏移动、在场上下文及 A+B launcher。
- 保留 Codex App 的 A/PTT、B、SEND、A1—A6、方向手势、USB/BLE 和主机身份逻辑。
- 原闪念胶囊对应的 GATT 末尾槽位改为不可访问的占位，维持 40 个既有属性的顺序，降低已配对 Mac 缓存句柄错位风险。
- 不删除用户此前保存在 Mac 上的历史录音或转录文件。

`.9` 的应用镜像为 4,361,120 字节，SHA-256 为
`589dee859947618e177addfbecd5120bc2db6b31a0b8134ab3f2a42f88345ae7`。
2026-09-07 仅写入设备的 `ota_1 @ 0x510000`，完整独立读回逐字节一致；
11,591,680 字节非目标区域与刷写前实际快照摘要一致。OTA 元数据最终读回 `[19,20]`。
单独重启后，设备以原 `Codex Micro` USB 身份返回，并产生两条有效原生 RPC 响应。
这些证据确认镜像、启动选择和 USB 通信链路，不代替 Friday/Codex 实体按键及双 Mac 的人工验收。

## `codex/host-switching-low-power`：`.10` 候选

- 基于已移除 Friday 语音备忘录的 `.9`，不会重新加入录音功能。
- 手动选定 Mac 后，另一台 Mac 的 USB 心跳、延迟握手或软件恢复不会自动抢回控制权。
- 新的物理 USB 连接仍可应用默认 USB 优先；连接开始后的用户手选拥有更高优先级。
- 拔线只接续同一 Mac 的 BLE；不可用时保持该主机 `Offline`，不会悄悄切到其他电脑。
- 切换主机时释放旧主机的按住状态，并让旧会话的排队动作失效。
- 按字段合并 Agent 状态，避免 USB/BLE 切换时被较旧的颜色、亮度或效果覆盖。
- USB 发送预算不足但尚未尝试发送时，将完整点击延后到下一轮，维持按下/释放顺序。
- 身份助手加入 BLE 扫描退避、60 秒身份心跳、180 秒额度刷新和全局额度请求节流，减少无设备时的后台唤醒与重复请求。
- 16 组主机/集成检查、完整 ESP-IDF 构建和 USB 描述验证已通过；候选镜像已生成，但本文更新时尚未刷写，不能把 `.9` 的真机结果当作 `.10` 验收。

更细的 `.10` 行为、工程验证、资源采样局限与双 Mac 验收矩阵见该分支中的
[Codex 主机切换与低功耗](https://github.com/Ryanyuyuyuyu/Fridaybot/blob/codex/friday-autonomy-portal/docs/codex-host-switching-low-power.md)。

## 部署与安全边界

- Friday 与 Codex 是相邻但独立的项目边界；修改一侧时必须回归另一侧的协议和控制行为。
- 构建成功、镜像检查、写入成功、独立读回、启动、USB/BLE 握手和实体按键是不同验证层级。
- 双 OTA 设备应保留完整实机快照和已知可用分区，只写当次确认的非活动 App 槽，再独立读回并验证非目标区域。
- `build-*`、硬件快照、设备 NVS 和恢复镜像是本地证据，不应提交到公开仓库。
- 每次新固件部署前重新读取设备、端口、分区表、OTA 元数据和保护区；不要复用旧快照或旧序列假设。
