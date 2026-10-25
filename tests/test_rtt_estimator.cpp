#include "protocol/rtt_estimator.hpp"

#include <gtest/gtest.h>

TEST(RttEstimator, HasNoEstimateInitially)
{
    lowlat::RttEstimator estimator;
    EXPECT_FALSE(estimator.has_estimate());
    EXPECT_EQ(estimator.samples(), 0u);
    EXPECT_DOUBLE_EQ(estimator.smoothed_us(), 0.0);
}

TEST(RttEstimator, FirstSampleSeedsWholeEstimate)
{
    lowlat::RttEstimator estimator;
    estimator.update(8000.0);

    EXPECT_TRUE(estimator.has_estimate());
    EXPECT_DOUBLE_EQ(estimator.smoothed_us(), 8000.0);
    EXPECT_DOUBLE_EQ(estimator.min_us(), 8000.0);
    EXPECT_EQ(estimator.samples(), 1u);
}

TEST(RttEstimator, IgnoresNonPositiveSamples)
{
    lowlat::RttEstimator estimator;
    estimator.update(0.0);
    estimator.update(-5.0);
    EXPECT_FALSE(estimator.has_estimate());
    EXPECT_EQ(estimator.samples(), 0u);
}

TEST(RttEstimator, SmoothsSuddenChangeByOneEighth)
{
    lowlat::RttEstimator estimator;
    estimator.update(10000.0);
    estimator.update(20000.0);  // srtt = 10000 + (20000-10000)/8 = 11250

    EXPECT_DOUBLE_EQ(estimator.smoothed_us(), 11250.0);
}

TEST(RttEstimator, ConvergesTowardsSteadyValue)
{
    lowlat::RttEstimator estimator;
    estimator.update(10000.0);
    for (int i = 0; i < 50; ++i) {
        estimator.update(5000.0);
    }

    // (7/8)^50 ≈ 0.00124，50 个样本后应基本收敛到 5000
    EXPECT_NEAR(estimator.smoothed_us(), 5000.0, 20.0);
}

TEST(RttEstimator, TracksMinimumSeparatelyFromSmoothing)
{
    lowlat::RttEstimator estimator;
    estimator.update(5000.0);
    estimator.update(3000.0);
    estimator.update(8000.0);

    EXPECT_DOUBLE_EQ(estimator.min_us(), 3000.0);
    // 平滑值应远大于最小值，体现两者职责不同
    EXPECT_GT(estimator.smoothed_us(), 4000.0);
}

TEST(RttEstimator, SingleLowOutlierDoesNotCollapseEstimate)
{
    lowlat::RttEstimator estimator;
    for (int i = 0; i < 20; ++i) {
        estimator.update(30000.0);
    }
    ASSERT_NEAR(estimator.smoothed_us(), 30000.0, 1.0);

    // 单个 0.1ms 的偶然样本（贴近发送端轮询点）只应把估计拉低一点点，
    // 而不是像 min RTT 那样直接塌到 0.1ms。
    estimator.update(100.0);
    EXPECT_GT(estimator.smoothed_us(), 25000.0);
    EXPECT_DOUBLE_EQ(estimator.min_us(), 100.0);
}

TEST(RttEstimator, ResetClearsState)
{
    lowlat::RttEstimator estimator;
    estimator.update(1000.0);
    estimator.update(2000.0);
    ASSERT_TRUE(estimator.has_estimate());

    estimator.reset();
    EXPECT_FALSE(estimator.has_estimate());
    EXPECT_EQ(estimator.samples(), 0u);
    EXPECT_DOUBLE_EQ(estimator.smoothed_us(), 0.0);
}
