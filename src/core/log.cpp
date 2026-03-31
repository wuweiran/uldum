#include "core/log.h"

#include <print>
#include <chrono>

#ifdef ULDUM_PLATFORM_WINDOWS
#include <Windows.h>
#endif

namespace uldum::log {

static Level s_min_level = Level::Trace;

void set_level(Level level) {
    s_min_level = level;
}

static constexpr std::string_view level_str(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

void write(Level level, std::string_view tag, std::string_view message) {
    if (level < s_min_level) return;

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::floor<std::chrono::milliseconds>(now);

    auto line = std::format("[{:%H:%M:%S}] [{}] [{}] {}\n", time, level_str(level), tag, message);

    std::print("{}", line);

#ifdef ULDUM_PLATFORM_WINDOWS
    OutputDebugStringA(line.c_str());
#endif
}

} // namespace uldum::log
