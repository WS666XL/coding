#include "capture/capture_factory.hpp"

#include "capture/synthetic_source.hpp"
#include "capture/v4l2_source.hpp"

namespace lowlat {

std::unique_ptr<CaptureSource> create_capture_source(const CaptureOptions& options,
                                                     std::string* error)
{
    if (options.source == "synthetic") {
        return std::make_unique<SyntheticSource>(options.width, options.height, options.fps,
                                                 options.realtime);
    }
    if (options.source == "v4l2") {
        return std::make_unique<V4l2Source>(options.device, options.width, options.height,
                                            options.fps, options.buffer_count);
    }

    if (error != nullptr) {
        *error = "未知采集源: " + options.source + "（可选 synthetic / v4l2）";
    }
    return nullptr;
}

}  // namespace lowlat
