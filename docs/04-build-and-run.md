# 04 · 构建、运行与排障

## 1. 依赖

```bash
./scripts/setup-deps.sh      # 需要 sudo
```

它装的是：

| 类别 | 包 |
|---|---|
| 构建 | `cmake` `ninja-build` `build-essential` `pkg-config` |
| 编码 | `libx264-dev` |
| 编解码 | `libavcodec-dev` `libavformat-dev` `libavutil-dev` `libswscale-dev` |
| 日志 | `libspdlog-dev` `libfmt-dev` |
| 界面 | `qt6-base-dev` `libqt6opengl6-dev` `libgl1-mesa-dev` |
| 测试 | `libgtest-dev` |
| 排障 | `v4l-utils`（摄像头）`valgrind` |

## 2. 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build              # 198 个用例
```

产物：

```
build/src/app/lowlat_sender        发送端
build/src/app/lowlat_receiver      接收端（无界面，落盘）
build/src/ui/lowlat_ui             界面
build/tools/h264_probe             码流校验
```

## 3. 运行

### 3.1 播放自己的视频（推荐先试）

```bash
./scripts/demo.sh 你的视频.mp4
```

或者不经脚本：

```bash
./build/src/ui/lowlat_ui --local 你的视频.mp4
```

**一个进程跑完整链路**：文件 → 解码 → 重编码 → RTP-lite → 本机 UDP → 收包 → 解码 → 渲染。
中间的协议栈是真的，所以面板指标是真的。

窗口里能做的操作：

| 控件 | 作用 |
|---|---|
| `Open file...` | 选视频（记住上次目录，支持 `/mnt/c` 等 Windows 路径） |
| 拖拽 | 把视频文件拖进画面区 |
| `Pause` / `Resume` | 暂停不拆连接 |
| `Restart` | 从头重播 |
| `Loss: off ▾` | 模拟丢包 0 / 2 / 5 / 10 / 20 / 30% |

**演示丢包恢复**：切到 20%，会看到画面变块、底部码率曲线塌到 300、
`nack sent` / `retransmits` 跳数，但 fps 仍是 30。

### 3.2 摄像头

```bash
./scripts/camera.sh                 # 默认 640x480@30
./scripts/camera.sh --loss 20       # 顺便模拟丢包
```

**前提**：Windows 侧能用摄像头（`Get-PnpDevice -Class Camera` 状态为 `OK`）。

脚本会：检查设备状态 → 启动 Windows ffmpeg 采集 → 等流可用 → 开窗口 →
**关窗口时自动杀掉采集进程**（否则摄像头指示灯会一直亮着）。

相关环境变量：`CAMERA_NAME` `SIZE` `FPS` `BITRATE` `PORT`。

##### 3.2b V4L2 摄像头（原生 Linux）

```bash
./build/src/app/lowlat_sender --source v4l2 --device /dev/video0 \
    --width 1280 --height 720 --fps 30 --frames 150 --output out/cam.h264
./build/tools/h264_probe out/cam.h264      # errors 必须为 0
```

格式协商顺序：**YUV420（可零拷贝） → MJPEG → YUYV**。
MJPEG 排在 YUYV 前面是因为同样 1280x720 下 YUYV 往往只有 10fps。
可用 `--v4l2-format mjpg|yuyv|yuv420` 强制指定。

`VIDIOC_S_PARM` 会请求目标帧率，并以驱动返回的实际值为准。

**实测**（VMware / Ubuntu 24.04 / 内置 UVC 摄像头 `5986:118a`）：

```
V4L2 协商结果: 格式=MJPG 640x480 stride=0 sizeimage=614400 帧率=30
file=out/cam.h264 access_units=150 frames=150 errors=0 pix_fmt=yuv420p
```

**推荐 640×480**：这台 VM 上稳定 30fps（编码 13ms/帧）。

**720p 不稳**：`dmesg` 反复出现 `uvcvideo: Failed to resubmit video URB (-1)`，
即 USB 等时传输提交失败——VMware 的 USB 透传撑不住 720p30 的带宽。
偶发能跑通 150 帧，但不可依赖。真机上应无此问题（未验证）。

### 3.3 双进程（跨机或本机调试）

```bash
# 终端 A
./build/src/app/lowlat_receiver --bind 127.0.0.1:5000 --output out/recv.h264 \
    --frames 300 --nack --feedback

# 终端 B
./build/src/app/lowlat_sender --source file --input a.mp4 --loop \
    --dest 127.0.0.1:5000 --fec --cc

# 播放落盘结果（裸流必须指定帧率）
ffplay -framerate 30 out/recv.h264
```

跨机时把 `--bind` 改成 `0.0.0.0:5000`，`--dest` 改成对方 IP。

### 3.4 离线校验（像素级）

```bash
./build/src/ui/lowlat_ui --input out/recv.h264 --frame 300 --dump /tmp/f.png
```

不开窗口，渲染指定帧存 PNG。用于和 ffmpeg 的解码结果做像素比对。

## 4. 压测

```bash
# 丢包档位扫描：6 档 × 3 策略 × 2 次，输出 CSV + 汇总表
./scripts/bench.sh

