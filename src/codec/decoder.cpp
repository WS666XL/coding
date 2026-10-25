#include "codec/decoder.hpp"

#include <cstring>
#include <utility>

#include "common/ffmpeg_headers.hpp"

namespace lowlat {
namespace {

constexpr std::uint8_t kStartCode[4] = {0, 0, 0, 1};

std::string av_error_text(const char* what, int code)
{
    char buffer[256] = {0};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(what) + ": " + buffer;
}

}  // namespace

struct H264Decoder::Impl {
    AVCodecContext* context{nullptr};
    AVFrame* frame{nullptr};
    AVPacket* packet{nullptr};
    AVCodecParserContext* parser{nullptr};
    std::vector<std::uint8_t> scratch;
    // 交给解析器的缓冲区必须带 AV_INPUT_BUFFER_PADDING_SIZE 的零填充：
    // H.264 解析器会预读起始码之后若干字节，不填充会越界读（valgrind 可复现）。
    std::vector<std::uint8_t> padded;

    std::uint64_t decoded_frames{0};
    std::uint64_t send_errors{0};
    std::uint64_t recv_errors{0};

    // 把解码器里所有可用帧取出来
    bool drain(std::vector<H264Decoder::Frame>* out);
    // 送入一个完整访问单元
    bool send_access_unit(const std::uint8_t* data, int size,
                          std::vector<H264Decoder::Frame>* out);

    ~Impl()
    {
        if (parser != nullptr) {
            av_parser_close(parser);
        }
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
    }
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}

H264Decoder::~H264Decoder()
{
    close();
}

bool H264Decoder::open(std::string* error)
{
    close();
    impl_ = std::make_unique<Impl>();

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        if (error != nullptr) {
            *error = "未找到 H.264 解码器";
        }
        return false;
    }

    impl_->context = avcodec_alloc_context3(codec);
    if (impl_->context == nullptr) {
        if (error != nullptr) {
            *error = "avcodec_alloc_context3 失败";
        }
        return false;
    }

    const int opened = avcodec_open2(impl_->context, codec, nullptr);
    if (opened < 0) {
        if (error != nullptr) {
            *error = av_error_text("avcodec_open2 失败", opened);
        }
        impl_ = std::make_unique<Impl>();
        return false;
    }

    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    impl_->parser = av_parser_init(AV_CODEC_ID_H264);
    if (impl_->frame == nullptr || impl_->packet == nullptr || impl_->parser == nullptr) {
        if (error != nullptr) {
            *error = "解码器资源分配失败";
        }
        impl_ = std::make_unique<Impl>();
        return false;
    }

    return true;
}

void H264Decoder::close() noexcept
{
    impl_ = std::make_unique<Impl>();
}

bool H264Decoder::is_open() const noexcept
{
    return impl_ != nullptr && impl_->context != nullptr;
}

bool H264Decoder::Impl::drain(std::vector<H264Decoder::Frame>* out)
{
    while (true) {
        const int received = avcodec_receive_frame(context, frame);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
            return true;
        }
        if (received < 0) {
            ++recv_errors;
            return false;
        }

        // 把引用整体搬走，下一次 receive 会拿到新的缓冲
        AVFrame* owned = av_frame_alloc();
        if (owned == nullptr) {
            av_frame_unref(frame);
            return false;
        }
        av_frame_move_ref(owned, frame);

        H264Decoder::Frame output;
        output.width = owned->width;
        output.height = owned->height;
        output.y = owned->data[0];
        output.u = owned->data[1];
        output.v = owned->data[2];
        output.stride_y = owned->linesize[0];
        output.stride_u = owned->linesize[1];
        output.stride_v = owned->linesize[2];
        output.owner = std::shared_ptr<AVFrame>(owned, [](AVFrame* value) {
            av_frame_free(&value);
        });

        if (output.y == nullptr || output.u == nullptr || output.v == nullptr) {
            // 非 YUV420P 输出（理论上不会发生），丢弃
            continue;
        }

        out->push_back(std::move(output));
        ++decoded_frames;
    }
}

bool H264Decoder::Impl::send_access_unit(const std::uint8_t* data, int size,
                                         std::vector<H264Decoder::Frame>* out)
{
    av_packet_unref(packet);
    if (av_new_packet(packet, size) < 0) {
        ++send_errors;
        return false;
    }
    std::memcpy(packet->data, data, static_cast<std::size_t>(size));

    const int sent = avcodec_send_packet(context, packet);
    av_packet_unref(packet);
    if (sent < 0 && sent != AVERROR(EAGAIN)) {
        ++send_errors;
        return false;
    }
    return drain(out);
}

