# Fridaybot 分支与版本说明

本文记录维护分支和历史里程碑的用途、继承关系、主要功能和验证边界。
2026-11-17 按用户要求收敛分支：`main` 整合 `.9` 源码，
`codex/friday-autonomy-portal` 保留为唯一继续开发的 `.11` 候选线。
这次源码整合及分支清理不包含设备刷写，也不增加任何真机验收结论。

## 分支总览

| 分支 | 代表提交 | 主要用途 | 当前状态 |
| --- | --- | --- | --- |
| `main` | `.9` 源码 `39fed12`，文档历史至 `1ad846b` | 双 App、Codex 控制、USB/主机身份及 Friday 无语音版 | 源码整合基线；不代表完整真机验收或正式发布 |
| `codex/friday-autonomy-portal` | `.11` 源码 `f8bf171`，硬件记录 `2ba082d` | `.10` 主机切换、Friday 桌面自主行为、Portal 预热与响应优化 | 唯一候选开发线；完整真机矩阵完成前不合入 `main` |

以下六个旧分支不再作为维护入口；其指针在确认全部历史由上述两条分支保全后清理。
永久提交链接可继续用于审计，不依赖旧分支名存在。

| 历史分支 | 保存的提交 | 主要用途 | 归属 |
| --- | --- | --- | --- |
| `codex/friday-codex-dual-app` | [`a0cf16e`](https://github.com/Ryanyuyuyuyu/Fridaybot/commit/a0cf16ef1b97c25a07e1d7d3416d2637484b81dc) | Friday 与 Codex 两个独立 App | `main` 与候选线历史 |
| `codex/remove-legacy-chat-mode` | [`23e48fa`](https://github.com/Ryanyuyuyuyu/Fridaybot/commit/23e48facf34b160332e4c9bd1af33e52bbb1daa9) | 删除旧 Chat 仪表盘 | `main` 与候选线历史 |
| `codex/friday-flash-capsule` | [`1e67fbd`](https://github.com/Ryanyuyuyuyu/Fridaybot/commit/1e67fbd2c6fea12c2eceaaf554dc5e772dc7ed9d) | 已撤销的流式录音实验 | 历史保留；`.9` 已完整移除，不用于部署 |
| `codex/single-host-usb-priority` | [`dc350c3`](https://github.com/Ryanyuyuyuyu/Fridaybot/commit/dc350c39f74c60118aae0bbc44b8dbf5da9a5a01) | 单主机、原生 USB、身份助手 | `main` 与候选线历史 |
| `codex/remove-voice-memo` | [`1ad846b`](https://github.com/Ryanyuyuyuyu/Fridaybot/commit/1ad846ba6bde7db83bc9ab9a6b5774dfce669ba2) | `.9` 移除语音及发布文档 | `main` 与候选线历史 |
| `codex/host-switching-low-power` | [`199111d`](https://github.com/Ryanyuyuyuyu/Fridaybot/commit/199111d60be4e2c0e46b86e9446865a978430cec) | `.10` 主机切换、低后台工作和跨 Mac 交接文档 | 功能及独有文档均由 `.11` 候选线保全 |

演进关系：

历史 `main@54c5925` → `friday-codex-dual-app` → `remove-legacy-chat-mode` →
`friday-flash-capsule` → `single-host-usb-priority` →
`remove-voice-memo` → `host-switching-low-power` → `friday-autonomy-portal`

## 2026-11-17 收敛规则

- `codex/friday-autonomy-portal` 是唯一继续开发的候选集成线；在真机验收完成前不合入
  `main`，也不打正式发布 tag。
- `main` 快进整合 `.9` 的完整历史；其独立读回、启动与 USB RPC 证据不代替
  Friday/Codex 实体按键、双 Mac、睡眠唤醒等尚未闭环的验收。
- `.11` 已合并 `remove-voice-memo` 的文档旁支，并保留最新候选状态；
  `host-switching-low-power` 独有的 `199111d` 跨 Mac 文档也已吸收，旧 `.10`
  推荐分支及“设备仍为 `.9`”陈述已更新。合并不改变 `.11` 应用源码。
- 六个旧功能分支的历史分别由 `main` 或候选线保全后，可删除本地和远端分支指针。
  此规则替代此前保留所有历史分支名的安排；历史提交不会随分支清理丢失。
- 历史记录中的 `codex/friday-face` 是旧独立 NimBLE 原型；`.11` 已将自主行为适配到
  共享 Codex BLE 服务，不能再合并旧原型。
- `codex/friday-flash-capsule` 仅作为历史提交供审计；录音、转写和收件箱已明确撤销，
  不得作为部署或恢复来源。

## `main`：`.9` 源码整合基线

- 提供圆屏 Friday 角色、表情、触摸、姿态和自主动画。
- 保留 M5Stack StopWatch 原厂 launcher 与评估 App。
- Mac Companion 仅交换粗粒度在场状态，并支持 Friday 在 StopWatch 与 Mac 屏幕间移动。
- 跨屏过程使用所有权交接，避免手表和 Mac 同时显示两个 Friday。
- 已整合 Codex 双 App、旧 Chat 面板移除、原生 USB、第一代主机选择及 `.9` 语音移除。
- 不包含 `.10` 手动切换修复和低后台助手、`.11` 自主行为及 Portal 优化。
- 这是源码分支收敛，不是新的固件部署或完整硬件验收；保留既有实验性说明。

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
- 产品决定已经撤销：该分支仅保留历史实现，不应作为当前固件部署目标。

## `codex/single-host-usb-priority`：单主机与原生 USB

- 一次只允许一台 Mac 接收 Codex 按键、摇杆、任务状态和额度状态。
- 圆屏顶部显示短主机名及 `USB` / `BLE` / `Offline`，并提供设备选择菜单。
- 同一 Mac 身份确认且原生 RPC 就绪后，USB 优先；仅供电或只枚举不会取得控制权。
- USB 拔出后只回退到同一 Mac 的已就绪 BLE，避免控制误发给另一台电脑。
- 增加 ESP32-S3 原生 USB HID：interrupt-IN `0x81`，Output 6 与 Feature 7/8 经 EP0。
- Mac 身份助手以稳定 UUID 将 USB 与 BLE 识别为同一主机，并同步小型额度快照。
- 最后提交让身份助手在 Mac 登录后受管理启动，解决重启后 USB 身份缺失导致控制不可用。
- `.6`—`.8` 分别覆盖主机选择、USB 发现/恢复和 EP0 接收修复；`.8` 已完成独立读回、启动及原生 RPC 验证，但实体双 Mac 场景仍是独立验收项。

## 历史 `codex/remove-voice-memo`：`.9` 无语音版

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
[Codex 主机切换与低功耗](https://github.com/Ryanyuyuyuyu/Fridaybot/blob/199111d60be4e2c0e46b86e9446865a978430cec/docs/codex-host-switching-low-power.md)。

## `codex/friday-autonomy-portal`：`.11` 候选集成线

- 继承 `.10` 的手动主机选择、同机 USB/BLE 接续、状态隔离和低功耗身份助手。
- Mac Companion 增加工作感知的 Friday 自主活动、屏幕边缘休息和即时打断。
- StopWatch 的 Portal 加入触摸预热和快速外滑识别；空闲 Friday BLE 参数调整为
  160—240 ms、latency 1，缩短首次交接等待。
- 2026-09-15 复跑 16 组主机、协议、传输和 GATT 兼容测试全部通过，既有 40 个
  GATT handles 保持不变。

候选镜像为 4,365,040 字节，SHA-256 为
`054931185efc806e2bbf4cd9229ec5082b92ada15cf9ab0104d9f600f0d68ca5`。
2026-09-14 应用镜像仅写入设备的 `ota_0 @ 0x20000`，并更新必要的 OTA 选择元数据；
独立读回与候选逐字节一致，非目标保护区一致，`ota_1` 中的 `.9` 回滚镜像保留。
曾观察到一次正常启动和基础原生 RPC 响应，但设备随后回到下载/串口状态。

2026-09-15，用户执行普通重启后，设备恢复为相同的 Codex Micro USB HID。停止普通
模式身份助手后，原生 `device.status` RPC 连续稳定超过 30 分钟且没有新增 HID 错误；
同时运行 Friday Companion 不影响这条 USB 链路。Companion 0.7 发现当前 Friday
service、context 与 travel characteristics，并完成以下用户可见的双向交接：

`DeviceOffer → HostAccept → DeviceHidden → Mac 入场 → DeviceRecall（实体 B） → HostReturnBegin → HostHidden`

第一次 Portal 尝试失败是因为 Friday Companion 未运行；当前仓库中的开发版没有自动
启动配置，需要显式启动。普通身份/额度助手仍出现旧 BLE 特征缓存/额度读取超时，其余
实体 Friday/Codex 控制、睡眠唤醒和双 Mac 矩阵也尚未闭环。因此这些证据允许将分支
作为候选备份和审阅，但不能将 `.11` 标记为完整真机通过或正式发布，也不能合入
`main`。

## 部署与安全边界

- Friday 与 Codex 是相邻但独立的项目边界；修改一侧时必须回归另一侧的协议和控制行为。
- 构建成功、镜像检查、写入成功、独立读回、启动、USB/BLE 握手和实体按键是不同验证层级。
- 双 OTA 设备应保留完整实机快照和已知可用分区，只写当次确认的非活动 App 槽，再独立读回并验证非目标区域。
- `build-*`、硬件快照、设备 NVS 和恢复镜像是本地证据，不应提交到公开仓库。
- 每次新固件部署前重新读取设备、端口、分区表、OTA 元数据和保护区；不要复用旧快照或旧序列假设。
