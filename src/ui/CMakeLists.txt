find_package(Qt6 QUIET COMPONENTS Widgets OpenGL OpenGLWidgets)

if(NOT Qt6_FOUND)
    message(STATUS "lowlat_ui: 跳过（未找到 Qt6）")
    return()
endif()

add_executable(lowlat_ui
    main.cpp
    app_options.cpp
    offline_mode.cpp
    live_mode.cpp
    video_widget.cpp
    receiver_thread.cpp
    local_sender.cpp
    stats_panel.cpp
)

target_include_directories(lowlat_ui PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(lowlat_ui
    PRIVATE
        lowlat_common
        lowlat_codec
        lowlat_protocol
        lowlat_transport
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
)
lowlat_target_warnings(lowlat_ui)

install(TARGETS lowlat_ui RUNTIME DESTINATION bin)
