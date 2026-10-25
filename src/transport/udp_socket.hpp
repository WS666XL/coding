#pragma once

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <string>

#include "common/byte_span.hpp"

namespace lowlat {

struct UdpEndpoint {
    std::string address;
    std::uint16_t port{0};

    bool valid() const noexcept { return port != 0; }
    std::string to_string() const;
};

// 解析 "host:port"、"1.2.3.4:5000"、":5000"（仅端口）。
bool parse_endpoint(const std::string& text, UdpEndpoint* out, std::string* error);

// 一次批量发送的结果。
struct SendReport {
    std::size_t sent{0};
    bool would_block{false};  // 发送缓冲区满，剩余包未发出（拥塞信号，P5 会用到）
};

// 非阻塞 UDP socket 的 RAII 封装。
//
// 设计要点：
//   - 创建时即带 SOCK_NONBLOCK | SOCK_CLOEXEC，避免 O_NONBLOCK 的竞态
//   - 发送走 sendmmsg 批量提交，减少 syscall 次数
//   - EAGAIN 不是错误，而是"发送缓冲区满"，通过 SendReport.would_block 上报，
//     由调用方决定丢包还是重试（背压信号）
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    // bind_address 为空表示 INADDR_ANY；bind_port 为 0 时由系统分配。
    bool open(const std::string& bind_address, std::uint16_t bind_port, std::string* error);
    void close() noexcept;

    bool is_open() const noexcept { return fd_ >= 0; }
    int fd() const noexcept { return fd_; }
    std::uint16_t local_port() const noexcept { return local_port_; }

    bool set_destination(const UdpEndpoint& destination, std::string* error);
    bool set_receive_buffer(std::size_t bytes, std::string* error);
    bool set_send_buffer(std::size_t bytes, std::string* error);

    // 批量发送。返回实际发出的包数；would_block 表示因缓冲满提前返回。
    SendReport send_batch(const ByteSpan* payloads, std::size_t count, std::string* error);

    // 单个包发送（内部走 send_batch）。
    bool send(ByteSpan payload, std::string* error);

    // 发送到指定端点，不影响已设置的目标（用于反向控制报文，如 NACK）。
    bool send_to(const UdpEndpoint& destination, ByteSpan payload, std::string* error);

    // 非阻塞接收。返回读取字节数；0 表示当前无数据；-1 表示错误。
    std::ptrdiff_t receive(std::uint8_t* buffer, std::size_t capacity, UdpEndpoint* from,
                           std::string* error);

private:
    int fd_{-1};
    std::uint16_t local_port_{0};
    bool has_destination_{false};
    sockaddr_in destination_{};
};

}  // namespace lowlat
