add_executable(lowlat_sender
    sender_main.cpp
)

target_link_libraries(lowlat_sender PRIVATE lowlat_capture lowlat_codec lowlat_transport)
lowlat_target_warnings(lowlat_sender)

add_executable(lowlat_receiver
    receiver_main.cpp
)

target_link_libraries(lowlat_receiver PRIVATE lowlat_transport lowlat_codec)
lowlat_target_warnings(lowlat_receiver)

install(TARGETS lowlat_sender lowlat_receiver RUNTIME DESTINATION bin)
