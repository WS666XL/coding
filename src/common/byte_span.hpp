#pragma once

#include <cstddef>
#include <cstdint>

namespace lowlat {

// C++17 下的轻量只读字节视图，替代 C++20 的 std::span<const std::uint8_t>。
// 刻意不引入任何 C++17 之后的标准库设施。
class ByteSpan {
public:
    constexpr ByteSpan() noexcept = default;

    constexpr ByteSpan(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size)
    {
    }

    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    const std::uint8_t* begin() const noexcept { return data_; }
    const std::uint8_t* end() const noexcept { return data_ + size_; }

    const std::uint8_t& operator[](std::size_t index) const noexcept { return data_[index]; }

    // 越界时返回空视图，不抛异常：网络路径上宁可丢包也不制造异常。
    ByteSpan first(std::size_t count) const noexcept
    {
        return count <= size_ ? ByteSpan(data_, count) : ByteSpan();
    }

    ByteSpan subspan(std::size_t offset, std::size_t count) const noexcept
    {
        if (offset > size_) {
            return ByteSpan();
        }
        const std::size_t remain = size_ - offset;
        return ByteSpan(data_ + offset, count <= remain ? count : remain);
    }

private:
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
};

}  // namespace lowlat