bool H264Decoder::feed_annexb(ByteSpan data, std::vector<Frame>* out, std::string* error)
{
    if (!is_open()) {
        if (error != nullptr) {
            *error = "解码器尚未打开";
        }
        return false;
    }
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "feed_annexb 输出指针为空";
        }
        return false;
    }
    if (data.empty()) {
        return true;
    }

    // 复制到带零填充的缓冲，满足 FFmpeg 对输入缓冲的硬性要求
    const std::size_t padding = AV_INPUT_BUFFER_PADDING_SIZE;
    impl_->padded.resize(data.size() + padding);
    std::memcpy(impl_->padded.data(), data.data(), data.size());
    std::memset(impl_->padded.data() + data.size(), 0, padding);

    const std::uint8_t* cursor = impl_->padded.data();
    int remaining = static_cast<int>(data.size());

    while (remaining > 0) {
        std::uint8_t* access_unit = nullptr;
        int access_unit_size = 0;

        const int consumed = av_parser_parse2(
            impl_->parser, impl_->context, &access_unit, &access_unit_size, cursor, remaining,
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (consumed < 0) {
            if (error != nullptr) {
                *error = av_error_text("码流解析失败", consumed);
            }
            return false;
        }

        cursor += consumed;
        remaining -= consumed;

        if (access_unit_size > 0) {
            if (!impl_->send_access_unit(access_unit, access_unit_size, out)) {
                if (error != nullptr) {
                    *error = "送包到解码器失败";
                }
                return false;
            }
        }

        if (consumed == 0 && access_unit_size == 0) {
            break;  // 解析器没有进展，避免死循环
        }
    }
    return true;
}

bool H264Decoder::feed_nal(ByteSpan nal, std::vector<Frame>* out, std::string* error)
{
    if (!is_open()) {
        if (error != nullptr) {
            *error = "解码器尚未打开";
        }
        return false;
    }
    if (nal.empty()) {
        if (error != nullptr) {
            *error = "feed_nal 收到空 NAL";
        }
        return false;
    }

    // 解析器需要起始码才能切分访问单元
    impl_->scratch.resize(sizeof(kStartCode) + nal.size());
    std::memcpy(impl_->scratch.data(), kStartCode, sizeof(kStartCode));
    std::memcpy(impl_->scratch.data() + sizeof(kStartCode), nal.data(), nal.size());

    return feed_annexb(ByteSpan(impl_->scratch.data(), impl_->scratch.size()), out, error);
}

bool H264Decoder::flush(std::vector<Frame>* out, std::string* error)
{
    if (!is_open()) {
        if (error != nullptr) {
            *error = "解码器尚未打开";
        }
        return false;
    }
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "flush 输出指针为空";
        }
        return false;
    }

    // 先冲刷解析器：它会把最后一个访问单元一直压着，直到看见下一个 AU 的起始码。
    // 不冲这一步会少解一帧（实测 10 帧只出 9 帧）。
    while (true) {
        std::uint8_t* access_unit = nullptr;
        int access_unit_size = 0;
        const int consumed = av_parser_parse2(impl_->parser, impl_->context, &access_unit,
                                              &access_unit_size, nullptr, 0, AV_NOPTS_VALUE,
                                              AV_NOPTS_VALUE, 0);
        if (access_unit_size <= 0) {
            break;
        }
        if (!impl_->send_access_unit(access_unit, access_unit_size, out)) {
            if (error != nullptr) {
                *error = "冲刷解析器时送包失败";
            }
            return false;
        }
        if (consumed <= 0) {
            break;
        }
    }

    const int sent = avcodec_send_packet(impl_->context, nullptr);
    if (sent < 0 && sent != AVERROR_EOF) {
        if (error != nullptr) {
            *error = av_error_text("解码器 flush 失败", sent);
        }
        return false;
    }
    return impl_->drain(out);
}

std::uint64_t H264Decoder::decoded_frames() const noexcept
{
    return impl_ != nullptr ? impl_->decoded_frames : 0;
}

std::uint64_t H264Decoder::send_errors() const noexcept
{
    return impl_ != nullptr ? impl_->send_errors : 0;
}

}  // namespace lowlat
