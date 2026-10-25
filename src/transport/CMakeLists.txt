add_library(lowlat_transport STATIC
    udp_socket.cpp
    stream_sender.cpp
    stream_receiver.cpp
)

target_link_libraries(lowlat_transport PUBLIC lowlat_common lowlat_protocol)
lowlat_target_warnings(lowlat_transport)
