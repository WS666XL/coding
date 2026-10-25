add_library(lowlat_protocol STATIC
    rtp_lite.cpp
    h264_fragmenter.cpp
    fec_xor.cpp
    nack.cpp
    jitter_buffer.cpp
    rtt_estimator.cpp
    bandwidth_estimator.cpp
    control_message.cpp
)

target_link_libraries(lowlat_protocol PUBLIC lowlat_common)
lowlat_target_warnings(lowlat_protocol)
