#include "protocol/control_message.hpp"

#include <algorithm>

#include "common/byte_order.hpp"

namespace lowlat {
namespace {

constexpr std::size_t kNackTypeBytes = 1;
constexpr std::size_t kFeedbackHeaderBytes = 4;  // type + count + loss_permille

}  // namespace

bool peek_control_type(ByteSpan input, ControlType* type) noexcept
{
    if (type == nullptr || input.empty()) {
        return false;
    }
    switch (input.data()[0]) {
        case static_cast<std::uint8_t>(ControlType::Nack):
            *type = ControlType::Nack;
            return true;
        case static_cast<std::uint8_t>(ControlType::Feedback):
            *type = ControlType::Feedback;
            return true;
        default:
            return false;
    }
}

std::size_t serialize_nack_control(const NackRequest& request, std::uint8_t* out) noexcept
{
    if (out == nullptr) {
        return 0;
    }
    out[0] = static_cast<std::uint8_t>(ControlType::Nack);
    const std::size_t body = serialize_nack(request, out + kNackTypeBytes);
    return kNackTypeBytes + body;
}

bool parse_nack_control(ByteSpan input, NackRequest* out) noexcept
{
    if (out == nullptr || input.size() <= kNackTypeBytes) {
        return false;
    }
    ControlType type{};
    if (!peek_control_type(input, &type) || type != ControlType::Nack) {
        return false;
    }
    return parse_nack(input.subspan(kNackTypeBytes, input.size() - kNackTypeBytes), out);
}

std::size_t serialize_feedback(const FeedbackReport& report, std::uint8_t* out) noexcept
{
    if (out == nullptr) {
        return 0;
    }
    const std::size_t count = std::min(report.samples.size(), FeedbackReport::kMaxSamples);

    out[0] = static_cast<std::uint8_t>(ControlType::Feedback);
    out[1] = static_cast<std::uint8_t>(count);
    net::write_be16(out + 2, report.loss_permille);

    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t* slot = out + kFeedbackHeaderBytes + 8 * i;
        net::write_be32(slot, report.samples[i].capture_us);
        net::write_be32(slot + 4, report.samples[i].arrival_us);
    }
    return kFeedbackHeaderBytes + 8 * count;
}

bool parse_feedback(ByteSpan input, FeedbackReport* out) noexcept
{
    if (out == nullptr || input.size() < kFeedbackHeaderBytes) {
        return false;
    }
    ControlType type{};
    if (!peek_control_type(input, &type) || type != ControlType::Feedback) {
        return false;
    }

    const std::size_t count = input.data()[1];
    if (count > FeedbackReport::kMaxSamples) {
        return false;
    }
    if (input.size() < kFeedbackHeaderBytes + 8 * count) {
        return false;
    }

    out->loss_permille = net::read_be16(input.data() + 2);
    out->samples.clear();
    out->samples.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* slot = input.data() + kFeedbackHeaderBytes + 8 * i;
        DelaySample sample;
        sample.capture_us = net::read_be32(slot);
        sample.arrival_us = net::read_be32(slot + 4);
        out->samples.push_back(sample);
    }
    return true;
}

}  // namespace lowlat
