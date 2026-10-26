# 02 · 代码导读

## 1. 目录树

```
lowlat-video/
├── CMakeLists.txt              顶层构建（CPack 打包开关在这里）
├── cmake/                      编译告警与 sanitizer 选项
├── src/
│   ├── common/                 基础组件（谁都可以依赖）
│   │   ├── ring_buffer.hpp         无锁 SPSC 环形队列
│   │   ├── frame_pool.hpp          帧缓冲池（复用，避免每帧 malloc）
│   │   ├── byte_span.hpp           只读字节视图
│   │   ├── byte_order.hpp          网络序读写
│   │   ├── stats.hpp/.cpp          延迟滑动窗口 + 分位数
│   │   ├── log.hpp/.cpp            spdlog 封装 + 限流宏
│   │   └── ffmpeg_headers.hpp      FFmpeg 头文件 C/C++ 兼容层
│   ├── capture/                采集
│   │   ├── capture_source.hpp      抽象接口（含 is_live / reset_pacing）
│   │   ├── capture_factory.*       按名字选源
│   │   ├── synthetic_source.*      彩条 + 帧号 + 可选噪声
│   │   ├── file_source.*           任意媒体文件 / URL
│   │   ├── v4l2_source.*           V4L2 MMAP 零拷贝
│   │   ├── v4l2_converter.*        像素格式 → I420
│   │   └── yuv_frame.hpp           帧结构（I420 + 时间戳 + 引用计数）
│   ├── codec/
│   │   ├── encoder.*               libx264 封装
│   │   └── decoder.*               libavcodec 封装
│   ├── protocol/               协议与算法
│   │   ├── rtp_lite.*              20 字节头
│   │   ├── h264_fragmenter.*       Annex-B 切分 + FU-A 分片/重组
│   │   ├── fec_xor.*               XOR 前向纠错
│   │   ├── nack.*                  NACK 报文格式
│   │   ├── nack_scheduler.hpp      重排序等待 + 请求合并
│   │   ├── jitter_buffer.*         RFC 3550 抖动估计 + 深度策略
│   │   ├── rtt_estimator.*         RTT（EWMA）
│   │   ├── bandwidth_estimator.*   延迟梯度 + AIMD
│   │   └── control_message.*       统一控制通道（类型字节 + NACK/反馈）
│   ├── transport/
│   │   ├── udp_socket.*            非阻塞 UDP + sendmmsg 批量
│   │   ├── stream_sender.*         打包、FEC、缓存、响应 NACK
│   │   └── stream_receiver.*       收包、恢复、去重、重排、帧交付
│   ├── app/                    命令行程序
│   └── ui/                     Qt6 界面
│       ├── main.cpp                只有 30 行，负责分发
│       ├── app_options.*           命令行解析
│       ├── offline_mode.*          离线校验
│       ├── live_mode.*             实时播放（1200+ 行，最大文件）
│       ├── video_widget.*          QOpenGLWidget + 着色器渲染
│       ├── receiver_thread.*       收包 + 解码线程
│       ├── frame_slot.hpp          跨线程最新帧槽
│       ├── local_sender.*          进程内发送端
│       ├── stats_panel.*           右侧指标面板
│       ├── metrics_chart.*         底部实时曲线
│       └── ui_metrics.hpp          面板与日志共用的指标结构
├── tools/
│   ├── h264_probe.cpp          码流校验（帧数/错误数/分辨率）
│   └── qt_probe.cpp            Qt/GL 可行性探针（保留作参考）
├── tests/                      25 个测试文件，198 个用例
├── scripts/                    demo / camera / bench / jitter_test / setup-deps
├── packaging/                  systemd 单元
└── docs/                       本套文档
```

## 2. 推荐阅读顺序

**不要从 `ui/` 开始**——它是最大的模块，但也是最"薄"的（大部分是 Qt 胶水）。
按数据流自底向上读：

### 第一站：`common/`（约 1 小时）

重点看三个文件：

| 文件 | 要理解什么 |
|---|---|
| `frame_pool.hpp` | 为什么需要池？`shared_ptr` 的自定义 deleter 怎么把缓冲还回池里 |
| `stats.hpp/.cpp` | 滑动窗口怎么维护、分位数为什么要排序、`peek()` 和 `snapshot()` 的区别 |
| `log.hpp` | `LOWLAT_LOG_EVERY_MS` 为什么存在（防止日志本身成为瓶颈） |

**读完应该能回答**：跨线程传一帧，内存谁负责释放？

### 第二站：`protocol/`（约 3 小时，核心）

顺序：`rtp_lite` → `h264_fragmenter` → `fec_xor` → `nack` → `nack_scheduler` → `jitter_buffer` → `rtt_estimator` → `bandwidth_estimator` → `control_message`。

**这一层是纯函数/纯状态机，不碰 socket、不碰线程，最容易看懂也最值得看。**

重点问题：

- `rtp_lite.cpp` 的 `serialize_rtp_lite` 里每个字节位怎么排？
- `fragment_nal` 什么条件下走 FU-A？
- `XorFecEncoder::flush` 为什么必须在帧边界调用？
- `NackScheduler` 的 8ms 等待窗口解决什么问题？
- `BandwidthEstimator` 的 `on_sample` 里那个"窗口延迟"是怎么算的？

### 第三站：`transport/`（约 2 小时）

| 文件 | 重点 |
|---|---|
| `udp_socket.cpp` | 非阻塞 + `poll` + `sendmmsg`，为什么批量发 |
| `stream_sender.cpp` | `send_frame` 的完整流程；重传缓存怎么存；控制报文怎么分发 |
| `stream_receiver.cpp` | `poll()` 是唯一入口；`output` 成员指针为什么要置空 |

