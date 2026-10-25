#include "capture/v4l2_source.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace lowlat {
namespace {

// ioctl 可能被信号打断，EINTR 时必须重试，否则会丢帧或误报错误。
int xioctl(int fd, unsigned long request, void* arg)
{
    int result = 0;
    do {
        result = ::ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

std::string with_errno(const std::string& what)
{
    return what + ": " + std::strerror(errno);
}

std::int64_t steady_now_us() noexcept
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

struct V4l2Source::Device {
    int fd{-1};
    std::vector<void*> buffers;
    std::vector<std::size_t> lengths;
    std::mutex mutex;
    std::atomic<bool> alive{false};
    std::atomic<std::uint64_t> requeue_failures{0};

    ~Device()
    {
        alive.store(false);
        for (std::size_t i = 0; i < buffers.size(); ++i) {
            if (buffers[i] != nullptr && buffers[i] != MAP_FAILED) {
                ::munmap(buffers[i], lengths[i]);
            }
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

V4l2Source::V4l2Source(std::string device, int width, int height, int fps,
                       std::uint32_t buffer_count)
    : device_(std::move(device)),
      requested_width_(width),
      requested_height_(height),
      fps_(fps),
      buffer_count_(buffer_count == 0 ? 4 : buffer_count)
{
}

V4l2Source::~V4l2Source()
{
    stop();
}

bool V4l2Source::start(std::string* error)
{
    if (streaming_) {
        if (error != nullptr) {
            *error = "V4L2 采集已启动";
        }
        return false;
    }
    if (requested_width_ <= 0 || requested_height_ <= 0 || (requested_width_ % 2) != 0 ||
        (requested_height_ % 2) != 0) {
        if (error != nullptr) {
            *error = "V4L2 采集要求宽高为正偶数";
        }
        return false;
    }
    if (fps_ <= 0) {
        if (error != nullptr) {
            *error = "V4L2 采集要求 fps > 0";
        }
        return false;
    }

    auto device = std::make_shared<Device>();
    device->fd = ::open(device_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (device->fd < 0) {
        if (error != nullptr) {
            *error = with_errno("打开设备失败 " + device_);
        }
        return false;
    }

    v4l2_capability capability{};
    if (xioctl(device->fd, VIDIOC_QUERYCAP, &capability) != 0) {
        if (error != nullptr) {
            *error = with_errno("VIDIOC_QUERYCAP 失败");
        }
        return false;
    }

    const std::uint32_t caps = ((capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0)
                                   ? capability.device_caps
                                   : capability.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        if (error != nullptr) {
            *error = "设备不支持单平面视频采集（V4L2_CAP_VIDEO_CAPTURE）";
        }
        return false;
    }
    if ((caps & V4L2_CAP_STREAMING) == 0) {
        if (error != nullptr) {
            *error = "设备不支持流式 IO（V4L2_CAP_STREAMING）";
        }
        return false;
    }

    // 格式协商：按优先级逐个尝试，接受驱动实际返回的格式。
    //
    // 实测（一台常见的笔记本内置摄像头，Bison 5986:118a）只提供 MJPG 与 YUYV，
    // 没有 YUV420。原先只请求 YUV420，导致 VIDIOC_S_FMT 直接失败——
    // 代码写完了却接不上任何真实摄像头。
    static const std::uint32_t kCandidates[] = {
        V4L2_PIX_FMT_YUV420,  // 最优先：可与 mmap 缓冲零拷贝
        V4L2_PIX_FMT_YUYV,    // 未压缩 4:2:2，UVC 最常见的格式
        V4L2_PIX_FMT_MJPEG,   // 压缩，带宽占用最小（USB/IP 下几乎是唯一可行选项）
    };

    v4l2_format format{};
    bool negotiated = false;
    std::uint32_t chosen_format = 0;
    for (std::uint32_t candidate : kCandidates) {
        format = v4l2_format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = static_cast<std::uint32_t>(requested_width_);
        format.fmt.pix.height = static_cast<std::uint32_t>(requested_height_);
        format.fmt.pix.pixelformat = candidate;
        format.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(device->fd, VIDIOC_S_FMT, &format) != 0) {
            continue;
        }
        // 驱动可能悄悄换成别的格式，只接受完全匹配的结果
        if (format.fmt.pix.pixelformat == candidate) {
            negotiated = true;
            chosen_format = candidate;
            break;
        }
    }
    if (!negotiated) {
        if (error != nullptr) {
            *error = "设备不支持任何可用像素格式（已尝试 YUV420 / YUYV / MJPG）";
        }
        return false;
    }

    const int actual_width = static_cast<int>(format.fmt.pix.width);
    const int actual_height = static_cast<int>(format.fmt.pix.height);
    const std::size_t stride = format.fmt.pix.bytesperline;

    if (!converter_.configure(chosen_format, actual_width, actual_height, stride, error)) {
        return false;
    }
    if (format.fmt.pix.sizeimage == 0) {
        if (error != nullptr) {
            *error = "驱动返回的缓冲区大小为 0";
        }
        return false;
    }

    // 非 YUV420 格式需要转换成 I420，不能把驱动缓冲直接交给上层，
    // 因此额外准备一个帧池；YUV420 仍走零拷贝路径，不分配。
    if (!converter_.is_passthrough()) {
        pool_ = FramePool::create(converter_.output_size(), buffer_count_ + 2);
        if (!pool_) {
            if (error != nullptr) {
                *error = "为格式转换创建帧池失败";
            }
            return false;
        }
    }

    v4l2_requestbuffers request{};
    request.count = buffer_count_;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(device->fd, VIDIOC_REQBUFS, &request) != 0) {
        if (error != nullptr) {
            *error = with_errno("VIDIOC_REQBUFS 失败");
        }
        return false;
    }
    if (request.count < 2) {
        if (error != nullptr) {
            *error = "驱动分配的缓冲区不足 2 个，无法做零拷贝采集";
        }
        return false;
    }

    device->buffers.assign(request.count, nullptr);
    device->lengths.assign(request.count, 0);

    for (std::uint32_t i = 0; i < request.count; ++i) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (xioctl(device->fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            if (error != nullptr) {
                *error = with_errno("VIDIOC_QUERYBUF 失败");
            }
            return false;
        }

        void* mapped = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                              device->fd, static_cast<off_t>(buffer.m.offset));
        if (mapped == MAP_FAILED) {
            if (error != nullptr) {
                *error = with_errno("mmap 失败");
            }
            return false;
        }
        device->buffers[i] = mapped;
        device->lengths[i] = buffer.length;

        // 所有缓冲区先入队，驱动才会开始填充。
        if (xioctl(device->fd, VIDIOC_QBUF, &buffer) != 0) {
            if (error != nullptr) {
                *error = with_errno("初始 VIDIOC_QBUF 失败");
            }
            return false;
        }
    }

    device->alive.store(true, std::memory_order_release);

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(device->fd, VIDIOC_STREAMON, &type) != 0) {
        if (error != nullptr) {
            *error = with_errno("VIDIOC_STREAMON 失败");
        }
        device->alive.store(false, std::memory_order_release);
        return false;
    }

    device_handle_ = std::move(device);
    actual_width_ = actual_width;
    actual_height_ = actual_height;
    frame_index_ = 0;
    streaming_ = true;
    return true;
}

void V4l2Source::stop() noexcept
{
    if (!streaming_ || !device_handle_) {
        return;
    }

    // 先置 alive=false：此后释放的在途帧不会再 QBUF 已停止的队列。
    device_handle_->alive.store(false, std::memory_order_release);

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    (void)xioctl(device_handle_->fd, VIDIOC_STREAMOFF, &type);

    // 真正的 munmap/close 由 Device 析构完成，可能在最后一个在途帧释放之后。
    device_handle_.reset();
    streaming_ = false;
}

bool V4l2Source::next_frame(YuvFrame* out, std::string* error)
{
    if (!streaming_ || !device_handle_) {
        if (error != nullptr) {
            *error = "V4L2 采集未启动";
        }
        return false;
    }
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "next_frame 输出指针为空";
        }
        return false;
    }

    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;

    while (true) {
        if (xioctl(device_handle_->fd, VIDIOC_DQBUF, &buffer) == 0) {
            break;
        }
        if (errno != EAGAIN) {
            if (error != nullptr) {
                *error = with_errno("VIDIOC_DQBUF 失败");
            }
            return false;
        }

        struct pollfd pfd;
        pfd.fd = device_handle_->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int ready = ::poll(&pfd, 1, 2000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (error != nullptr) {
                *error = with_errno("poll 等待帧失败");
            }
            return false;
        }
        if (ready == 0) {
            if (error != nullptr) {
                *error = "等待帧超时（2s）";
            }
            return false;
        }
    }

    if (buffer.index >= device_handle_->buffers.size()) {
        if (error != nullptr) {
            *error = "驱动返回非法缓冲区索引 " + std::to_string(buffer.index);
        }
        return false;
    }

    // 驱动给出的是 CLOCK_MONOTONIC 时间戳，与本项目的 steady_clock 基准一致，
    // 直接采用可以避免"取帧后再打点"引入的额外偏差。
    std::int64_t capture_ts_us = 0;
    if ((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) != 0) {
        capture_ts_us = static_cast<std::int64_t>(buffer.timestamp.tv_sec) * 1000000 +
                        static_cast<std::int64_t>(buffer.timestamp.tv_usec);
    } else {
        capture_ts_us = steady_now_us();
    }

    auto* raw = static_cast<std::uint8_t*>(device_handle_->buffers[buffer.index]);
    const std::uint32_t index = buffer.index;
    std::shared_ptr<Device> owner = device_handle_;

    auto requeue = [owner, index]() noexcept {
        if (!owner->alive.load(std::memory_order_acquire)) {
            return;
        }
        v4l2_buffer again{};
        again.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        again.memory = V4L2_MEMORY_MMAP;
        again.index = index;
        std::lock_guard<std::mutex> lock(owner->mutex);
        if (xioctl(owner->fd, VIDIOC_QBUF, &again) != 0) {
            owner->requeue_failures.fetch_add(1, std::memory_order_relaxed);
        }
    };

    if (!converter_.is_passthrough()) {
        // 需要格式转换：拷进帧池后立刻把驱动缓冲还回去，
        // 否则转换期间该缓冲一直被占用，可用槽位会越来越少。
        auto converted = pool_->acquire();
        if (!converted) {
            requeue();
            if (error != nullptr) {
                *error = "转换帧池耗尽";
            }
            return false;
        }
        if (!converter_.convert(raw, buffer.bytesused, converted.get(), error)) {
            requeue();
            return false;
        }
        requeue();

        *out = YuvFrame(frame_index_, capture_ts_us, actual_width_, actual_height_,
                        std::move(converted));
        ++frame_index_;
        return true;
    }

    // YUV420 紧凑布局：直接把 mmap 区包装成帧，最后一个引用释放时还回驱动。
    auto holder = std::shared_ptr<std::uint8_t>(raw, [requeue](std::uint8_t* /*unused*/) noexcept {
        requeue();
    });

    *out = YuvFrame(frame_index_, capture_ts_us, actual_width_, actual_height_, std::move(holder));
    ++frame_index_;
    return true;
}

int V4l2Source::width() const noexcept
{
    return streaming_ ? actual_width_ : requested_width_;
}

int V4l2Source::height() const noexcept
{
    return streaming_ ? actual_height_ : requested_height_;
}

std::uint64_t V4l2Source::requeue_failures() const noexcept
{
    return device_handle_ ? device_handle_->requeue_failures.load(std::memory_order_relaxed) : 0;
}

}  // namespace lowlat
