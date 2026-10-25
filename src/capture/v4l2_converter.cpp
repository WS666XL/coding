#include "capture/v4l2_converter.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "common/ffmpeg_headers.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <linux/videodev2.h>
}

namespace lowlat {
namespace {

const char* ffmpeg_error_text(int code, char* buffer, std::size_t size)
{
    av_strerror(code, buffer, size);
    return buffer;
}

}  // namespace

struct V4l2Converter::Impl {
    std::uint32_t format{0};
    int width{0};
    int height{0};
    std::size_t stride{0};

    // YUYV 与 MJPG 解码后都用它做色彩转换
    SwsContext* sws{nullptr};
    int sws_src_format{-1};

    // MJPG
    AVCodecContext* mjpeg{nullptr};
    AVFrame* frame{nullptr};
    AVPacket* packet{nullptr};

    void release() noexcept
    {
        if (sws != nullptr) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        sws_src_format = -1;
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (mjpeg != nullptr) {
            avcodec_free_context(&mjpeg);
        }
        format = 0;
        width = 0;
        height = 0;
        stride = 0;
    }
};

V4l2Converter::V4l2Converter() : impl_(std::make_unique<Impl>()) {}

V4l2Converter::~V4l2Converter()
{
    if (impl_ != nullptr) {
        impl_->release();
    }
}

void V4l2Converter::reset() noexcept
{
    if (impl_ != nullptr) {
        impl_->release();
    }
}

bool V4l2Converter::configure(std::uint32_t v4l2_pixel_format, int width, int height,
                              std::size_t bytes_per_line, std::string* error)
{
    if (impl_ == nullptr) {
        return false;
    }
    impl_->release();

    if (width <= 0 || height <= 0) {
        if (error != nullptr) {
            *error = "转换器配置的尺寸非法";
        }
        return false;
    }
    if ((width % 2) != 0 || (height % 2) != 0) {
        if (error != nullptr) {
            *error = "I420 要求宽高均为偶数";
        }
        return false;
    }

    impl_->format = v4l2_pixel_format;
    impl_->width = width;
    impl_->height = height;
    impl_->stride = bytes_per_line != 0 ? bytes_per_line
                                        : static_cast<std::size_t>(width);

    switch (v4l2_pixel_format) {
        case V4L2_PIX_FMT_YUV420:
            if (impl_->stride < static_cast<std::size_t>(width)) {
                if (error != nullptr) {
                    *error = "YUV420 的 bytesperline 小于宽度";
                }
                return false;
            }
            return true;

        case V4L2_PIX_FMT_YUYV:
            if (impl_->stride < static_cast<std::size_t>(width) * 2u) {
                if (error != nullptr) {
                    *error = "YUYV 的 bytesperline 小于 width*2";
                }
                return false;
            }
            return true;

        case V4L2_PIX_FMT_MJPEG: {
            const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
            if (codec == nullptr) {
                if (error != nullptr) {
                    *error = "FFmpeg 缺少 MJPEG 解码器";
                }
                return false;
            }
            impl_->mjpeg = avcodec_alloc_context3(codec);
            if (impl_->mjpeg == nullptr) {
                if (error != nullptr) {
                    *error = "分配 MJPEG 解码上下文失败";
                }
                return false;
            }
            impl_->mjpeg->width = width;
            impl_->mjpeg->height = height;
            if (avcodec_open2(impl_->mjpeg, codec, nullptr) < 0) {
                if (error != nullptr) {
                    *error = "打开 MJPEG 解码器失败";
                }
                return false;
            }
            impl_->frame = av_frame_alloc();
            impl_->packet = av_packet_alloc();
            if (impl_->frame == nullptr || impl_->packet == nullptr) {
                if (error != nullptr) {
                    *error = "分配 MJPEG 解码帧失败";
                }
                return false;
            }
            return true;
        }

        default:
            if (error != nullptr) {
                *error = "不支持的 V4L2 像素格式: " +
                         std::to_string(static_cast<unsigned>(v4l2_pixel_format));
            }
            return false;
    }
}

bool V4l2Converter::is_passthrough() const noexcept
{
    return impl_ != nullptr && impl_->format == V4L2_PIX_FMT_YUV420 &&
           impl_->stride == static_cast<std::size_t>(impl_->width);
}

std::size_t V4l2Converter::output_size() const noexcept
{
    if (impl_ == nullptr) {
        return 0;
    }
    return static_cast<std::size_t>(impl_->width) * static_cast<std::size_t>(impl_->height) *
           3u / 2u;
}

const char* V4l2Converter::format_name() const noexcept
{
    if (impl_ == nullptr || impl_->format == 0) {
        return "none";
    }
    switch (impl_->format) {
        case V4L2_PIX_FMT_YUV420:
            return "YUV420";
        case V4L2_PIX_FMT_YUYV:
            return "YUYV";
        case V4L2_PIX_FMT_MJPEG:
            return "MJPG";
        default:
            return "unknown";
    }
}

namespace {

// YUYV(4:2:2) -> I420(4:2:0)。
//
// YUYV 每个 4 字节描述两个相邻像素：Y0 U Y1 V。
// 转 4:2:0 时色度要按 2x2 块下采样，所以纵向也要两行取平均——
// 只取奇数行或偶数行都会让色度整体偏移半像素。
void yuyv_to_i420(const std::uint8_t* src, std::size_t stride, int width, int height,
                  std::uint8_t* dst)
{
    const std::size_t y_size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const int chroma_width = width / 2;
    std::uint8_t* dst_y = dst;
    std::uint8_t* dst_u = dst + y_size;
    std::uint8_t* dst_v = dst_u + y_size / 4;

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = src + static_cast<std::size_t>(y) * stride;
        std::uint8_t* out_row = dst_y + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = 0; x < width; x += 2) {
            const std::size_t offset = static_cast<std::size_t>(x) * 2u;
            out_row[x] = row[offset];
            out_row[x + 1] = row[offset + 2];
        }
    }

