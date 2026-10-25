#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/byte_span.hpp"

namespace lowlat {

// H.264 解码器（libavcodec 封装）。
//
// 接口有意不暴露任何 FFmpeg 类型：上层（Qt 渲染层）只看到裸平面指针和
// 一个不透明的 owner，避免 FFmpeg 头文件污染 UI 代码。
//
// 输出帧带 stride（FFmpeg 的解码输出行宽通常大于图像宽度，为了 16/32 字节对齐），
// 调用方上传纹理时必须用 GL_UNPACK_ROW_LENGTH 之类的机制处理，不能假设紧凑布局。
class H264Decoder {
public:
    struct Frame {
        int width{0};
        int height{0};
        int stride_y{0};
        int stride_u{0};
        int stride_v{0};
        const std::uint8_t* y{nullptr};
        const std::uint8_t* u{nullptr};
        const std::uint8_t* v{nullptr};
        // 保活用：持有它，平面指针就一直有效。不要手动释放。
        std::shared_ptr<void> owner;
    };

    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    bool open(std::string* error);
    void close() noexcept;
    bool is_open() const noexcept;

    // 送入一个 NAL（不含起始码）。内部自动补起始码并交给解析器聚成 AU。
    bool feed_nal(ByteSpan nal, std::vector<Frame>* out, std::string* error);

    // 送入任意 Annex-B 字节流（含起始码，可包含多个 AU）。
    bool feed_annexb(ByteSpan data, std::vector<Frame>* out, std::string* error);

    // 冲刷解码器内部缓存（解码器通常会压一帧，不 flush 会少一帧）。
    bool flush(std::vector<Frame>* out, std::string* error);

    std::uint64_t decoded_frames() const noexcept;
    std::uint64_t send_errors() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lowlat
