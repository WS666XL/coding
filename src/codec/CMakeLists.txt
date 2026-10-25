find_package(PkgConfig REQUIRED)
pkg_check_modules(X264 REQUIRED IMPORTED_TARGET x264)
# decoder 需要 libavcodec；libavutil 放在 Requires.private 里，必须显式请求
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec libavutil)

add_library(lowlat_codec STATIC
    encoder.cpp
    decoder.cpp
)

target_link_libraries(lowlat_codec
    PUBLIC
        lowlat_common
        lowlat_capture
        PkgConfig::X264
        PkgConfig::AVCODEC
)
lowlat_target_warnings(lowlat_codec)
