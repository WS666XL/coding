#include "protocol/fec_xor.hpp"

#include <algorithm>

#include "common/byte_order.hpp"

namespace lowlat {
namespace {

constexpr std::size_t kHeaderFixedBytes = 4;
constexpr std::size_t kMaxGroupSize = 64;

std::string invalid(const std::string& text)
{
    return text;
}

}  // namespace

XorFecEncoder::XorFecEncoder() : XorFecEncoder(Config{}) {}

XorFecEncoder::XorFecEncoder(Config config) : config_(config)
{
    if (config_.group_size == 0) {
        config_.group_size = 1;
    }
    if (config_.group_size > kMaxGroupSize) {
        config_.group_size = kMaxGroupSize;
    }
    if (config_.max_parity_bytes < kHeaderFixedBytes + 2) {
        config_.max_parity_bytes = kHeaderFixedBytes + 2;
    }
    reset();
}

void XorFecEncoder::reset() noexcept
{
    lengths_.clear();
    parity_.clear();
    base_sequence_ = 0;
}

bool XorFecEncoder::add(std::uint16_t sequence, ByteSpan packet,
                        std::vector<std::uint8_t>* repair, bool* produced, std::string* error)
{
    if (repair == nullptr || produced == nullptr) {
        if (error != nullptr) {
            *error = invalid("XorFecEncoder::add 输出指针为空");
        }
        return false;
    }
    *produced = false;

    if (packet.empty()) {
        if (error != nullptr) {
            *error = invalid("XorFecEncoder::add 收到空包");
        }
        return false;
    }
    if (packet.size() > 0xFFFFu) {
        if (error != nullptr) {
            *error = invalid("数据包超过 65535 字节，长度表无法表示");
        }
        return false;
    }

    if (lengths_.empty()) {
        base_sequence_ = sequence;
        parity_.assign(packet.size(), 0);
    } else if (packet.size() > parity_.size()) {
        // 后到的包更长：扩展异或缓冲，新增字节初始为 0，等价于给短包补零
        parity_.resize(packet.size(), 0);
    }

    for (std::size_t i = 0; i < packet.size(); ++i) {
        parity_[i] = static_cast<std::uint8_t>(parity_[i] ^ packet.data()[i]);
    }
    lengths_.push_back(static_cast<std::uint16_t>(packet.size()));

    if (lengths_.size() < config_.group_size) {
        return true;
    }

    const std::size_t header_bytes = kHeaderFixedBytes + 2 * lengths_.size();
    if (header_bytes + parity_.size() > config_.max_parity_bytes) {
        if (error != nullptr) {
            *error = invalid("修复包 " + std::to_string(header_bytes + parity_.size()) +
                             " 字节超出上限 " + std::to_string(config_.max_parity_bytes) +
                             "，请减小 group_size 或数据包尺寸");
        }
        // 该组作废，避免下一次 add 把它和下一组的包混在一起
        reset();
        return false;
    }

    repair->assign(header_bytes + parity_.size(), 0);
    net::write_be16(repair->data(), base_sequence_);
    (*repair)[2] = static_cast<std::uint8_t>(lengths_.size());
    (*repair)[3] = 0;
    for (std::size_t i = 0; i < lengths_.size(); ++i) {
        net::write_be16(repair->data() + kHeaderFixedBytes + 2 * i, lengths_[i]);
    }
    std::copy(parity_.begin(), parity_.end(),
              repair->begin() + static_cast<std::ptrdiff_t>(header_bytes));

    ++groups_emitted_;
    *produced = true;
    reset();
    return true;
}

bool XorFecEncoder::flush(std::vector<std::uint8_t>* repair, bool* produced, std::string* error)
{
    if (repair == nullptr || produced == nullptr) {
        if (error != nullptr) {
            *error = invalid("XorFecEncoder::flush 输出指针为空");
        }
        return false;
    }
    *produced = false;

    if (lengths_.size() < 2) {
        reset();
        return true;
    }

    const std::size_t header_bytes = kHeaderFixedBytes + 2 * lengths_.size();
    if (header_bytes + parity_.size() > config_.max_parity_bytes) {
        if (error != nullptr) {
            *error = invalid("修复包超出上限，flush 放弃该组");
        }
        reset();
        return false;
    }

    repair->assign(header_bytes + parity_.size(), 0);
    net::write_be16(repair->data(), base_sequence_);
    (*repair)[2] = static_cast<std::uint8_t>(lengths_.size());
    (*repair)[3] = 0;
    for (std::size_t i = 0; i < lengths_.size(); ++i) {
        net::write_be16(repair->data() + kHeaderFixedBytes + 2 * i, lengths_[i]);
    }
    std::copy(parity_.begin(), parity_.end(),
              repair->begin() + static_cast<std::ptrdiff_t>(header_bytes));

    ++groups_emitted_;
    *produced = true;
    reset();
    return true;
}

XorFecDecoder::XorFecDecoder(std::size_t window)
{
    if (window == 0) {
        window = 1;
    }
    slots_.resize(window);
    reset();
}

void XorFecDecoder::reset() noexcept
{
    for (auto& slot : slots_) {
        slot.valid = false;
        slot.sequence = 0;
        slot.data.clear();
    }
    stats_ = Stats{};
}

void XorFecDecoder::push_data(std::uint16_t sequence, ByteSpan packet)
{
    if (slots_.empty() || packet.empty()) {
        return;
    }
    Slot& slot = slots_[static_cast<std::size_t>(sequence) % slots_.size()];
    slot.valid = true;
    slot.sequence = sequence;
    slot.data.assign(packet.begin(), packet.end());
}

bool XorFecDecoder::contains(std::uint16_t sequence) const noexcept
{
    const std::vector<std::uint8_t>* unused = nullptr;
    return lookup(sequence, &unused);
}

bool XorFecDecoder::lookup(std::uint16_t sequence,
                           const std::vector<std::uint8_t>** out) const noexcept
{
    if (slots_.empty() || out == nullptr) {
        return false;
    }
    const Slot& slot = slots_[static_cast<std::size_t>(sequence) % slots_.size()];
    if (!slot.valid || slot.sequence != sequence) {
        return false;
    }
    *out = &slot.data;
    return true;
}

bool XorFecDecoder::recover_with(ByteSpan repair, std::vector<std::uint8_t>* recovered,
                                 bool* ok, std::string* error)
{
    if (recovered == nullptr || ok == nullptr) {
        if (error != nullptr) {
            *error = invalid("XorFecDecoder::recover_with 输出指针为空");
        }
        return false;
    }
    *ok = false;

    if (repair.size() < kHeaderFixedBytes) {
        if (error != nullptr) {
            *error = invalid("修复包不足 4 字节");
        }
        return false;
    }

    const std::uint16_t base = net::read_be16(repair.data());
    const std::size_t group_size = repair[2];
    if (group_size == 0 || group_size > kMaxGroupSize) {
        ++stats_.repairs_unusable;
        return true;
    }

    const std::size_t header_bytes = kHeaderFixedBytes + 2 * group_size;
    if (repair.size() < header_bytes) {
        ++stats_.repairs_unusable;
        return true;
    }

    // 统计组内缺失情况：必须恰好缺 1 个才能用异或还原
    std::size_t missing_index = group_size;
    std::size_t missing_count = 0;
    std::size_t max_length = 0;
    for (std::size_t i = 0; i < group_size; ++i) {
        const std::uint16_t length =
            net::read_be16(repair.data() + kHeaderFixedBytes + 2 * i);
        max_length = std::max(max_length, static_cast<std::size_t>(length));

        const std::vector<std::uint8_t>* data = nullptr;
        const std::uint16_t sequence = static_cast<std::uint16_t>(base + i);
        if (!lookup(sequence, &data)) {
            ++missing_count;
            missing_index = i;
        }
    }

    if (missing_count != 1) {
        ++stats_.repairs_unusable;
        return true;
    }

    const std::size_t parity_length = repair.size() - header_bytes;
    if (parity_length < max_length) {
        ++stats_.repairs_unusable;
        return true;
    }

    std::vector<std::uint8_t> result(parity_length);
    std::copy(repair.begin() + static_cast<std::ptrdiff_t>(header_bytes), repair.end(),
              result.begin());

    for (std::size_t i = 0; i < group_size; ++i) {
        if (i == missing_index) {
            continue;
        }
        const std::vector<std::uint8_t>* data = nullptr;
        if (!lookup(static_cast<std::uint16_t>(base + i), &data)) {
            // missing_count 已经保证只有一个缺失，这里不应发生
            ++stats_.repairs_unusable;
            return true;
        }
        for (std::size_t b = 0; b < data->size(); ++b) {
            result[b] = static_cast<std::uint8_t>(result[b] ^ (*data)[b]);
        }
    }

    const std::uint16_t missing_length =
        net::read_be16(repair.data() + kHeaderFixedBytes + 2 * missing_index);
    if (missing_length > result.size()) {
        ++stats_.repairs_unusable;
        return true;
    }
    result.resize(missing_length);

    *recovered = std::move(result);
    *ok = true;
    ++stats_.repairs_used;
    ++stats_.recovered_packets;
    return true;
}

}  // namespace lowlat
