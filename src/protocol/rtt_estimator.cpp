#include "protocol/rtt_estimator.hpp"

namespace lowlat {
namespace {

constexpr double kSmoothingFactor = 8.0;

}  // namespace

void RttEstimator::reset() noexcept
{
    srtt_us_ = 0.0;
    min_us_ = 0.0;
    samples_ = 0;
}

void RttEstimator::update(double sample_us) noexcept
{
    if (sample_us <= 0.0) {
        return;
    }

    if (samples_ == 0) {
        srtt_us_ = sample_us;
        min_us_ = sample_us;
        ++samples_;
        return;
    }

    srtt_us_ += (sample_us - srtt_us_) / kSmoothingFactor;
    if (sample_us < min_us_) {
        min_us_ = sample_us;
    }
    ++samples_;
}

}  // namespace lowlat
