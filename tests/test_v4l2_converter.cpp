// V4L2 像素格式转换器的测试。
//
// 背景：本机是 WSL2，摄像头只能通过 usbipd 透传且极不稳定（实测一次
// kill -9 就把 USB/IP 连接打成 ECONNRESET，之后再也取不到帧）。
// 因此无法用真实设备做回归，只能把"最容易写错的那部分"——色彩格式换算——
// 抽成纯函数逐字节验证。
//
// YUYV -> I420 的色度下采样是典型易错点：只取奇数行会让色度整体偏移半像素，
// 而这种错误肉眼几乎看不出来，只能靠精确断言。

#include "capture/v4l2_converter.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

extern "C" {
#include <linux/videodev2.h>
}

TEST(V4l2Converter, RejectsOddDimensions)
{
    lowlat::V4l2Converter converter;
    std::string error;
    // I420 的色度平面是宽高各半，奇数尺寸无法整除
    EXPECT_FALSE(converter.configure(V4L2_PIX_FMT_YUYV, 5, 4, 10, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(converter.configure(V4L2_PIX_FMT_YUYV, 4, 5, 8, &error));
}

TEST(V4l2Converter, RejectsUnknownFormat)
{
    lowlat::V4l2Converter converter;
    std::string error;
    EXPECT_FALSE(converter.configure(0xDEADBEEF, 4, 4, 4, &error));
    EXPECT_FALSE(error.empty());
}

TEST(V4l2Converter, Yuv420IsPassthroughWhenStrideMatchesWidth)
{
    lowlat::V4l2Converter converter;
    std::string error;
    ASSERT_TRUE(converter.configure(V4L2_PIX_FMT_YUV420, 4, 4, 4, &error)) << error;

    // 紧凑布局才能零拷贝，调用方据此决定是否走 mmap 包装路径
    EXPECT_TRUE(converter.is_passthrough());
    EXPECT_EQ(converter.output_size(), 4u * 4u * 3u / 2u);
    EXPECT_STREQ(converter.format_name(), "YUV420");
}

TEST(V4l2Converter, Yuv420WithPaddingIsNotPassthrough)
{
    lowlat::V4l2Converter converter;
    std::string error;
    ASSERT_TRUE(converter.configure(V4L2_PIX_FMT_YUV420, 4, 4, 8, &error)) << error;
    EXPECT_FALSE(converter.is_passthrough());
}

TEST(V4l2Converter, Yuv420StridedCopyDropsPaddingBytes)
{
    lowlat::V4l2Converter converter;
    std::string error;
    // 宽 4 高 2，行跨度为 8（每行有 4 字节填充）
    ASSERT_TRUE(converter.configure(V4L2_PIX_FMT_YUV420, 4, 2, 8, &error)) << error;

    const std::uint8_t padding = 0xEE;
    std::vector<std::uint8_t> src(24, padding);
    // Y 平面：两行，每行前 4 字节有效
    const std::uint8_t y_values[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 4; ++col) {
            src[static_cast<std::size_t>(row) * 8 + static_cast<std::size_t>(col)] =
                y_values[row * 4 + col];
        }
    }
    // 色度平面在 Y 之后：U 占 16..19，V 占 20..23，各只用前 2 字节
    src[16] = 9;
    src[17] = 10;
    src[20] = 11;
    src[21] = 12;

    std::vector<std::uint8_t> dst(converter.output_size(), 0);
    ASSERT_TRUE(converter.convert(src.data(), src.size(), dst.data(), &error)) << error;

    const std::uint8_t expected_y[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(dst[static_cast<std::size_t>(i)], expected_y[i]) << "Y[" << i << "]";
    }
    EXPECT_EQ(dst[8], 9);
    EXPECT_EQ(dst[9], 10);
    EXPECT_EQ(dst[10], 11);
    EXPECT_EQ(dst[11], 12);

    // 填充字节不能被带进结果
    for (std::uint8_t value : dst) {
        EXPECT_NE(value, padding);
    }
}

TEST(V4l2Converter, YuyvToI420AveragesChromaOverTwoByTwoBlock)
{
    lowlat::V4l2Converter converter;
    std::string error;
    ASSERT_TRUE(converter.configure(V4L2_PIX_FMT_YUYV, 4, 2, 8, &error)) << error;

    // 宽 4 高 2：每行 8 字节（4 像素 × 2 字节），共 16 字节
    // 每 4 字节 = Y0 U Y1 V，对应两个相邻像素
    std::vector<std::uint8_t> src = {
        10, 100, 20, 200, 30, 110, 40, 210,  // 第 0 行
        50, 120, 60, 220, 70, 130, 80, 230,  // 第 1 行
    };

    std::vector<std::uint8_t> dst(converter.output_size(), 0);
    ASSERT_TRUE(converter.convert(src.data(), src.size(), dst.data(), &error)) << error;

    // Y 平面原样保留
    const std::uint8_t expected_y[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(dst[static_cast<std::size_t>(i)], expected_y[i]) << "Y[" << i << "]";
    }

    // U/V 取上下两行的平均：(100+120)/2=110, (110+130)/2=120
    EXPECT_EQ(dst[8], 110) << "U[0]";
    EXPECT_EQ(dst[9], 120) << "U[1]";
    // V: (200+220)/2=210, (210+230)/2=220
    EXPECT_EQ(dst[10], 210) << "V[0]";
    EXPECT_EQ(dst[11], 220) << "V[1]";
}

TEST(V4l2Converter, YuyvRejectsTooSmallBuffer)
{
    lowlat::V4l2Converter converter;
    std::string error;
    ASSERT_TRUE(converter.configure(V4L2_PIX_FMT_YUYV, 4, 2, 8, &error)) << error;

    std::vector<std::uint8_t> src(8, 0);  // 需要 16 字节
    std::vector<std::uint8_t> dst(converter.output_size(), 0);
    EXPECT_FALSE(converter.convert(src.data(), src.size(), dst.data(), &error));
    EXPECT_FALSE(error.empty());
}

TEST(V4l2Converter, RejectsYuyvWithInsufficientStride)
{
    lowlat::V4l2Converter converter;
    std::string error;
    // YUYV 每像素 2 字节，bytesperline 至少要 width*2
    EXPECT_FALSE(converter.configure(V4L2_PIX_FMT_YUYV, 8, 4, 8, &error));
}

TEST(V4l2Converter, MjpegConfigureSucceeds)
{
    lowlat::V4l2Converter converter;
    std::string error;
    ASSERT_TRUE(converter.configure(V4L2_PIX_FMT_MJPEG, 32, 32, 0, &error)) << error;
    EXPECT_FALSE(converter.is_passthrough());
    EXPECT_STREQ(converter.format_name(), "MJPG");
    EXPECT_EQ(converter.output_size(), 32u * 32u * 3u / 2u);
}

TEST(V4l2Converter, MjpegRejectsGarbageInput)
{
    lowlat::V4l2Converter converter;
    std::string error;
    ASSERT_TRUE(converter.configure(V4L2_PIX_FMT_MJPEG, 32, 32, 0, &error)) << error;

    std::vector<std::uint8_t> garbage(64, 0xAB);
    std::vector<std::uint8_t> dst(converter.output_size(), 0);
    // 必须返回 false 而不是崩溃
    EXPECT_FALSE(converter.convert(garbage.data(), garbage.size(), dst.data(), &error));
    EXPECT_FALSE(error.empty());
}

TEST(V4l2Converter, ResetClearsConfiguration)
{
    lowlat::V4l2Converter converter;
    std::string error;
    ASSERT_TRUE(converter.configure(V4L2_PIX_FMT_YUYV, 4, 4, 8, &error)) << error;
    converter.reset();

    EXPECT_FALSE(converter.is_passthrough());
    EXPECT_EQ(converter.output_size(), 0u);
    EXPECT_STREQ(converter.format_name(), "none");

    std::vector<std::uint8_t> dst(4, 0);
    std::vector<std::uint8_t> src(4, 0);
    EXPECT_FALSE(converter.convert(src.data(), src.size(), dst.data(), &error));
}
