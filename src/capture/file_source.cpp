#include "capture/file_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>

#include "common/ffmpeg_headers.hpp"  // avcodec / avutil（自带 extern "C" 保护）
#include "common/frame_pool.hpp"

// ffmpeg_headers.hpp 只包了编解码相关的基础头；
// 文件源额外需要 demux 与色彩转换，这里单独补一个 extern "C" 块。
// 嵌套 extern "C" 对标准头是安全的。
extern "C" {
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace lowlat {
namespace {

std::int64_t steady_now_us() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string ffmpeg_error(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

}  // namespace

struct FileSource::Impl {
    std::string path;
    int out_fps{0};
    bool loop{false};
    bool realtime{true};

    AVFormatContext* format{nullptr};
    AVCodecContext* decoder{nullptr};
    AVFrame* decoded{nullptr};
    AVPacket* packet{nullptr};
    SwsContext* scaler{nullptr};
    int stream_index{-1};

    int width{0};
    int height{0};
    bool running{false};
    bool input_ended{false};
    std::uint32_t frame_index{0};

    std::shared_ptr<FramePool> pool;
    std::chrono::steady_clock::time_point start_time{};

    void close_input() noexcept
    {
        if (scaler != nullptr) {
            sws_freeContext(scaler);
            scaler = nullptr;
        }
        if (decoded != nullptr) {
            av_frame_free(&decoded);
        }
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (decoder != nullptr) {
            avcodec_free_context(&decoder);
        }
        if (format != nullptr) {
            avformat_close_input(&format);
        }
        stream_index = -1;
    }

    void release() noexcept
    {
        close_input();
        pool.reset();
    }

    bool open_input(std::string* error);

    bool rewind(std::string* error);

    // 把解码帧搬进 I420 缓冲。目标缓冲是紧凑布局（stride == 平面宽）。
    void convert(const AVFrame* source, YuvFrame* out) const
    {
        std::uint8_t* dst[4] = {out->y_plane(), out->u_plane(), out->v_plane(), nullptr};
        const int dst_linesize[4] = {width, width / 2, width / 2, 0};

        if (source->format == AV_PIX_FMT_YUV420P) {
            for (int plane = 0; plane < 3; ++plane) {
                const int plane_height = plane == 0 ? height : height / 2;
                const int plane_width = plane == 0 ? width : width / 2;
                const int src_stride = source->linesize[plane];
                const std::uint8_t* src = source->data[plane];
                if (src_stride == plane_width) {
                    std::memcpy(dst[plane], src,
                                static_cast<std::size_t>(plane_width) *
                                    static_cast<std::size_t>(plane_height));
                } else {
                    for (int row = 0; row < plane_height; ++row) {
                        std::memcpy(dst[plane] + static_cast<std::ptrdiff_t>(row) * plane_width,
                                    src + static_cast<std::ptrdiff_t>(row) * src_stride,
                                    static_cast<std::size_t>(plane_width));
                    }
                }
            }
            return;
        }

        sws_scale(scaler, source->data, source->linesize, 0, height, dst, dst_linesize);
    }
};

bool FileSource::Impl::open_input(std::string* error)
{
    int ret = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        if (error != nullptr) {
            *error = "打开文件失败 " + path + ": " + ffmpeg_error(ret);
        }
        return false;
    }

    ret = avformat_find_stream_info(format, nullptr);
    if (ret < 0) {
        if (error != nullptr) {
            *error = "读取流信息失败: " + ffmpeg_error(ret);
        }
        return false;
    }

    stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        if (error != nullptr) {
            *error = "文件里没有视频流: " + path;
        }
        return false;
    }

    const AVStream* stream = format->streams[stream_index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        if (error != nullptr) {
            *error = "找不到对应的解码器";
        }
        return false;
    }

    decoder = avcodec_alloc_context3(codec);
    if (decoder == nullptr) {
        if (error != nullptr) {
            *error = "分配解码器上下文失败";
        }
        return false;
    }
    if (avcodec_parameters_to_context(decoder, stream->codecpar) < 0 ||
        avcodec_open2(decoder, codec, nullptr) < 0) {
        if (error != nullptr) {
            *error = "打开解码器失败";
        }
        return false;
    }

    const int new_width = decoder->width;
    const int new_height = decoder->height;
    if (new_width <= 0 || new_height <= 0) {
        if (error != nullptr) {
            *error = "文件分辨率非法";
        }
        return false;
    }
    // 重开（循环）时分辨率必须一致，否则帧池尺寸对不上
    if (width != 0 && (new_width != width || new_height != height)) {
        if (error != nullptr) {
            *error = "重新打开后分辨率发生变化，无法循环播放";
        }
        return false;
    }
    // I420 要求偶数尺寸；奇数尺寸直接拒绝，避免后续 stride 计算错位
    if ((new_width % 2) != 0 || (new_height % 2) != 0) {
        if (error != nullptr) {
            *error = "文件分辨率不是偶数，I420 无法处理: " + std::to_string(new_width) + "x" +
                     std::to_string(new_height);
        }
        return false;
    }
    width = new_width;
    height = new_height;

    if (out_fps <= 0) {
        // 未指定时沿用文件帧率。注意裸 H.264 ES 没有帧率信息，
        // demuxer 会退回一个猜测值（常见 25），此时用 --fps 显式覆盖。
        AVRational rate = stream->avg_frame_rate;
        if (rate.num <= 0 || rate.den <= 0) {
            rate = stream->r_frame_rate;
        }
        out_fps = (rate.num > 0 && rate.den > 0) ? (rate.num / rate.den) : 30;
        if (out_fps <= 0) {
            out_fps = 30;
        }
    }

    decoded = av_frame_alloc();
    packet = av_packet_alloc();
    if (decoded == nullptr || packet == nullptr) {
        if (error != nullptr) {
            *error = "分配帧/包对象失败";
        }
        return false;
    }

    scaler = sws_getContext(width, height, decoder->pix_fmt, width, height, AV_PIX_FMT_YUV420P,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (scaler == nullptr && decoder->pix_fmt != AV_PIX_FMT_YUV420P) {
        if (error != nullptr) {
            *error = "初始化色彩转换失败";
        }
        return false;
    }

    return true;
}

bool FileSource::Impl::rewind(std::string* error)
{
    // 优先用 seek：O(1)，不重建解码器
    if (av_seek_frame(format, stream_index, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers(decoder);
        input_ended = false;
        return true;
    }

    // 有些格式（例如裸 H.264 ES）不支持 seek，只能整个重开。
    // 不做这个回退的话，--loop 对这类文件会静默失效。
    close_input();
    if (!open_input(error)) {
        return false;
    }
    input_ended = false;
    return true;
}

FileSource::FileSource(std::string path, int fps, bool loop, bool realtime)
    : impl_(std::make_unique<Impl>())
{
    impl_->path = std::move(path);
    impl_->out_fps = fps;
    impl_->loop = loop;
    impl_->realtime = realtime;
}

FileSource::~FileSource()
{
    stop();
}

bool FileSource::start(std::string* error)
{
    if (impl_->running) {
        return true;
    }

    if (!impl_->open_input(error)) {
        impl_->release();
        return false;
    }

    const std::size_t need = static_cast<std::size_t>(impl_->width) *
                             static_cast<std::size_t>(impl_->height) * 3 / 2;
    impl_->pool = FramePool::create(need, 4);
    if (!impl_->pool) {
        if (error != nullptr) {
            *error = "创建帧池失败";
        }
        impl_->release();
        return false;
    }

    impl_->frame_index = 0;
    impl_->input_ended = false;
    impl_->start_time = std::chrono::steady_clock::now();
    impl_->running = true;
    return true;
}

void FileSource::stop() noexcept
{
    if (impl_ == nullptr) {
        return;
    }
    impl_->running = false;
    impl_->release();
}

bool FileSource::next_frame(YuvFrame* out, std::string* error)
{
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "next_frame 输出指针为空";
        }
        return false;
    }
    if (!impl_->running) {
        if (error != nullptr) {
            *error = "文件源尚未启动";
        }
        return false;
    }

    if (impl_->realtime) {
        const std::chrono::microseconds period(1000000 / impl_->out_fps);
        const auto deadline =
            impl_->start_time + period * static_cast<std::int64_t>(impl_->frame_index);
        std::this_thread::sleep_until(deadline);
    }

    for (;;) {
        const int ret = avcodec_receive_frame(impl_->decoder, impl_->decoded);
        if (ret == 0) {
            auto buffer = impl_->pool->acquire();
            if (!buffer) {
                if (error != nullptr) {
                    *error = "帧池耗尽";
                }
                return false;
            }

            YuvFrame frame(impl_->frame_index, steady_now_us(), impl_->width, impl_->height,
                           std::move(buffer));
            impl_->convert(impl_->decoded, &frame);
            av_frame_unref(impl_->decoded);

            *out = std::move(frame);
            ++impl_->frame_index;
            return true;
        }

        if (ret == AVERROR_EOF) {
            if (!impl_->loop) {
                return false;  // 正常播放结束
            }
            if (!impl_->rewind(error)) {
                return false;
            }
            continue;
        }

        if (ret != AVERROR(EAGAIN)) {
            if (error != nullptr) {
                *error = "解码失败: " + ffmpeg_error(ret);
            }
            return false;
        }

        if (impl_->input_ended) {
            // 已经投递过 flush 包却还是 EAGAIN，不再继续等，防止死循环
            return false;
        }

        const int read_ret = av_read_frame(impl_->format, impl_->packet);
        if (read_ret == AVERROR_EOF) {
            avcodec_send_packet(impl_->decoder, nullptr);  // 冲刷解码器
            impl_->input_ended = true;
            continue;
        }
        if (read_ret < 0) {
            if (error != nullptr) {
                *error = "读取文件失败: " + ffmpeg_error(read_ret);
            }
            return false;
        }

        if (impl_->packet->stream_index == impl_->stream_index) {
            avcodec_send_packet(impl_->decoder, impl_->packet);
        }
        av_packet_unref(impl_->packet);
    }
}

int FileSource::width() const noexcept
{
    return impl_->width;
}

int FileSource::height() const noexcept
{
    return impl_->height;
}

int FileSource::fps() const noexcept
{
    return impl_->out_fps > 0 ? impl_->out_fps : 30;
}

std::int64_t FileSource::next_frame_due_us() const noexcept
{
    if (!impl_->realtime || !impl_->running || impl_->out_fps <= 0) {
        return 0;
    }
    const auto period = std::chrono::microseconds(1000000 / impl_->out_fps);
    const auto deadline =
        impl_->start_time + period * static_cast<std::int64_t>(impl_->frame_index);
    return std::chrono::duration_cast<std::chrono::microseconds>(deadline.time_since_epoch())
        .count();
}

const std::string& FileSource::path() const noexcept
{
    return impl_->path;
}

}  // namespace lowlat
