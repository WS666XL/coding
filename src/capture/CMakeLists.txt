add_library(lowlat_capture STATIC
    synthetic_source.cpp
    v4l2_source.cpp
    capture_factory.cpp
)

target_link_libraries(lowlat_capture PUBLIC lowlat_common)
lowlat_target_warnings(lowlat_capture)
