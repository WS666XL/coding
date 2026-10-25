#include "protocol/nack.hpp"

#include <algorithm>

#include "common/byte_order.hpp"

namespace lowlat {

std::size_t serialize_nack(const NackRequest& request, std::uint8_t* out) noexcept
{
    if (out == nullptr) {
        return 0;
    }
    const std::size_t count = std::min(request.sequences.size(), NackRequest::kMaxSequences);
    net::write_be16(out, static_cast<std::uint16_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        net::write_be16(out + NackRequest::kHeaderBytes + 2 * i, request.sequences[i]);
    }
    return NackRequest::kHeaderBytes + 2 * count;
}

bool parse_nack(ByteSpan input, NackRequest* out) noexcept
{
    if (out == nullptr || input.size() < NackRequest::kHeaderBytes) {
        return false;
    }

    const std::size_t count = net::read_be16(input.data());
    if (count > NackRequest::kMaxSequences) {
        return false;
    }
    if (input.size() < NackRequest::kHeaderBytes + 2 * count) {
        return false;
    }

    out->sequences.clear();
    out->sequences.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out->sequences.push_back(net::read_be16(input.data() + NackRequest::kHeaderBytes + 2 * i));
    }
    return true;
}

}  // namespace lowlat
