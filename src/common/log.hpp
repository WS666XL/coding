#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace lowlat {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
};

struct LogConfig {
    LogLevel level{LogLevel::Info};
    std::string log_dir{"logs"};
    std::string file_name{"lowlat.log"};
    bool console{true};
    bool to_file{true};
    std::size_t max_file_size{16u * 1024u * 1024u};
    std::size_t max_files{5};
};

// 全局日志器。未调用 init() 时，handle() 返回 spdlog 的默认控制台日志器，
// 因此日志宏在任何阶段都可以安全使用。
class Logger {
public:
    static Logger& instance() noexcept;

    // 返回 false 表示初始化失败（例如日志目录不可创建），调用方可决定降级策略。
    bool init(const LogConfig& config);
    void shutdown() noexcept;

    bool initialized() const noexcept { return initialized_; }

    std::shared_ptr<spdlog::logger> handle() const noexcept;

private:
    Logger() = default;

    std::shared_ptr<spdlog::logger> logger_;
    bool initialized_{false};
};

namespace detail {

// 供 LOWLAT_LOG_EVERY_MS 使用：同一处调用点在 interval_ms 内最多写一条。
void log_throttled(spdlog::level::level_enum level,
                   std::atomic<std::int64_t>& last_ms,
                   std::int64_t interval_ms,
                   const std::string& message);

}  // namespace detail
}  // namespace lowlat

// ---------------------------------------------------------------- 常规日志
#define LOWLAT_LOG_TRACE(...) ::lowlat::Logger::instance().handle()->trace(__VA_ARGS__)
#define LOWLAT_LOG_DEBUG(...) ::lowlat::Logger::instance().handle()->debug(__VA_ARGS__)
#define LOWLAT_LOG_INFO(...) ::lowlat::Logger::instance().handle()->info(__VA_ARGS__)
#define LOWLAT_LOG_WARN(...) ::lowlat::Logger::instance().handle()->warn(__VA_ARGS__)
#define LOWLAT_LOG_ERROR(...) ::lowlat::Logger::instance().handle()->error(__VA_ARGS__)

// ---------------------------------------------------------------- 限频日志
// 热路径（每帧 / 每包）必须使用本宏，避免逐帧日志的 IO 抖动污染延迟测量。
// 用法：LOWLAT_LOG_EVERY_MS(1000, info, "sent={} dropped={}", sent, dropped);
#define LOWLAT_LOG_EVERY_MS(interval_ms, lvl, ...)                              \
    do {                                                                        \
        static std::atomic<std::int64_t> lowlat_last_ms_{0};                     \
        ::lowlat::detail::log_throttled(::spdlog::level::lvl, lowlat_last_ms_,   \
                                        (interval_ms),                           \
                                        ::spdlog::fmt_lib::format(__VA_ARGS__)); \
    } while (false)
