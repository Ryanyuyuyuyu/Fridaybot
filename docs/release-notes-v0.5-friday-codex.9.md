# V0.5-friday-codex.9：移除 Friday 语音备忘录

`.9` 删除了尚未达到实机体验要求的 Friday 语音备忘录功能，继续保留 Friday 与 Codex Micro 的其他功能。对应源码分支为 `codex/remove-voice-memo`，提交为 `39fed12cd141e328514f91fefed5683c25f322a1`。

## 删除的功能

- Friday A 键不再启动录音，不再显示红色录音环或保存、转写反馈。
- 固件不再采集麦克风音频、重采样、编码或通过 BLE 流式发送录音。
- Mac Companion 不再接收语音帧、创建临时 WAV、维护每日语音 inbox 或调用系统语音识别。
- 删除录音超时、确认、重试和转写结果相关的队列、状态及界面。

## 保留的功能

- Friday B 键、Travel、Friday Companion 的非语音能力及启动器入口保持不变。
- Codex A1–A6、摇杆、Agent 状态、额度显示、USB/BLE 控制协议和设备身份保持不变。
- 为避免已经配对的 BLE 主机因 GATT handle 变化失效，原语音特征所占位置被保留为不可访问的兼容槽；完整 GATT 表仍为 40 个 handle。
- 用户电脑里已有的录音、转写文件和历史数据没有被删除。

## 验证与部署

- 主机及集成测试通过，并检查语音录制、传输、转写和 inbox 代码已从构建中移除。
- 链接后的 USB 描述保持 interrupt-IN `0x81` / 64 字节，Output 6 与 Feature 7/8 继续通过 EP0。
- `.9` 镜像大小为 4,361,120 字节，SHA-256 为 `589dee859947618e177addfbecd5120bc2db6b31a0b8134ab3f2a42f88345ae7`。
- 实机只写入 `ota_1 @ 0x510000` 和必要的 OTA metadata；镜像独立读回逐字节一致，11,591,680 字节保护区保持一致。
- 设备随后重新枚举为 Codex Micro，原生 USB RPC 收到有效响应。Friday 实体按键与 Codex 实体按键仍需用户侧验收，构建和读回不能代替这一步。

`.9` 是后续 `.10` 多 Mac 主机切换版本的基线。语音备忘录功能不会因为升级到 `.10` 而恢复。
