find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec libavutil)

add_executable(h264_probe
    h264_probe.cpp
)

target_include_directories(h264_probe PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(h264_probe PRIVATE PkgConfig::AVCODEC)
lowlat_target_warnings(h264_probe)

install(TARGETS h264_probe RUNTIME DESTINATION bin)

# ---------------------------------------------------------------- Qt 探针
# 只用于验证窗口/GL 是否可用，不是交付物。Qt 缺失时直接跳过，
# 不让整个工程因为缺一个可选依赖而构建失败。
find_package(Qt6 QUIET COMPONENTS Widgets OpenGLWidgets)

if(Qt6_FOUND)
    add_executable(qt_probe qt_probe.cpp)
    target_link_libraries(qt_probe PRIVATE Qt6::Widgets Qt6::OpenGLWidgets)
    lowlat_target_warnings(qt_probe)
    message(STATUS "qt_probe: 已启用（Qt6 ${Qt6_VERSION}）")
else()
    message(STATUS "qt_probe: 跳过（未找到 Qt6 Widgets/OpenGLWidgets）")
endif()
