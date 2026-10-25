// h264_probe：解析 Annex-B 码流并用 libavcodec 解码，统计帧数与错误。
//
// 用途：在没有 ffprobe 的环境里做可自动化的验收（例如"连播 60s 无花屏"）。
// 退出码：0 正常；1 参数错误；2 运行失败或解码出错。

#include "codec/ffmpeg_headers.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int drain_frames(AVCodecContext* context, AVFrame* frame, int* frames, int* width, int* height,
                 int* pix_fmt)
{
    while (true) {
        const int received = avcodec_receive_frame(context, frame);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
            return 0;
        }
        if (received < 0) {
            return -1;
        }
        if (*frames == 0) {
            *width = frame->width;
            *height = frame->height;
            *pix_fmt = frame->format;
        }
        ++(*frames);
        av_frame_unref(frame);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "用法: h264_probe <file.h264>\n");
        return 1;
    }

    const std::string path = argv[1];
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "无法打开文件: %s\n", path.c_str());
        return 2;
    }
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)),
                                         std::istreambuf_iterator<char>());
    if (data.empty()) {
        std::fprintf(stderr, "文件为空: %s\n", path.c_str());
        return 2;
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        std::fprintf(stderr, "未找到 H.264 解码器\n");
        return 2;
    }

    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == nullptr) {
        std::fprintf(stderr, "avcodec_alloc_context3 失败\n");
        return 2;
    }
    if (avcodec_open2(context, codec, nullptr) < 0) {
        std::fprintf(stderr, "avcodec_open2 失败\n");
        avcodec_free_context(&context);
        return 2;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    if (frame == nullptr || packet == nullptr || parser == nullptr) {
        std::fprintf(stderr, "解码器/解析器分配失败\n");
        av_frame_free(&frame);
        av_packet_free(&packet);
        av_parser_close(parser);
        avcodec_free_context(&context);
        return 2;
    }

    int frames = 0;
    int width = 0;
    int height = 0;
    int pix_fmt = -1;
    int errors = 0;
    int access_units = 0;

    // 把一个 access unit 送进解码器并取回所有可用帧。
    auto feed = [&](const std::uint8_t* payload, int payload_size) -> bool {
        if (payload_size <= 0) {
            return true;
        }
        ++access_units;
        if (av_new_packet(packet, payload_size) < 0) {
            std::fprintf(stderr, "av_new_packet 失败\n");
            ++errors;
            return false;
        }
        std::memcpy(packet->data, payload, static_cast<std::size_t>(payload_size));

        const int sent = avcodec_send_packet(context, packet);
        av_packet_unref(packet);
        if (sent < 0 && sent != AVERROR(EAGAIN)) {
            ++errors;
            return true;
        }
        if (drain_frames(context, frame, &frames, &width, &height, &pix_fmt) < 0) {
            ++errors;
        }
        return true;
    };

    std::size_t offset = 0;
    while (offset < data.size()) {
        std::uint8_t* parsed = nullptr;
        int parsed_size = 0;
        const int consumed = av_parser_parse2(
            parser, context, &parsed, &parsed_size, data.data() + offset,
            static_cast<int>(data.size() - offset), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (consumed < 0) {
            std::fprintf(stderr, "码流解析失败，偏移 %zu\n", offset);
            ++errors;
            break;
        }
        offset += static_cast<std::size_t>(consumed);

        if (!feed(parsed, parsed_size)) {
            break;
        }

        if (consumed == 0 && parsed_size == 0) {
            break;
        }
    }

    // 解析器在 EOF 处会缓存最后一个 access unit，必须显式冲刷，
    // 否则统计会比实际少一帧。
    while (true) {
        std::uint8_t* parsed = nullptr;
        int parsed_size = 0;
        (void)av_parser_parse2(parser, context, &parsed, &parsed_size, nullptr, 0,
                               AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (parsed_size <= 0) {
            break;
        }
        if (!feed(parsed, parsed_size)) {
            break;
        }
    }

    // 冲刷解码器内部残留帧
    (void)avcodec_send_packet(context, nullptr);
    if (drain_frames(context, frame, &frames, &width, &height, &pix_fmt) < 0) {
        ++errors;
    }

    const char* pix_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(pix_fmt));
    std::printf("file=%s bytes=%zu access_units=%d frames=%d width=%d height=%d pix_fmt=%s errors=%d\n",
                path.c_str(), data.size(), access_units, frames, width, height,
                pix_name != nullptr ? pix_name : "unknown", errors);

    av_frame_free(&frame);
    av_packet_free(&packet);
    av_parser_close(parser);
    avcodec_free_context(&context);

    if (errors != 0 || frames == 0) {
        return 2;
    }
    return 0;
}
