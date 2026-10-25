#include "common/log.hpp"

#include <chrono>
#include <filesystem>
#include <system_error>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace lowlat {
namespace {

spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Trace:
        return spdlog::level::trace;
    case LogLevel::Debug:
        return spdlog::level::debug;
    case LogLevel::Info:
        return spdlog::level::info;
    case LogLevel::Warn:
        return spdlog::level::warn;
    case LogLevel::Error:
        return spdlog::level::err;
    case LogLevel::Critical:
        return spdlog::level::critical;
    }
    return spdlog::level::info;
}

std::int64_t now_ms() noexcept
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

Logger& Logger::instance() noexcept
{
    static Logger inst;
    return inst;
}

bool Logger::init(const LogConfig& config)
{
    try {
        std::vector<spdlog::sink_ptr> sinks;

        if (config.console) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_level(to_spdlog_level(config.level));
            sinks.push_back(console_sink);
        }

        if (config.to_file) {
            std::error_code ec;
            std::filesystem::create_directories(config.log_dir, ec);
            if (ec) {
                return false;
            }
            const std::filesystem::path path =
                std::filesystem::path(config.log_dir) / config.file_name;
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                path.string(), config.max_file_size, config.max_files);
            file_sink->set_level(to_spdlog_level(config.level));
            sinks.push_back(file_sink);
        }

        if (sinks.empty()) {
            return false;
        }

        auto created = std::make_shared<spdlog::logger>("lowlat", sinks.begin(), sinks.end());
        created->set_level(to_spdlog_level(config.level));
        created->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [t:%t] %v");
        created->flush_on(spdlog::level::warn);

        spdlog::drop("lowlat");
        spdlog::register_logger(created);

        logger_ = std::move(created);
        initialized_ = true;
        return true;
    } catch (const spdlog::spdlog_ex&) {
        return false;
    } catch (const std::system_error&) {
        return false;
    }
}

void Logger::shutdown() noexcept
{
    if (logger_) {
        logger_->flush();
        logger_.reset();
        spdlog::drop("lowlat");
    }
    initialized_ = false;
}

std::shared_ptr<spdlog::logger> Logger::handle() const noexcept
{
    return logger_ ? logger_ : spdlog::default_logger();
}

namespace detail {

void log_throttled(spdlog::level::level_enum level,
                   std::atomic<std::int64_t>& last_ms,
                   std::int64_t interval_ms,
                   const std::string& message)
{
    const std::int64_t current = now_ms();
    const std::int64_t previous = last_ms.load(std::memory_order_relaxed);
    if (previous != 0 && (current - previous) < interval_ms) {
        return;
    }
    last_ms.store(current, std::memory_order_relaxed);
    Logger::instance().handle()->log(level, message);
}

}  // namespace detail
}  // namespace lowlat
