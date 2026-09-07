# Fridaybot 分支与功能更新

本文件记录当前本地分支的用途和关系。硬件完整备份、实机读取、烧录收据、构建缓存及本机身份配置不属于 Git 仓库，不会推送到远端。

## 分支总览

| 分支 | 主要功能 | 状态与关系 |
| --- | --- | --- |
| `main` | Friday Companion、跨设备 Travel 和基础 M5Stack StopWatch 启动器 | 稳定基线；不包含后续 Codex 专项实验 |
| `codex/friday-codex-dual-app` | 将 Friday 与 Codex Micro 作为两个独立启动器应用 | 建立应用边界，避免两个产品入口和状态混在一起 |
| `codex/remove-legacy-chat-mode` | 删除旧版 Codex Chat 仪表盘 | 保留 Agent A1–A6 控制面，减少重复入口 |
| `codex/friday-flash-capsule` | Friday A 键语音采集、BLE 流式传输、Mac 保存与转写原型 | 历史实验分支；实机体验未达到要求，后续 `.9` 已完整移除此功能 |
| `codex/single-host-usb-priority` | `.8` 单活动 Mac、USB 优先、同机 BLE 回退、圆弧主机状态 UI | 第一代主机选择实现；已被 `.10` 的手动切换和低功耗修复扩展 |
| `codex/remove-voice-memo` | `.9` 删除 Friday 语音备忘录链路 | 保留 Friday 其他功能、Codex 控制与 GATT handle 兼容；是 `.10` 的直接基线 |
| `codex/host-switching-low-power` | `.10` 多 Mac 手动切换、同机 USB 优先、Agent 状态隔离、低功耗身份助手 | 当前最新功能分支；代码、构建及离线测试完成，实体双 Mac 验收单独进行 |

## `.9`：删除语音备忘录

`.9` 从固件和 Mac Companion 两端删除录音、音频编码、BLE 语音帧、临时 WAV、每日 inbox、系统转写和录音反馈。Friday A 键不再启动录音。Friday B、Travel、启动器、Codex A1–A6、摇杆、Agent 状态、额度和 USB/BLE 协议继续保留。

为了兼容已经配对的主机，旧语音 GATT 位置保留为不可访问槽，完整 attribute handle 数量仍为 40。已有本地录音和转写数据没有被删除。完整说明见 [`.9` 发布说明](release-notes-v0.5-friday-codex.9.md)。

`.9` 已在实机只写 `ota_1` 和必要 OTA metadata，独立读回与候选逐字节一致；Friday、设置、存储和原始回退应用所在的 11,591,680 字节保护区校验保持一致。USB 重新枚举和原生 RPC 已验证，实体按键仍需要用户体验验收。

## `.10`：多 Mac 切换与 USB 优先

`.10` 建立一个明确的活动 Mac。顶部名称、USB/BLE/Offline、Agent A1–A6 和真实控制目标以同一份选中主机状态发布，避免界面显示一台电脑、按键却发给另一台电脑。

- 同一台 Mac 同时具备经过身份和原生 RPC 确认的 USB、BLE 时，数据优先走 USB；仅插线充电或仅完成枚举不算 USB 就绪。
- 工作 Mac 走 USB、私人 Mac 走 BLE 时，可以手动选择任意一台。手选私人 Mac 后，工作 Mac 的延迟握手、心跳或休眠恢复不会抢回控制权。
- 拔掉 USB 时只回退到同一台 Mac 已就绪的 BLE；没有同机回退通道时显示 Offline，不自动把控制权交给另一台 Mac。
- 切换主机时释放旧主机的按住状态，丢弃旧会话的排队操作，避免按键或摇杆残留到新主机。
- Agent 字段按实际接收时间合并，同机 USB/BLE 可以接续最新状态；其他 Mac 的状态不能混入，所有原生控制会话断开后清空。
- USB 发送预算耗尽但尚未尝试发送时，按下和释放会保留到下一轮；已经尝试而结果不确定的操作不会危险重发。

设备选择界面沿圆形屏幕上缘显示简短的 `Mac P` / `Mac W` 与 `USB` / `BLE` / `Offline`，保留原有六个 Agent 触控区域。完整设计、测试及资源样本见 [`.10` 主机切换说明](codex-host-switching-low-power.md)。

## 身份助手与后台开销

每台 Mac 使用独立的 `CodexHostIdentity` 助手提供稳定身份和短名称。助手通过 USB Feature Report 或加密 BLE 特征发送身份；它不发送聊天内容，也不创建 AI 任务。额度读取只调用本机 Codex app-server 的初始化和 rate-limit 读取接口。

无设备时 BLE 扫描按 15、30、60、120 秒退避；身份恢复正常间隔为 60 秒，额度刷新正常间隔为 180 秒，同一时刻最多一个额度子进程。195 秒样本中，助手自身平均 CPU 为单核的 0.0071%、RSS 峰值 6.23 MiB；计入短暂额度子进程后，整组平均 CPU 为单核的 0.2814%、采样 RSS 合计峰值 117.69 MiB。该样本说明后台成本较低且临时进程会退出，但不能直接换算为电池续航。

## 验证边界

`.10` 的 16 组主机、传输、协议和兼容测试已通过，真实 LVGL 菜单输入测试通过，完整 ESP-IDF 5.5.4 构建和链接后 USB 描述检查通过。候选镜像为 4,364,624 字节，应用分区剩余 812,720 字节。

这些结果不能代替实体双 Mac 验收。实机仍需依次验证单 BLE、双 BLE、同机 USB+BLE、工作 USB+私人 BLE、拔线回退、睡眠/唤醒、按住时切换，以及按键、摇杆和 Agent 状态是否始终属于选中的电脑。