**`stream_receiver.cpp` 是全项目最复杂的文件**，建议配合
[protocol.md](protocol.md) 第 7.2 节（接收路径时序）一起看。

### 第四站：`codec/` + `capture/`（约 2 小时）

- `encoder.cpp`：x264 参数为什么这么选（zerolatency / bframes=0 / keyint）
- `encoder.cpp` 的 `set_bitrate`：**为什么是重建编码器而不是 reconfig**（见 [05-pitfalls.md](05-pitfalls.md) 第 6 条）
- `file_source.cpp`：`avformat_open_input` 怎么同时支持文件和 URL；循环播放为什么要两条路
- `v4l2_converter.cpp`：YUYV→I420 的色度下采样，注意是 2x2 块平均

### 第五站：`app/`（约 1 小时）

`sender_main.cpp` 有 500 行，主要是参数解析。重点看主循环：
码率调整（拥塞控制）怎么作用到编码器。

### 第六站：`ui/`（按需）

如果要改界面才需要细读。关键三点：

1. `live_mode.cpp` 的 `apply_ui_scale`——所有字号必须显式设置（见 [05-pitfalls.md](05-pitfalls.md) 第 15 条）
2. `frame_slot.hpp`——跨线程唯一的交接点
3. `video_widget.cpp` 的 fragment shader——YUV→RGB 的 BT.601 矩阵

## 3. 每个模块的"一句话职责"

| 模块 | 职责 | 不做什么 |
|---|---|---|
| `common` | 提供与业务无关的基础设施 | 不知道视频是什么 |
| `capture` | 产出 I420 帧 | 不知道帧会被怎么编码 |
| `codec` | 编/解码 | 不知道网络存在 |
| `protocol` | 定义报文格式与恢复算法 | 不碰 socket、不碰线程 |
| `transport` | 把协议跑在 UDP 上 | 不知道谁在消费帧 |
| `app` | 命令行外壳 | 不含业务逻辑 |
| `ui` | 显示与交互 | 不碰 socket、不做解码 |

**这个"不做什么"列表比"做什么"更重要**——它保证了每一层都能单独测试。

## 4. 三个最值得模仿的代码习惯

### 4.1 错误用 `std::string*` 传出，不用异常

```cpp
bool StreamSender::open(const Config& config, std::string* error);
```

理由：实时链路里错误要能记录上下文（哪个 ioctl、errno 是多少），
异常会把栈打散；而且调用方几乎总是要"记录并继续"，不是"向上抛"。

### 4.2 统计量用原子量，快照用单独接口

```cpp
std::uint64_t received_frames() const noexcept;  // 轻量，随时可读
ReceiverStats snapshot() const;                  // 重，会排序，别在循环里调
```

**踩过的坑**：一开始主循环每轮都调 `snapshot()` 判断退出条件，
结果因为它要排序 4096 个延迟样本，CPU 白烧。

### 4.3 注释写"为什么"，不写"做什么"

```cpp
// 帧边界：把未满的组也结算掉，保证修复包与本帧数据在同一批里发出，
// 收端能在本帧内完成恢复，避免 NAL 顺序错乱。
if (fec_encoder_.flush(&tail_repair, &tail_produced, error) && tail_produced) {
```

代码本身说明了"做什么"，注释要说明"不这么做会怎样"。
全项目的注释都按这个标准写。

## 5. 想动手改的话，从哪开始

| 想做的事 | 改哪里 | 难度 |
|---|---|---|
| 加一种采集源 | `capture/` 新增类 + `capture_factory.cpp` 加分支 | 低 |
| 加一个指标 | `ui/ui_metrics.hpp` 加字段 → 面板加一行 → 日志加一列 | 低 |
| 换用 Reed-Solomon FEC | `protocol/fec_xor.*` 换实现，接口不变 | 中 |
| 加音频 | 需要新的帧类型、双路时间戳对齐、混流策略 | 高 |
| 支持跨平台 | 换 socket 层与采集层（见 README 已知限制） | 中高 |
| 让 V4L2 真正可用 | 已补 `VIDIOC_S_PARM` 帧率协商与格式优先级（YUV420>MJPEG>YUYV），只差实机验证 | 已完成，待验证 |

## 6. 读代码时最容易被绊住的三个点

### ① `poll()` 的 `output` 成员指针

`stream_receiver.cpp` 里产帧有三个位置（帧边界、缓冲上限、到期冲刷），
层层传参会把签名污染得到处都是，所以用成员 `output` 记录"这次调用的输出目标"。

**必须记得在 `poll()` 结尾置空**，否则下一次调用会写进已销毁的 vector。

### ② 序列号用 `int16_t` 差值比较

```cpp
const bool is_retransmit =
    have_sequence && (static_cast<std::int16_t>(header.sequence - last_sequence) <= 0);
```

uint16 序列号会回绕，直接比大小会在 65535→0 处出错。
转成 int16 差值，回绕自然正确。**所有与序列号有关的比较都必须这么写。**

### ③ 采集时间戳的低 32 位

`capture_ts_us` 只存 32 位（约 71 分钟回绕）。计算延迟时用**无符号差值**：

```cpp
const std::uint32_t delta = now_low32 - capture_ts;
```

跨报文比较时还要 `unwrap32` 展开成 64 位（见 `stream_sender.cpp`），
否则回绕瞬间会得到巨大的负值，被误判成"链路突然变快"从而错误提速。
