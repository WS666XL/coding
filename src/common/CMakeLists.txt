find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

add_library(lowlat_common STATIC
    log.cpp
    stats.cpp
)

# 统一以 src/ 为根，源码内使用 "common/xxx.hpp" 形式包含
target_include_directories(lowlat_common PUBLIC "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(lowlat_common
    PUBLIC
        spdlog::spdlog
        Threads::Threads
)

lowlat_target_warnings(lowlat_common)
