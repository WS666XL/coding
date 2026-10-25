#pragma once

// FFmpeg 头文件链接兼容层。
//
// 背景：本机 libavcodec-dev(6.1.1) 提供的头文件里没有任何 extern "C" 保护
// （对 avcodec.h/codec.h/frame.h 的 grep 计数均为 0）。直接用 C++ 编译时，
// 声明会获得 C++ 链接性，目标文件里出现 _Z20avcodec_find_decoder9AVCodecID 这类
// 修饰名，链接 libavcodec.so 必然报 undefined reference。
//
// 处理方式：统一在 extern "C" 作用域内包含。嵌套 extern "C" 在 C++ 中是合法的，
// 因此对"带保护的标准 FFmpeg 头文件"也完全安全，不会重复引入符号。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}