# 快速验证（约 2 分钟）
LEVELS="2 5" CONFIGS="none fec+nack" REPS=1 FRAMES=150 ./scripts/bench.sh

# 抖动缓冲对照
./scripts/jitter_test.sh --mode fixed    --delay "20ms 20ms" --loss 5 --frames 600
./scripts/jitter_test.sh --mode adaptive --delay "20ms 20ms" --loss 5 --frames 600 --k 6
```

两个脚本都会**自动进入独立 netns**（`unshare -rn`），`tc netem` 只影响隔离环境，
不会动宿主网络，也不需要 sudo。

**注意**：`bench.sh` 有 `flock` 单实例锁。两个压测同时跑会抢端口、覆盖 CSV，
事后无法分辨数据来源——这是踩过的坑。

## 5. 打包与安装

```bash
cmake -S . -B build-deb -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLOWLAT_ENABLE_PACKAGING=ON -DLOWLAT_BUILD_TESTS=OFF
cmake --build build-deb -j
(cd build-deb && cpack -G DEB)

sudo dpkg -i build-deb/lowlat-video_0.1.0_amd64.deb
sudo systemctl start lowlat-receiver
```

包内文件：

```
/usr/bin/lowlat_sender  lowlat_receiver  lowlat_ui  h264_probe
/usr/lib/systemd/system/lowlat-receiver.service
/usr/share/doc/lowlat-video/README.md
```

systemd 单元用 `DynamicUser=yes` + `StateDirectory=lowlat`，
不需要 `postinst` 建用户；`ProtectSystem=strict` 保证进程除了状态目录什么都写不了。

## 6. 排障

| 现象 | 原因 | 解决 |
|---|---|---|
| 窗口不出现 | WSLg 窗口被别处遮住 | 看任务栏；或 `pkill -f lowlat_ui` 重启 |
| 打开文件失败 | 路径不存在 / 格式不支持 | 用绝对路径；`ffprobe 文件` 确认格式 |
| 画面全黑但没报错 | 视频第一帧就是黑的，或解码未出帧 | 换 `--frame 30` 试试 |
| 面板文字是方框 | 系统没有中文字体 | 界面文案已全英文；文件名含中文时面板会显示方框，`sudo apt install fonts-noto-cjk` 可解决 |
| **关窗口后摄像头灯还亮** | Windows 侧 ffmpeg 仍在占用设备 | 用 `./scripts/camera.sh`（自动清理）；或 `pkill -f ffmpeg.exe` |
| **摄像头打不开** | 设备被 usbipd 绑着，Windows 拿不到 | 管理员 PowerShell：`usbipd unbind --busid 1-6` |
| 摄像头状态 `Unknown` | 反复 bind/unbind 后枚举卡住 | 管理员 `usbipd unbind`，仍不行则重启 Windows |
| UDP 流收不到 | WSL IP 变了（重启后） | 用 `$(hostname -I \| awk '{print $1}')` 现取，别写死 |
| 帧率只有 10fps | USB 摄像头 YUYV 720p 上限就是 10fps | 改用 MJPG，或降到 640x480 |
| 端口被占用 | 上一个实例没退 | `pkill -f lowlat_` |
| 压测报"已有压测在运行" | `flock` 锁未释放 | 等它结束，或删 `out/bench/.bench.lock` |

## 7. 常用调试手段

```bash
# 码流是否完整（errors 必须为 0）
./build/tools/h264_probe out/recv.h264

# 单测过滤
./build/tests/lowlat_tests --gtest_filter='BandwidthEstimator.*'

# ThreadSanitizer
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
    -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST
cmake --build build-tsan -j --target lowlat_tests
setarch "$(uname -m)" -R ./build-tsan/tests/lowlat_tests

# 内存
valgrind --error-exitcode=99 --leak-check=full \
    ./build/src/app/lowlat_receiver --bind 127.0.0.1:5990 --frames 60 --output /tmp/x.h264

# 网络损伤（不用 root，在独立 netns 里）
unshare -rn sh -c 'ip link set lo up; tc qdisc replace dev lo root netem loss 10%; ...'
```

## 8. 界面命令行参数

```
--local <文件>      单机播放媒体文件（推荐）
--bind <addr:port>  接收远端推流
--input <h264>      离线校验模式
--window <WxH>      初始窗口尺寸，默认 1600x760（字号随高度缩放）
--bitrate <kbps>    单机模式码率，默认 2000
--loss <percent>    启动时的模拟丢包档位
--hold <ms>         帧保持窗口，默认 60
--nack              接收端请求重传
--feedback          回传时序样本（配合发送端 --cc）
--adaptive          自适应抖动缓冲
--duration <sec>    运行时长上限
--dump <prefix>     截图前缀
--dump-seq <n>      连拍 n 张（生成演示动图用）
```
