// V4L2 采集的测试。
//
// 注意：本机是 WSL2，没有 /dev/video*，因此这里只覆盖参数校验、错误路径和工厂
// 逻辑；真正的取帧路径（REQBUFS/QBUF/DQBUF/mmap/零拷贝回收）依赖真实设备，
// 属于未验证范围，必须在有 v4l2loopback 或摄像头的 Linux 上另行验收。

#include "capture/capture_factory.hpp"
#include "capture/v4l2_source.hpp"

#include <gtest/gtest.h>

#include <string>

TEST(V4l2Source, FailsOnMissingDevice)
{
    lowlat::V4l2Source source("/dev/lowlat-no-such-video", 640, 480, 30);

    std::string error;
    EXPECT_FALSE(source.start(&error));
    EXPECT_FALSE(error.empty());

    // 未启动时返回请求参数，启动成功后才返回驱动协商值
    EXPECT_EQ(source.width(), 640);
    EXPECT_EQ(source.height(), 480);
    EXPECT_EQ(source.fps(), 30);
    EXPECT_STREQ(source.name(), "v4l2");
    EXPECT_EQ(source.buffer_count(), 4u);
    EXPECT_EQ(source.requeue_failures(), 0u);
}

TEST(V4l2Source, RejectsOddDimensions)
{
    lowlat::V4l2Source source("/dev/video0", 641, 480, 30);

    std::string error;
    EXPECT_FALSE(source.start(&error));
    EXPECT_NE(error.find("偶数"), std::string::npos);
}

TEST(V4l2Source, RejectsNonPositiveFps)
{
    lowlat::V4l2Source source("/dev/video0", 640, 480, 0);

    std::string error;
    EXPECT_FALSE(source.start(&error));
    EXPECT_FALSE(error.empty());
}

TEST(V4l2Source, NextFrameFailsWhenNotStarted)
{
    lowlat::V4l2Source source("/dev/video0", 640, 480, 30);

    std::string error;
    lowlat::YuvFrame frame;
    EXPECT_FALSE(source.next_frame(&frame, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(frame.valid());
}

TEST(V4l2Source, StopIsIdempotentBeforeStart)
{
    lowlat::V4l2Source source("/dev/video0", 640, 480, 30);
    source.stop();
    source.stop();
    SUCCEED();
}

TEST(CaptureFactory, UnknownSourceReturnsNull)
{
    lowlat::CaptureOptions options;
    options.source = "rtsp";

    std::string error;
    auto source = lowlat::create_capture_source(options, &error);
    EXPECT_EQ(source, nullptr);
    EXPECT_NE(error.find("rtsp"), std::string::npos);
}

TEST(CaptureFactory, CreatesSyntheticSource)
{
    lowlat::CaptureOptions options;
    options.source = "synthetic";
    options.width = 64;
    options.height = 48;
    options.realtime = false;

    std::string error;
    auto source = lowlat::create_capture_source(options, &error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_STREQ(source->name(), "synthetic");
    EXPECT_EQ(source->width(), 64);

    ASSERT_TRUE(source->start(&error)) << error;
    lowlat::YuvFrame frame;
    ASSERT_TRUE(source->next_frame(&frame, &error)) << error;
    EXPECT_EQ(frame.width, 64);
    EXPECT_TRUE(frame.valid());
    source->stop();
}

TEST(CaptureFactory, CreatesV4l2Source)
{
    lowlat::CaptureOptions options;
    options.source = "v4l2";
    options.device = "/dev/lowlat-no-such-video";

    std::string error;
    auto source = lowlat::create_capture_source(options, &error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_STREQ(source->name(), "v4l2");
    EXPECT_FALSE(source->start(&error));
    EXPECT_FALSE(error.empty());
}

TEST(CaptureFactory, RejectsSyntheticOddDimensionsAtStart)
{
    lowlat::CaptureOptions options;
    options.source = "synthetic";
    options.width = 65;
    options.height = 48;

    std::string error;
    auto source = lowlat::create_capture_source(options, &error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_FALSE(source->start(&error));
    EXPECT_FALSE(error.empty());
}
