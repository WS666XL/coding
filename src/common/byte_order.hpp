#pragma once

#include <cstdint>
#include <cstring>

namespace lowlat {
namespace net {

// C++17 没有 std::endian，用编译器内建宏判断主机字节序。
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
constexpr bool kHostIsBigEndian = true;
#else
constexpr bool kHostIsBigEndian = false;
#endif

namespace detail {

inline std::uint16_t swap16(std::uint16_t v) noexcept { return __builtin_bswap16(v); }
inline std::uint32_t swap32(std::uint32_t v) noexcept { return __builtin_bswap32(v); }
inline std::uint64_t swap64(std::uint64_t v) noexcept { return __builtin_bswap64(v); }

}  // namespace detail

// 一律通过 memcpy 访问，避免未对齐指针解引用带来的 UB。
// 这些函数允许 p 未对齐（例如协议头后紧跟的负载）。

inline std::uint16_t read_be16(const std::uint8_t* p) noexcept
{
    std::uint16_t raw = 0;
    std::memcpy(&raw, p, sizeof(raw));
    return kHostIsBigEndian ? raw : detail::swap16(raw);
}

inline std::uint32_t read_be32(const std::uint8_t* p) noexcept
{
    std::uint32_t raw = 0;
    std::memcpy(&raw, p, sizeof(raw));
    return kHostIsBigEndian ? raw : detail::swap32(raw);
}

inline std::uint64_t read_be64(const std::uint8_t* p) noexcept
{
    std::uint64_t raw = 0;
    std::memcpy(&raw, p, sizeof(raw));
    return kHostIsBigEndian ? raw : detail::swap64(raw);
}

inline void write_be16(std::uint8_t* p, std::uint16_t v) noexcept
{
    const std::uint16_t raw = kHostIsBigEndian ? v : detail::swap16(v);
    std::memcpy(p, &raw, sizeof(raw));
}

inline void write_be32(std::uint8_t* p, std::uint32_t v) noexcept
{
    const std::uint32_t raw = kHostIsBigEndian ? v : detail::swap32(v);
    std::memcpy(p, &raw, sizeof(raw));
}

inline void write_be64(std::uint8_t* p, std::uint64_t v) noexcept
{
    const std::uint64_t raw = kHostIsBigEndian ? v : detail::swap64(v);
    std::memcpy(p, &raw, sizeof(raw));
}

}  // namespace net
}  // namespace lowlat
