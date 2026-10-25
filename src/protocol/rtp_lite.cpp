#include "protocol/rtp_lite.hpp"

#include "common/byte_order.hpp"

namespace lowlat {

std::size_t serialize_rtp_lite(const RtpLiteHeader& header, std::uint8_t* out) noexcept
{
    if (out == nullptr) {
        return 0;
    }

    // V(2) | P(1,恒0) | X(1) | CC(4,恒0)
    out[0] = static_cast<std::uint8_t>(((header.version & 0x03u) << 6) |
                                       (header.has_frame_index ? 0x10u : 0x00u));
    // M(1) | PT(7)
    out[1] = static_cast<std::uint8_t>((header.marker ? 0x80u : 0x00u) |
                                       (header.payload_type & 0x7Fu));

    net::write_be16(out + 2, header.sequence);
    net::write_be32(out + 4, header.timestamp);
    net::write_be32(out + 8, header.ssrc);

    if (header.has_frame_index) {
        net::write_be32(out + 12, header.frame_index);
        net::write_be32(out + 16, header.capture_ts_us);
    }
    return header.serialized_size();
}

bool parse_rtp_lite(ByteSpan input, RtpLiteHeader* out) noexcept
{
    if (out == nullptr || input.size() < RtpLiteHeader::kBaseSize) {
        return false;
    }

    const std::uint8_t* p = input.data();
    const std::uint8_t version = static_cast<std::uint8_t>((p[0] >> 6) & 0x03u);
    if (version != RtpLiteHeader::kVersion) {
        return false;
    }

    RtpLiteHeader header;
    header.version = version;
    header.has_frame_index = (p[0] & 0x10u) != 0;
    header.marker = (p[1] & 0x80u) != 0;
    header.payload_type = static_cast<std::uint8_t>(p[1] & 0x7Fu);
    header.sequence = net::read_be16(p + 2);
    header.timestamp = net::read_be32(p + 4);
    header.ssrc = net::read_be32(p + 8);
    header.frame_index = 0;

    if (header.has_frame_index) {
        if (input.size() < RtpLiteHeader::kBaseSize + RtpLiteHeader::kExtensionSize) {
            return false;
        }
        header.frame_index = net::read_be32(p + 12);
        header.capture_ts_us = net::read_be32(p + 16);
    }

    *out = header;
    return true;
}

}  // namespace lowlat
