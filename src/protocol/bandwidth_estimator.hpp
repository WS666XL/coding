#pragma once

#include <cstddef>
#include <cstdint>

namespace lowlat {

// 简化版 GCC 带宽估计 + AIMD 码率控制。
//
// 核心信号是"排队延迟梯度"：如果数据包到达间隔比发送间隔更长，说明链路里
// 正在堆积队列，也就是我们发得太快。相比只看丢包，延迟梯度能在丢包发生前
// 就发现拥塞，这是 GCC 相对传统 AIMD 的关键优势。
//
//   单窗延迟变化 = Σ [ (到达_i - 到达_{i-1}) - (发送_i - 发送_{i-1}) ]
//
// 一个窗口累积下来：
//   > +阈值  → 过载（队列在涨）→ 乘性降速
//   < -阈值  → 欠载（队列在排空）→ 保持不变，不趁机涨
//   否则     → 正常 → 加性提速（受冷却时间约束）
//
// 丢包作为第二信号：超过高水位直接降速，中等丢包只暂停提速。
// 只用延迟会漏掉"缓冲区极小的链路"（还没堆积就已经丢包），只用丢包又太晚，
// 所以两者都要。
//
// 本类不做任何 IO，输入是时序样本与丢包率，输出是目标码率，
// 因此可以完全用单元测试覆盖，不依赖网络环境。
class BandwidthEstimator {
public:
    enum class State {
        Normal,
        Overuse,
        Underuse,
    };

    struct Config {
        std::int64_t min_bps{300000};
        std::int64_t max_bps{4000000};
        std::int64_t start_bps{1000000};

        double increase_factor{1.08};
        double decrease_factor{0.85};

        // 降温时间：降速后多久内不许提速，避免抖动时反复试探
        std::int64_t cooldown_us{500000};

        // 连续降速的最小间隔。
        //
        // 没有这个间隔时，一旦发生大过冲（比如 2000kbps 打到 1000kbit 链路上），
        // 队列会堆积到几百毫秒，随后的好几个窗口都在"排空"过程中，检测器仍看到
        // 正值延迟就继续下调，最终一路降到底。实测 1Mbit 链路会被压到 200kbps 下限。
        // 给队列留出排空时间，才能停在正确的水平。
        std::int64_t decrease_cooldown_us{250000};

        // 步长：每积累这么多个样本评估一次
        std::size_t window_samples{20};

        // 判定阈值
        std::int64_t overuse_threshold_us{15000};
        std::int64_t underuse_threshold_us{15000};

        // 丢包水位
        double loss_high{0.10};
        double loss_low{0.02};
    };

    BandwidthEstimator();
    explicit BandwidthEstimator(Config config);

    void reset() noexcept;

    // 一个时序样本：send_us 是发送端时刻，arrival_us 是接收端时刻。
    // 两者必须来自可比的单调时钟（本项目两端同主机，都用 steady_clock）。
    // now_us 用于冷却判定，通常就是 arrival_us。
    void on_sample(std::int64_t send_us, std::int64_t arrival_us, std::int64_t now_us) noexcept;

    // 接收端上报的窗口丢包率（0.0 ~ 1.0）
    void on_loss_report(double loss_ratio, std::int64_t now_us) noexcept;

    std::int64_t target_bps() const noexcept { return target_bps_; }
    State state() const noexcept { return state_; }
    std::int64_t last_window_delay_us() const noexcept { return last_window_delay_us_; }

    std::uint64_t overuse_events() const noexcept { return overuse_events_; }
    std::uint64_t underuse_events() const noexcept { return underuse_events_; }
    std::uint64_t increase_events() const noexcept { return increase_events_; }
    std::uint64_t decrease_events() const noexcept { return decrease_events_; }

    const Config& config() const noexcept { return config_; }

private:
    void evaluate_window(std::int64_t now_us) noexcept;
    void apply_rate(std::int64_t now_us) noexcept;
    std::int64_t clamp_bps(double value) const noexcept;

    Config config_{};

    bool has_previous_{false};
    std::int64_t previous_send_us_{0};
    std::int64_t previous_arrival_us_{0};

    std::int64_t accumulated_delay_us_{0};
    std::int64_t last_window_delay_us_{0};
    std::size_t window_samples_{0};

    std::int64_t target_bps_{0};
    State state_{State::Normal};

    std::int64_t last_decrease_us_{0};
    bool has_decreased_once_{false};

    std::uint64_t overuse_events_{0};
    std::uint64_t underuse_events_{0};
    std::uint64_t increase_events_{0};
    std::uint64_t decrease_events_{0};
};

}  // namespace lowlat
