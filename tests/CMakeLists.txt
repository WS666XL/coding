find_package(GTest REQUIRED)
find_package(PkgConfig REQUIRED)
# libavcodec.pc 把 libavutil 放在 Requires.private，静态链接场景不会自动带上，
# 因此显式同时请求两者。
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec libavutil)

add_executable(lowlat_tests
    test_ring_buffer.cpp
    test_frame_pool.cpp
    test_stats.cpp
    test_byte_order.cpp
    test_cxx17_compliance.cpp
    test_encoder.cpp
    test_decoder.cpp
    test_v4l2_source.cpp
    test_protocol_roundtrip.cpp
    test_fragmenter.cpp
    test_nal_integration.cpp
    test_udp_socket.cpp
    test_stream_sender.cpp
    test_stream_receiver.cpp
    test_fec_xor.cpp
    test_nack.cpp
    test_jitter_buffer.cpp
    test_nack_scheduler.cpp
    test_rtt_estimator.cpp
    test_frame_slot.cpp
    test_bandwidth_estimator.cpp
    test_control_message.cpp
    test_file_source.cpp
)

target_link_libraries(lowlat_tests
    PRIVATE
        lowlat_common
        lowlat_capture
        lowlat_codec
        lowlat_protocol
        lowlat_transport
        PkgConfig::AVCODEC
        GTest::gtest_main
)

lowlat_target_warnings(lowlat_tests)

include(GoogleTest)
gtest_discover_tests(lowlat_tests)