    for (int y = 0; y < height; y += 2) {
        const std::uint8_t* row0 = src + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* row1 = src + static_cast<std::size_t>(y + 1) * stride;
        const int chroma_row = y / 2;
        for (int x = 0; x < width; x += 2) {
            const std::size_t offset = static_cast<std::size_t>(x) * 2u;
            const int u = (static_cast<int>(row0[offset + 1]) + static_cast<int>(row1[offset + 1])) / 2;
            const int v = (static_cast<int>(row0[offset + 3]) + static_cast<int>(row1[offset + 3])) / 2;
            dst_u[static_cast<std::size_t>(chroma_row) * chroma_width + x / 2] =
                static_cast<std::uint8_t>(u);
            dst_v[static_cast<std::size_t>(chroma_row) * chroma_width + x / 2] =
                static_cast<std::uint8_t>(v);
        }
    }
}

}  // namespace

bool V4l2Converter::convert(const std::uint8_t* src, std::size_t size, std::uint8_t* dst,
                            std::string* error)
{
    if (impl_ == nullptr || src == nullptr || dst == nullptr) {
        if (error != nullptr) {
            *error = "转换器参数为空";
        }
        return false;
    }

    const std::size_t y_size =
        static_cast<std::size_t>(impl_->width) * static_cast<std::size_t>(impl_->height);
    const int width = impl_->width;
    const int height = impl_->height;

    switch (impl_->format) {
        case V4L2_PIX_FMT_YUV420: {
            // 有行填充：Y 平面按宽度逐行搬，两个色度平面的 stride 是 rows_stride / 2
            const std::size_t chroma_stride = impl_->stride / 2u;
            const std::size_t chroma_width = static_cast<std::size_t>(width) / 2u;
            const std::size_t chroma_height = static_cast<std::size_t>(height) / 2u;

            for (int row = 0; row < height; ++row) {
                std::memcpy(dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(width),
                            src + static_cast<std::size_t>(row) * impl_->stride,
                            static_cast<std::size_t>(width));
            }
            const std::uint8_t* src_u = src + impl_->stride * static_cast<std::size_t>(height);
            const std::uint8_t* src_v =
                src_u + chroma_stride * chroma_height;
            std::uint8_t* dst_u = dst + y_size;
            std::uint8_t* dst_v = dst_u + y_size / 4;
            for (std::size_t row = 0; row < chroma_height; ++row) {
                std::memcpy(dst_u + row * chroma_width, src_u + row * chroma_stride, chroma_width);
                std::memcpy(dst_v + row * chroma_width, src_v + row * chroma_stride, chroma_width);
            }
            return true;
        }

        case V4L2_PIX_FMT_YUYV: {
            const std::size_t needed =
                impl_->stride * static_cast<std::size_t>(height);
            if (size < needed) {
                if (error != nullptr) {
                    *error = "YUYV 缓冲过小";
                }
                return false;
            }
            yuyv_to_i420(src, impl_->stride, width, height, dst);
            return true;
        }

        case V4L2_PIX_FMT_MJPEG: {
            if (impl_->mjpeg == nullptr || impl_->frame == nullptr) {
                if (error != nullptr) {
                    *error = "MJPEG 解码器未初始化";
                }
                return false;
            }

            av_packet_unref(impl_->packet);
            const int alloc = av_new_packet(impl_->packet, static_cast<int>(size));
            if (alloc < 0) {
                if (error != nullptr) {
                    *error = "为 MJPEG 帧分配 packet 失败";
                }
                return false;
            }
            std::memcpy(impl_->packet->data, src, size);

            const int sent = avcodec_send_packet(impl_->mjpeg, impl_->packet);
            av_packet_unref(impl_->packet);
            if (sent < 0) {
                if (error != nullptr) {
                    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
                    *error = std::string("MJPEG 送包失败: ") +
                             ffmpeg_error_text(sent, text, sizeof(text));
                }
                return false;
            }

            const int received = avcodec_receive_frame(impl_->mjpeg, impl_->frame);
            if (received < 0) {
                if (error != nullptr) {
                    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
                    *error = std::string("MJPEG 解码失败: ") +
                             ffmpeg_error_text(received, text, sizeof(text));
                }
                return false;
            }

            // 解码输出格式可能是 YUVJ422P / YUVJ420P，按实际格式惰性建 sws 上下文
            if (impl_->sws == nullptr || impl_->sws_src_format != impl_->frame->format) {
                if (impl_->sws != nullptr) {
                    sws_freeContext(impl_->sws);
                    impl_->sws = nullptr;
                }
                impl_->sws = sws_getContext(width, height,
                                            static_cast<AVPixelFormat>(impl_->frame->format),
                                            width, height, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                            nullptr, nullptr, nullptr);
                if (impl_->sws == nullptr) {
                    if (error != nullptr) {
                        *error = "为 MJPEG 输出建立色彩转换失败";
                    }
                    return false;
                }
                impl_->sws_src_format = impl_->frame->format;
            }

            std::uint8_t* dst_planes[4] = {dst, dst + y_size, dst + y_size + y_size / 4, nullptr};
            const int dst_linesize[4] = {width, width / 2, width / 2, 0};
            sws_scale(impl_->sws, impl_->frame->data, impl_->frame->linesize, 0, height,
                      dst_planes, dst_linesize);
            av_frame_unref(impl_->frame);
            return true;
        }

        default:
            if (error != nullptr) {
                *error = "转换器格式未配置";
            }
            return false;
    }
}

}  // namespace lowlat
