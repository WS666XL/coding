#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace lowlat {

// 一帧 I420（YUV420P）图像。
//
// 内存布局：Y 平面 w*h 字节，随后 U、V 平面各 (w/2)*(h/2) 字节，
// 行间无 padding（stride == 平面宽度）。
//
// 缓冲来自 FramePool，最后一个引用释放时自动归还，因此帧对象可以安全地
// 跨线程传递（采集 -> 编码 -> 发送）。
struct YuvFrame {
    YuvFrame() noexcept = default;

    YuvFrame(std::uint32_t index, std::int64_t ts_us, int w, int h,
             std::shared_ptr<std::uint8_t> buffer) noexcept
        : frame_index(index),
          capture_ts_us(ts_us),
          width(w),
          height(h),
          data(std::move(buffer))
    {
    }

    // C++17：不使用指定初始化器。
    std::uint32_t frame_index{0};
    std::int64_t capture_ts_us{0};  // steady_clock 起点，端到端时延的唯一基准
    int width{0};
    int height{0};
    std::shared_ptr<std::uint8_t> data;

    bool valid() const noexcept
    {
        return data != nullptr && width > 0 && height > 0 &&
               (width % 2 == 0) && (height % 2 == 0);
    }

    std::size_t y_size() const noexcept
    {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    std::size_t chroma_size() const noexcept
    {
        return static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2);
    }

    std::size_t byte_size() const noexcept { return y_size() + 2 * chroma_size(); }

    std::uint8_t* y_plane() const noexcept { return data.get(); }
    std::uint8_t* u_plane() const noexcept { return data.get() + y_size(); }
    std::uint8_t* v_plane() const noexcept { return data.get() + y_size() + chroma_size(); }
};

}  // namespace lowlat
