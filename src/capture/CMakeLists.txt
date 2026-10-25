find_package(PkgConfig REQUIRED)
# 文件源需要 demux + 色彩转换，合起来请求一次即可
pkg_check_modules(AVMEDIA REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)

add_library(lowlat_capture STATIC
    synthetic_source.cpp
    v4l2_source.cpp
    file_source.cpp
    capture_factory.cpp
)

target_link_libraries(lowlat_capture PUBLIC lowlat_common PkgConfig::AVMEDIA)
lowlat_target_warnings(lowlat_capture)
