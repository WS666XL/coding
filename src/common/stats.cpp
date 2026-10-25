#include "common/stats.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace lowlat {
namespace {

// 线性插值分位数。传入的样本单位是微秒，返回毫秒。
double percentile_ms(std::vector<std::int64_t> samples, double percent)
{
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const double rank = percent / 100.0 * static_cast<double>(samples.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(rank));
    const auto upper = static_cast<std::size_t>(std::ceil(rank));
    const double weight = rank - static_cast<double>(lower);
    const double lo = static_cast<double>(samples[lower]);
    const double hi = static_cast<double>(samples[upper]);
    return (lo + (hi - lo) * weight) / 1000.0;
}

double average_ms(const std::vector<std::int64_t>& samples)
{
    if (samples.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const std::int64_t value : samples) {
        sum += static_cast<double>(value);
    }
    return sum / static_cast<double>(samples.size()) / 1000.0;
}

void fill_latency_stats(StatsSnapshot& snap, const std::vector<std::int64_t>& samples)
{
    snap.latency_samples = samples.size();
    if (samples.empty()) {
        return;
    }
    snap.latency_avg_ms = average_ms(samples);
    snap.latency_p50_ms = percentile_ms(samples, 50.0);
    snap.latency_p99_ms = percentile_ms(samples, 99.0);
    snap.latency_max_ms =
        static_cast<double>(*std::max_element(samples.begin(), samples.end())) / 1000.0;
}

std::vector<std::int64_t> take_window_locked(std::vector<std::int64_t>& window,
                                             std::size_t count)
{
    std::vector<std::int64_t> samples;
    samples.reserve(count);
    samples.insert(samples.end(), window.begin(),
                   window.begin() + static_cast<std::ptrdiff_t>(count));
    return samples;
}

}  // namespace

std::string StatsSnapshot::to_string() const
{
    return spdlog::fmt_lib::format(
        "frames={} bytes={} pkts={} lost={} fec={} retx={} err={} "
        "lat(avg/p50/p99/max)={:.2f}/{:.2f}/{:.2f}/{:.2f}ms n={}",
        frames, bytes, packets, packets_lost, fec_recovered, retransmits, errors,
        latency_avg_ms, latency_p50_ms, latency_p99_ms, latency_max_ms,
        latency_samples);
}

Stats::Stats(std::size_t latency_window)
    : latency_capacity_(latency_window == 0 ? 1 : latency_window)
{
    latency_window_.assign(latency_capacity_, 0);
}

void Stats::add_frames(std::uint64_t n) noexcept
{
    frames_.fetch_add(n, std::memory_order_relaxed);
}

void Stats::add_bytes(std::uint64_t n) noexcept
{
    bytes_.fetch_add(n, std::memory_order_relaxed);
}

void Stats::add_packets(std::uint64_t n) noexcept
{
    packets_.fetch_add(n, std::memory_order_relaxed);
}

void Stats::add_packets_lost(std::uint64_t n) noexcept
{
    packets_lost_.fetch_add(n, std::memory_order_relaxed);
}

void Stats::add_fec_recovered(std::uint64_t n) noexcept
{
    fec_recovered_.fetch_add(n, std::memory_order_relaxed);
}

void Stats::add_retransmits(std::uint64_t n) noexcept
{
    retransmits_.fetch_add(n, std::memory_order_relaxed);
}

void Stats::add_errors(std::uint64_t n) noexcept
{
    errors_.fetch_add(n, std::memory_order_relaxed);
}

void Stats::record_latency_us(std::int64_t latency_us) noexcept
{
    if (latency_us < 0) {
        latency_us = 0;
    }
    std::lock_guard<std::mutex> lock(latency_mutex_);
    latency_window_[latency_cursor_] = latency_us;
    latency_cursor_ = (latency_cursor_ + 1) % latency_capacity_;
    if (latency_count_ < latency_capacity_) {
        ++latency_count_;
    }
}

StatsSnapshot Stats::snapshot_and_reset() noexcept
{
    StatsSnapshot snap;
    snap.frames = frames_.exchange(0, std::memory_order_relaxed);
    snap.bytes = bytes_.exchange(0, std::memory_order_relaxed);
    snap.packets = packets_.exchange(0, std::memory_order_relaxed);
    snap.packets_lost = packets_lost_.exchange(0, std::memory_order_relaxed);
    snap.fec_recovered = fec_recovered_.exchange(0, std::memory_order_relaxed);
    snap.retransmits = retransmits_.exchange(0, std::memory_order_relaxed);
    snap.errors = errors_.exchange(0, std::memory_order_relaxed);

    std::vector<std::int64_t> samples;
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        samples = take_window_locked(latency_window_, latency_count_);
        latency_cursor_ = 0;
        latency_count_ = 0;
    }
    fill_latency_stats(snap, samples);
    return snap;
}

StatsSnapshot Stats::peek() const noexcept
{
    StatsSnapshot snap;
    snap.frames = frames_.load(std::memory_order_relaxed);
    snap.bytes = bytes_.load(std::memory_order_relaxed);
    snap.packets = packets_.load(std::memory_order_relaxed);
    snap.packets_lost = packets_lost_.load(std::memory_order_relaxed);
    snap.fec_recovered = fec_recovered_.load(std::memory_order_relaxed);
    snap.retransmits = retransmits_.load(std::memory_order_relaxed);
    snap.errors = errors_.load(std::memory_order_relaxed);

    std::vector<std::int64_t> samples;
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        samples.reserve(latency_count_);
        samples.insert(samples.end(), latency_window_.begin(),
                       latency_window_.begin() + static_cast<std::ptrdiff_t>(latency_count_));
    }
    fill_latency_stats(snap, samples);
    return snap;
}

}  // namespace lowlat
