#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace lowlat {

// 一次统计窗口的快照。snapshot_and_reset() 返回的是"自上次调用以来的增量"。
struct StatsSnapshot {
    std::uint64_t frames{0};
    std::uint64_t bytes{0};
    std::uint64_t packets{0};
    std::uint64_t packets_lost{0};
    std::uint64_t fec_recovered{0};
    std::uint64_t retransmits{0};
    std::uint64_t errors{0};

    std::size_t latency_samples{0};
    double latency_avg_ms{0.0};
    double latency_p50_ms{0.0};
    double latency_p99_ms{0.0};
    double latency_max_ms{0.0};

    std::string to_string() const;
};

// 计数器全部为无锁原子变量；只有时延采样走互斥量（每秒百次量级，非热路径）。
class Stats {
public:
    explicit Stats(std::size_t latency_window = 4096);

    void add_frames(std::uint64_t n = 1) noexcept;
    void add_bytes(std::uint64_t n = 1) noexcept;
    void add_packets(std::uint64_t n = 1) noexcept;
    void add_packets_lost(std::uint64_t n = 1) noexcept;
    void add_fec_recovered(std::uint64_t n = 1) noexcept;
    void add_retransmits(std::uint64_t n = 1) noexcept;
    void add_errors(std::uint64_t n = 1) noexcept;

    // latency_us < 0 会被截断为 0，避免 clock 混用导致负数污染分位数。
    void record_latency_us(std::int64_t latency_us) noexcept;

    // 取出增量并清空时延窗口。建议由单一聚合线程每秒调用一次。
    StatsSnapshot snapshot_and_reset() noexcept;

    // 只读快照，不清空。
    StatsSnapshot peek() const noexcept;

private:
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> packets_{0};
    std::atomic<std::uint64_t> packets_lost_{0};
    std::atomic<std::uint64_t> fec_recovered_{0};
    std::atomic<std::uint64_t> retransmits_{0};
    std::atomic<std::uint64_t> errors_{0};

    mutable std::mutex latency_mutex_;
    std::vector<std::int64_t> latency_window_;
    std::size_t latency_cursor_{0};
    std::size_t latency_count_{0};
    std::size_t latency_capacity_;
};

}  // namespace lowlat
