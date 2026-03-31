#pragma once

#include <format>
#include <string_view>

namespace uldum::log {

enum class Level : int {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

void set_level(Level level);
void write(Level level, std::string_view tag, std::string_view message);

template <typename... Args>
void trace(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Trace, tag, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void debug(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, tag, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void info(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, tag, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, tag, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, tag, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace uldum::log
