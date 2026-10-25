#include "transport/udp_socket.hpp"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace lowlat {
namespace {

constexpr std::size_t kMaxBatchPerCall = 64;

std::string with_errno(const std::string& what)
{
    return what + ": " + std::strerror(errno);
}

bool resolve_ipv4(const std::string& host, in_addr* out) noexcept
{
    if (host.empty() || host == "0.0.0.0") {
        out->s_addr = htonl(INADDR_ANY);
        return true;
    }
    return ::inet_pton(AF_INET, host.c_str(), out) == 1;
}

}  // namespace

std::string UdpEndpoint::to_string() const
{
    const std::string host = address.empty() ? "0.0.0.0" : address;
    return host + ":" + std::to_string(port);
}

bool parse_endpoint(const std::string& text, UdpEndpoint* out, std::string* error)
{
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "parse_endpoint 输出指针为空";
        }
        return false;
    }

    const std::size_t pos = text.rfind(':');
    if (pos == std::string::npos) {
        if (error != nullptr) {
            *error = "端点格式应为 host:port，收到: " + text;
        }
        return false;
    }

    const std::string host = text.substr(0, pos);
    const std::string port_text = text.substr(pos + 1);
    if (port_text.empty()) {
        if (error != nullptr) {
            *error = "端点缺少端口号: " + text;
        }
        return false;
    }

    char* end = nullptr;
    const long port = std::strtol(port_text.c_str(), &end, 10);
    if (end == port_text.c_str() || *end != '\0' || port <= 0 || port > 65535) {
        if (error != nullptr) {
            *error = "端点端口非法: " + port_text;
        }
        return false;
    }

    out->address = host;
    out->port = static_cast<std::uint16_t>(port);
    return true;
}

UdpSocket::~UdpSocket()
{
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : fd_(other.fd_),
      local_port_(other.local_port_),
      has_destination_(other.has_destination_),
      destination_(other.destination_)
{
    other.fd_ = -1;
    other.local_port_ = 0;
    other.has_destination_ = false;
    other.destination_ = sockaddr_in{};
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this != &other) {
        close();
        fd_ = other.fd_;
        local_port_ = other.local_port_;
        has_destination_ = other.has_destination_;
        destination_ = other.destination_;
        other.fd_ = -1;
        other.local_port_ = 0;
        other.has_destination_ = false;
        other.destination_ = sockaddr_in{};
    }
    return *this;
}

bool UdpSocket::open(const std::string& bind_address, std::uint16_t bind_port, std::string* error)
{
    close();

    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        if (error != nullptr) {
            *error = with_errno("socket 创建失败");
        }
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(bind_port);
    if (!resolve_ipv4(bind_address, &address.sin_addr)) {
        if (error != nullptr) {
            *error = "无法解析绑定地址: " + bind_address;
        }
        close();
        return false;
    }

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (error != nullptr) {
            *error = with_errno("bind " + bind_address + ":" + std::to_string(bind_port) + " 失败");
        }
        close();
        return false;
    }

    sockaddr_in actual{};
    socklen_t length = sizeof(actual);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&actual), &length) == 0) {
        local_port_ = ntohs(actual.sin_port);
    }
    return true;
}

void UdpSocket::close() noexcept
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    local_port_ = 0;
    has_destination_ = false;
}

bool UdpSocket::set_destination(const UdpEndpoint& destination, std::string* error)
{
    if (fd_ < 0) {
        if (error != nullptr) {
            *error = "socket 尚未打开";
        }
        return false;
    }
    if (!destination.valid()) {
        if (error != nullptr) {
            *error = "目标端点非法（端口为 0）";
        }
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(destination.port);
    if (!resolve_ipv4(destination.address, &address.sin_addr)) {
        if (error != nullptr) {
            *error = "无法解析目标地址: " + destination.address;
        }
        return false;
    }

    destination_ = address;
    has_destination_ = true;
    return true;
}

bool UdpSocket::set_receive_buffer(std::size_t bytes, std::string* error)
{
    if (fd_ < 0) {
        if (error != nullptr) {
            *error = "socket 尚未打开";
        }
        return false;
    }
    const int value = static_cast<int>(bytes);
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) != 0) {
        if (error != nullptr) {
            *error = with_errno("设置 SO_RCVBUF 失败");
        }
        return false;
    }
    return true;
}

