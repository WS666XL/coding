# 项目文档导航

这套文档面向两个用途：**自学**（搞清楚每一行代码为什么这么写）和**面试**（经得起追问）。

## 先读哪一份

### 如果你是来学的（建议顺序）

| 顺序 | 文档 | 读完你能回答 |
|---|---|---|
| 1 | [01-system-overview.md](01-system-overview.md) | 这个系统由哪几块组成？一帧数据怎么走完全程？ |
| 2 | [02-code-walkthrough.md](02-code-walkthrough.md) | 每个文件负责什么？该按什么顺序读代码？ |
| 3 | [03-key-designs.md](03-key-designs.md) | 为什么这样设计？代价是什么？ |
| 4 | [05-pitfalls.md](05-pitfalls.md) | 踩过哪些坑？怎么定位的？**（学习价值最高）** |
| 5 | [04-build-and-run.md](04-build-and-run.md) | 怎么跑起来、怎么加摄像头、出问题怎么查 |

### 如果你是来准备面试的（30 分钟速通）

| 顺序 | 文档 | 用途 |
|---|---|---|
| 1 | [interview.md](interview.md) | 15 组问答 + 追问陷阱，**先背这个** |
| 2 | [03-key-designs.md](03-key-designs.md) | 每条的"问题→方案→代价"，用来支撑追问 |
| 3 | [protocol.md](protocol.md) | 被问到具体字段时翻 |
| 4 | [benchmark.md](benchmark.md) | 被问到数字来源时翻 |
| 5 | [05-pitfalls.md](05-pitfalls.md) | 被问"遇到过什么难题"时翻 |

## 全部文档

### 我写的（这套）

| 文件 | 内容 | 适合场景 |
|---|---|---|
| `GUIDE.md` | 本文件，导航 | — |
| `01-system-overview.md` | 系统全景：模块划分、数据流、线程模型、关键常量 | 建立整体认知 |
| `02-code-walkthrough.md` | 代码导读：目录树、每个模块职责、推荐阅读顺序 | 看代码前必读 |
| `03-key-designs.md` | 12 个关键设计决策，每条讲清问题、方案、代价 | 理解"为什么" |
| `04-build-and-run.md` | 构建、运行、摄像头、压测、打包、排障 | 动手 |
| `05-pitfalls.md` | 17 个真实 bug 的现象、根因、修法、教训 | 学调试思路 |

### 项目原有的

| 文件 | 内容 |
|---|---|
| `protocol.md` | 协议规范：RTP-lite 头逐字节布局、FU-A、XOR FEC 载荷、控制报文、时序 |
| `benchmark.md` | 四个实验：丢包扫描、抖动缓冲、延迟构成、拥塞控制 |
| `bench-raw.csv` | 36 次压测原始数据（`benchmark.md` 里表格的来源） |
| `interview.md` | 面试问答 + 不要说什么 |

## 项目速览

```
C++17 / Qt6 / FFmpeg / Linux        约 10600 行源码 + 4600 行测试
198 个单元与集成测试                 ThreadSanitizer 零数据竞争
端到端延迟 110ms（可压到 70ms）      5% 丢包下 PSNR 60.97dB，NAL 送达率 99.7%
```

| 模块 | 行数 | 一句话 |
|---|---|---|
| `common` | 809 | 无锁队列、帧池、字节序、统计、日志 |
| `capture` | 1877 | 合成源 / 文件源 / V4L2（含格式转换） |
| `codec` | 683 | x264 编码、libavcodec 解码 |
| `protocol` | 1694 | RTP-lite、FU-A、FEC、NACK、抖动、RTT、带宽估计、控制通道 |
| `transport` | 1816 | UDP socket、发送器、接收流水线 |
| `app` | 1057 | 两个命令行程序 |
| `ui` | 2665 | Qt6 界面、GL 渲染、指标面板、进程内发送端 |
