#include "protocol/bandwidth_estimator.hpp"

namespace lowlat {

BandwidthEstimator::BandwidthEstimator() : BandwidthEstimator(Config{}) {}

BandwidthEstimator::BandwidthEstimator(Config config) : config_(config)
{
    // 配置自愈：任何一项写反都不该让估计器进入死循环
    if (config_.min_bps < 1000) {
        config_.min_bps = 1000;
    }
    if (config_.max_bps < config_.min_bps) {
        config_.max_bps = config_.min_bps;
    }
    if (config_.start_bps < config_.min_bps) {
        config_.start_bps = config_.min_bps;
    }
    if (config_.start_bps > config_.max_bps) {
        config_.start_bps = config_.max_bps;
    }
    if (config_.window_samples == 0) {
        config_.window_samples = 1;
    }
    if (config_.increase_factor < 1.0) {
        config_.increase_factor = 1.0;
    }
    if (config_.decrease_factor <= 0.0 || config_.decrease_factor > 1.0) {
        config_.decrease_factor = 0.85;
    }
    reset();
}

void BandwidthEstimator::reset() noexcept
{
    has_previous_ = false;
    previous_send_us_ = 0;
    previous_arrival_us_ = 0;
    accumulated_delay_us_ = 0;
    last_window_delay_us_ = 0;
    window_samples_ = 0;
    target_bps_ = config_.start_bps;
    state_ = State::Normal;
    last_decrease_us_ = 0;
    has_decreased_once_ = false;
    overuse_events_ = 0;
    underuse_events_ = 0;
    increase_events_ = 0;
    decrease_events_ = 0;
}

std::int64_t BandwidthEstimator::clamp_bps(double value) const noexcept
{
    if (value < static_cast<double>(config_.min_bps)) {
        return config_.min_bps;
    }
    if (value > static_cast<double>(config_.max_bps)) {
        return config_.max_bps;
    }
    return static_cast<std::int64_t>(value);
}

void BandwidthEstimator::on_sample(std::int64_t send_us, std::int64_t arrival_us,
                                   std::int64_t now_us) noexcept
{
    if (!has_previous_) {
        has_previous_ = true;
        previous_send_us_ = send_us;
        previous_arrival_us_ = arrival_us;
        return;
    }

    // 到达间隔减去发送间隔：正值代表队列在堆积
    const std::int64_t arrival_delta = arrival_us - previous_arrival_us_;
    const std::int64_t send_delta = send_us - previous_send_us_;

    previous_send_us_ = send_us;
    previous_arrival_us_ = arrival_us;

    accumulated_delay_us_ += (arrival_delta - send_delta);
    ++window_samples_;

    if (window_samples_ < config_.window_samples) {
        return;
    }
    evaluate_window(now_us);
}

void BandwidthEstimator::evaluate_window(std::int64_t now_us) noexcept
{
    last_window_delay_us_ = accumulated_delay_us_;

    if (accumulated_delay_us_ > config_.overuse_threshold_us) {
        state_ = State::Overuse;
        ++overuse_events_;
    } else if (accumulated_delay_us_ < -config_.underuse_threshold_us) {
        state_ = State::Underuse;
        ++underuse_events_;
    } else {
        state_ = State::Normal;
    }

    // 评估完清零：每个窗口只看自己这一段的变化，
    // 否则历史包袱会让估计对新的拥塞反应迟钝。
    accumulated_delay_us_ = 0;
    window_samples_ = 0;

    apply_rate(now_us);
}

void BandwidthEstimator::apply_rate(std::int64_t now_us) noexcept
{
    if (state_ == State::Overuse) {
        // 刚降过速就先等队列排空，避免大过冲后连续下调到底
        if (has_decreased_once_ && now_us - last_decrease_us_ < config_.decrease_cooldown_us) {
            return;
        }

        // 按过载严重程度分档降速。
        //
        // 只按固定系数缓降是不够的：实测 100kbit 瓶颈下，每窗口乘 0.85
        // 会让码率从 2000 只降到 1228 就结束了，而队列延迟已经堆到 656ms。
        // 延迟越大说明排队越久，必须一次让出更多带宽，否则收敛时间远超
        // 用户能忍受的范围。
        // 最多两步（一次最多砍 ~28%）。
        //
        // 早期版本按严重度分到 4 步（一次砍 48%），实测会过度收敛：
        // 1.2Mbit 链路上码率已经降到 1190（几乎正好），下一窗口仍判过载，
        // 一刀砍到 621，再砍到 324，最后贴到下限 300。
        // 窗口延迟大只说明队列长，不等于"链路容量只有这么点"，
        // 一次砍太多会远离真实工作点，而且恢复要靠 8% 的慢增。
        int steps = 1;
        if (config_.overuse_threshold_us > 0) {
            const double severity = static_cast<double>(last_window_delay_us_) /
                                    static_cast<double>(config_.overuse_threshold_us);
            if (severity >= 2.0) {
                steps = 2;
            }
        }

        double next = static_cast<double>(target_bps_);
        for (int i = 0; i < steps; ++i) {
            next *= config_.decrease_factor;
        }
        target_bps_ = clamp_bps(next);
        last_decrease_us_ = now_us;
        has_decreased_once_ = true;
        ++decrease_events_;
        return;
    }

    // 欠载不趁机涨：队列排空往往意味着对端刚限速，此时提速会立刻再撞墙
    if (state_ == State::Underuse) {
        return;
    }

    if (has_decreased_once_ && now_us - last_decrease_us_ < config_.cooldown_us) {
        return;
    }

    const std::int64_t next = clamp_bps(static_cast<double>(target_bps_) * config_.increase_factor);
    if (next != target_bps_) {
        target_bps_ = next;
        ++increase_events_;
    }
}

void BandwidthEstimator::on_loss_report(double loss_ratio, std::int64_t now_us) noexcept
{
    if (loss_ratio >= config_.loss_high) {
        target_bps_ = clamp_bps(static_cast<double>(target_bps_) * config_.decrease_factor);
        last_decrease_us_ = now_us;
        has_decreased_once_ = true;
        ++decrease_events_;
        return;
    }

    // 中等丢包：不降速但重置冷却，把"继续提速"往后推
    if (loss_ratio > config_.loss_low) {
        last_decrease_us_ = now_us;
        has_decreased_once_ = true;
    }
}

}  // namespace lowlat