bool UdpSocket::set_send_buffer(std::size_t bytes, std::string* error)
{
    if (fd_ < 0) {
        if (error != nullptr) {
            *error = "socket 尚未打开";
        }
        return false;
    }
    const int value = static_cast<int>(bytes);
    if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)) != 0) {
        if (error != nullptr) {
            *error = with_errno("设置 SO_SNDBUF 失败");
        }
        return false;
    }
    return true;
}

SendReport UdpSocket::send_batch(const ByteSpan* payloads, std::size_t count, std::string* error)
{
    SendReport report;
    if (fd_ < 0) {
        if (error != nullptr) {
            *error = "socket 尚未打开";
        }
        return report;
    }
    if (!has_destination_) {
        if (error != nullptr) {
            *error = "尚未设置发送目标";
        }
        return report;
    }
    if (payloads == nullptr || count == 0) {
        return report;
    }

    while (report.sent < count) {
        const std::size_t chunk = std::min(kMaxBatchPerCall, count - report.sent);

        std::array<iovec, kMaxBatchPerCall> iovs{};
        std::array<mmsghdr, kMaxBatchPerCall> messages{};
        for (std::size_t i = 0; i < chunk; ++i) {
            const ByteSpan& span = payloads[report.sent + i];
            iovs[i].iov_base = const_cast<std::uint8_t*>(span.data());
            iovs[i].iov_len = span.size();
            messages[i].msg_hdr.msg_name = &destination_;
            messages[i].msg_hdr.msg_namelen = sizeof(destination_);
            messages[i].msg_hdr.msg_iov = &iovs[i];
            messages[i].msg_hdr.msg_iovlen = 1;
        }

        const int sent = ::sendmmsg(fd_, messages.data(), static_cast<unsigned int>(chunk), 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                report.would_block = true;
                return report;
            }
            if (errno == EINTR) {
                continue;
            }
            if (error != nullptr) {
                *error = with_errno("sendmmsg 失败");
            }
            return report;
        }

        report.sent += static_cast<std::size_t>(sent);
        if (static_cast<std::size_t>(sent) < chunk) {
            report.would_block = true;
            return report;
        }
    }

    return report;
}

bool UdpSocket::send(ByteSpan payload, std::string* error)
{
    const SendReport report = send_batch(&payload, 1, error);
    return report.sent == 1;
}

bool UdpSocket::send_to(const UdpEndpoint& destination, ByteSpan payload, std::string* error)
{
    if (fd_ < 0) {
        if (error != nullptr) {
            *error = "socket 尚未打开";
        }
        return false;
    }
    if (!destination.valid() || payload.empty()) {
        if (error != nullptr) {
            *error = "send_to 参数非法";
        }
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(destination.port);
    if (!resolve_ipv4(destination.address, &address.sin_addr)) {
        if (error != nullptr) {
            *error = "无法解析目标地址: " + destination.address;
        }
        return false;
    }

    const ssize_t sent = ::sendto(fd_, payload.data(), payload.size(), 0,
                                  reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;  // 发送缓冲满：调用方当作可丢弃的控制报文处理
        }
        if (error != nullptr) {
            *error = with_errno("sendto 失败");
        }
        return false;
    }
    return static_cast<std::size_t>(sent) == payload.size();
}

std::ptrdiff_t UdpSocket::receive(std::uint8_t* buffer, std::size_t capacity,
                                  UdpEndpoint* from, std::string* error)
{
    if (fd_ < 0) {
        if (error != nullptr) {
            *error = "socket 尚未打开";
        }
        return -1;
    }
    if (buffer == nullptr || capacity == 0) {
        if (error != nullptr) {
            *error = "接收缓冲区为空";
        }
        return -1;
    }

    sockaddr_in peer{};
    socklen_t peer_length = sizeof(peer);
    const ssize_t received =
        ::recvfrom(fd_, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&peer), &peer_length);

    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        if (error != nullptr) {
            *error = with_errno("recvfrom 失败");
        }
        return -1;
    }

    if (from != nullptr) {
        char text[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text));
        from->address = text;
        from->port = ntohs(peer.sin_port);
    }
    return static_cast<std::ptrdiff_t>(received);
}

}  // namespace lowlat
