#include "codec/encoder.hpp"

#include <cstddef>

#include <x264.h>

namespace lowlat {

H264Encoder::~H264Encoder()
{
    close();
}

bool H264Encoder::open(const Config& config, std::string* error)
{
    close();

    if (config.width <= 0 || config.height <= 0) {
        if (error != nullptr) {
            *error = "编码尺寸必须为正数";
        }
        return false;
    }
    if ((config.width % 2 != 0) || (config.height % 2 != 0)) {
        if (error != nullptr) {
            *error = "I420 编码要求宽高均为偶数";
        }
        return false;
    }
    if (config.fps <= 0 || config.bitrate_kbps <= 0) {
        if (error != nullptr) {
            *error = "fps 与码率必须为正数";
        }
        return false;
    }

    x264_param_t param;
    if (x264_param_default_preset(&param, config.preset.c_str(), config.tune.c_str()) < 0) {
        if (error != nullptr) {
            *error = "x264 预设参数设置失败: " + config.preset + "/" + config.tune;
        }
        return false;
    }

    param.i_csp = X264_CSP_I420;
    param.i_width = config.width;
    param.i_height = config.height;
    param.i_fps_num = config.fps;
    param.i_fps_den = 1;
    param.i_timebase_num = 1;
    param.i_timebase_den = config.fps;
    param.b_vfr_input = 0;
    param.b_repeat_headers = 1;
    param.b_annexb = 1;

    // 零延迟显式设定，不依赖 tune 的默认值，方便逐条解释取舍
    param.i_bframe = 0;
    param.i_bframe_pyramid = 0;
    param.rc.i_lookahead = 0;
    param.i_sync_lookahead = 0;
    param.b_sliced_threads = 1;

    param.i_keyint_max = config.fps * (config.keyint_sec > 0 ? config.keyint_sec : 1);
    param.i_keyint_min = param.i_keyint_max;
    param.rc.i_rc_method = X264_RC_ABR;
    param.rc.i_bitrate = config.bitrate_kbps;
    param.i_log_level = X264_LOG_WARNING;

    handle_ = x264_encoder_open(&param);
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = "x264_encoder_open 失败，请检查尺寸与参数";
        }
        return false;
    }

    config_ = config;
    return true;
}

void H264Encoder::close() noexcept
{
    if (handle_ != nullptr) {
        x264_encoder_close(handle_);
        handle_ = nullptr;
    }
}

namespace {

bool collect_nals(x264_nal_t* nals, int nal_count, const YuvFrame& frame, std::vector<Packet>* out)
{
    for (int i = 0; i < nal_count; ++i) {
        if (nals[i].i_payload <= 0 || nals[i].p_payload == nullptr) {
            continue;
        }
        Packet packet;
        packet.data.assign(nals[i].p_payload,
                           nals[i].p_payload + static_cast<std::size_t>(nals[i].i_payload));
        packet.pts_us = frame.capture_ts_us;
        packet.frame_index = frame.frame_index;
        packet.keyframe = (nals[i].i_type == NAL_SLICE_IDR);
        out->push_back(std::move(packet));
    }
    return true;
}

}  // namespace

bool H264Encoder::encode(const YuvFrame& frame, std::vector<Packet>* out, std::string* error)
{
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = "编码器尚未打开";
        }
        return false;
    }
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "encode 输出指针为空";
        }
        return false;
    }
    if (!frame.valid()) {
        if (error != nullptr) {
            *error = "输入帧无效";
        }
        return false;
    }
    if (frame.width != config_.width || frame.height != config_.height) {
        if (error != nullptr) {
            *error = "输入帧尺寸与编码器配置不一致";
        }
        return false;
    }

    x264_picture_t picture;
    x264_picture_init(&picture);
    picture.i_pts = static_cast<std::int64_t>(frame.frame_index);
    picture.img.i_csp = X264_CSP_I420;
    picture.img.i_plane = 3;
    picture.img.plane[0] = frame.y_plane();
    picture.img.plane[1] = frame.u_plane();
    picture.img.plane[2] = frame.v_plane();
    picture.img.i_stride[0] = config_.width;
    picture.img.i_stride[1] = config_.width / 2;
    picture.img.i_stride[2] = config_.width / 2;

    x264_nal_t* nals = nullptr;
    int nal_count = 0;
    x264_picture_t picture_out;
    x264_picture_init(&picture_out);

    const int frame_size =
        x264_encoder_encode(handle_, &nals, &nal_count, &picture, &picture_out);
    if (frame_size < 0) {
        if (error != nullptr) {
            *error = "x264_encoder_encode 返回错误";
        }
        return false;
    }

    return collect_nals(nals, nal_count, frame, out);
}

bool H264Encoder::flush(std::vector<Packet>* out, std::string* error)
{
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = "编码器尚未打开";
        }
        return false;
    }
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "flush 输出指针为空";
        }
        return false;
    }

    YuvFrame placeholder;
    placeholder.frame_index = 0;
    placeholder.capture_ts_us = 0;

    while (true) {
        x264_nal_t* nals = nullptr;
        int nal_count = 0;
        x264_picture_t picture_out;
        x264_picture_init(&picture_out);

        const int frame_size = x264_encoder_encode(handle_, &nals, &nal_count, nullptr, &picture_out);
        if (frame_size < 0) {
            if (error != nullptr) {
                *error = "x264 flush 返回错误";
            }
            return false;
        }
        if (frame_size == 0 && nal_count == 0) {
            break;
        }
        collect_nals(nals, nal_count, placeholder, out);
    }
    return true;
}

bool H264Encoder::set_bitrate(int bitrate_kbps, std::string* error)
{
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = "编码器尚未打开";
        }
        return false;
    }
    if (bitrate_kbps <= 0) {
        if (error != nullptr) {
            *error = "码率必须为正数";
        }
        return false;
    }

    // 不走 x264_encoder_reconfig：本机 x264 164 上实测无效——
    // 把 rc.i_bitrate 从 3000 改到 300 后，后 40 帧的码流大小
    // 反而是 481437 字节（改之前 474094），完全没有下降。
    // tests/test_encoder.cpp 的 RuntimeBitrateReconfigActuallyChangesOutput
    // 就是用来盯着这个行为的。
    //
    // 改为重建编码器：open() 内部会先 close()，是安全的。
    // 代价是重新输出一次 SPS/PPS + IDR（几毫秒），
    // 相比"码率改不动导致拥塞控制失效"完全值得。
    Config next = config_;
    next.bitrate_kbps = bitrate_kbps;
    return open(next, error);
}

}  // namespace lowlat
