cmake_minimum_required(VERSION 3.20)
project(lowlat_video VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------- C++17 硬约束
set(LOWLAT_CXX_STANDARD 17 CACHE STRING "本项目语言标准，只允许 17")
if(NOT LOWLAT_CXX_STANDARD EQUAL 17)
    message(FATAL_ERROR
        "lowlat-video 固定使用 C++17，收到 LOWLAT_CXX_STANDARD=${LOWLAT_CXX_STANDARD}")
endif()

set(CMAKE_CXX_STANDARD ${LOWLAT_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 配置期断言：C++20 头必须不可用。若有人把标准提到 20+，这里会直接失败。
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
    #include <span>
    int main() {
        int raw[1] = {0};
        std::span<int> view(raw);
        return static_cast<int>(view.size());
    }
" LOWLAT_HAS_STD_SPAN)
if(LOWLAT_HAS_STD_SPAN)
    message(FATAL_ERROR
        "检测到 C++20 的 <span> 可用，说明编译标准被提高到 C++20 以上。"
        "本项目锁定 C++17，请检查 CMAKE_CXX_STANDARD 与依赖的接口要求。")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

option(LOWLAT_BUILD_TESTS "构建单元测试" ON)
option(LOWLAT_ENABLE_PACKAGING "启用 CPack 打包（P6 阶段开启）" OFF)

# ---------------------------------------------------------------- 模块
add_subdirectory(src/common)
add_subdirectory(src/capture)
add_subdirectory(src/codec)
add_subdirectory(src/protocol)
add_subdirectory(src/transport)
add_subdirectory(src/app)
add_subdirectory(tools)

if(LOWLAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- 打包（P6）
if(LOWLAT_ENABLE_PACKAGING)
    set(CPACK_PACKAGE_NAME "lowlat-video")
    set(CPACK_PACKAGE_VENDOR "lowlat")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_CONTACT "lowlat <lowlat@example.com>")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Low-latency realtime video transport over UDP")
    # 必须写成单个字符串：拆成多个参数会被当成 CMake list，
    # CPack 会用 ';' 把它们拼起来，control 文件里就会出现多余分号。
    set(CPACK_PACKAGE_DESCRIPTION "Sender/receiver for H.264 over a custom RTP-lite protocol with XOR FEC, NACK retransmission and adaptive jitter buffering.")

    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "lowlat <lowlat@example.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "video")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # 由 dpkg-shlibdeps 自动推导运行时依赖，避免手写库版本号
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    # 生成标准的 包名_版本_架构.deb
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

    # systemd 单元：装到 /usr/lib/systemd/system
    install(FILES packaging/lowlat-receiver.service
        DESTINATION lib/systemd/system)
    install(FILES README.md
        DESTINATION share/doc/lowlat-video)

    include(CPack)
endif()

message(STATUS "lowlat-video: C++${CMAKE_CXX_STANDARD} | build=${CMAKE_BUILD_TYPE} | tests=${LOWLAT_BUILD_TESTS}")
