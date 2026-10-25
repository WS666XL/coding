#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lowlat {

// NACK 发送时机控制。
//
// 为什么不能"一发现缺口就立刻请求"：网络抖动与乱序让包可能只是"晚到"而不是
// "丢失"。立刻发 NACK 会把乱序误判成丢包，既浪费反向带宽，又制造额外的重传
// 流量，进一步加剧乱序——这是个正反馈。
//
// 上一轮实验就踩到了这个坑：netem 引入 20ms±20ms 抖动后，600 帧触发了 866 次
// NACK 请求、收到 4146 个"重传包"，而其中绝大多数只是乱序的正常包。
//
// 标准做法是等一个重排序窗口（通常几毫秒）再决定是否真的请求。
class NackScheduler {
public:
    struct Entry {
        std::uint16_t sequence{0};
        std::int64_t first_seen_us{0};
    };

    explicit NackScheduler(int delay_ms = 8, std::size_t max_pending = 64)
        : delay_ms_(delay_ms < 0 ? 0 : delay_ms),
          max_pending_(max_pending == 0 ? 1 : max_pending)
    {
    }

    // 登记一个缺失序列号。重复登记不会重置首次发现时刻，
    // 否则持续缺口的包会被无限推迟。
    void note_missing(std::uint16_t sequence, std::int64_t now_us)
    {
        for (const Entry& entry : entries_) {
            if (entry.sequence == sequence) {
                return;
            }
        }
        if (entries_.size() >= max_pending_) {
            entries_.erase(entries_.begin());  // 丢弃最旧的，避免无界增长
        }
        Entry fresh;
        fresh.sequence = sequence;
        fresh.first_seen_us = now_us;
        entries_.push_back(fresh);
    }

    // 取出已超过延迟窗口、且仍然缺失的序列号。
    // still_missing(seq) 由调用方提供，用来剔除"其实已经晚到"的包。
    //
    // 到期的条目无论最终是否请求，都会出队：留着只会让下一轮重复询问同一个包。
    template <typename Predicate>
    std::vector<std::uint16_t> collect(std::int64_t now_us, Predicate still_missing)
    {
        std::vector<std::uint16_t> ready;
        if (entries_.empty()) {
            return ready;
        }

        const std::int64_t window_us = static_cast<std::int64_t>(delay_ms_) * 1000;

        std::size_t kept = 0;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const Entry entry = entries_[i];
            if (now_us - entry.first_seen_us < window_us) {
                entries_[kept++] = entry;  // 窗口未到，继续等
                continue;
            }
            if (still_missing(entry.sequence)) {
                ready.push_back(entry.sequence);
            }
        }
        entries_.resize(kept);
        return ready;
    }

    std::size_t pending() const noexcept { return entries_.size(); }
    int delay_ms() const noexcept { return delay_ms_; }
    void reset() noexcept { entries_.clear(); }

private:
    std::vector<Entry> entries_;
    int delay_ms_{8};
    std::size_t max_pending_{64};
};

}  // namespace lowlat
