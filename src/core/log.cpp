#include "core/log.h"

#include <print>
#include <chrono>
#include <fstream>
#include <string>

#ifdef ULDUM_PLATFORM_WINDOWS
#include <Windows.h>
#endif

#ifdef ULDUM_PLATFORM_ANDROID
#include <android/log.h>
#endif

namespace uldum::log {

static Level s_min_level = Level::Trace;

// Desktop-only file sink — opens run.log next to the exe on first use. Flushed
// on every write so a crash leaves a readable trail even if stdout is buffered
// or redirected somewhere we can't see. Android skips this and relies on logcat.
#ifndef ULDUM_PLATFORM_ANDROID
static std::ofstream& log_file() {
    static std::ofstream f("run.log", std::ios::out | std::ios::trunc);
    return f;
}
#endif

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

#ifdef ULDUM_PLATFORM_ANDROID
static int android_priority(Level level) {
    switch (level) {
        case Level::Trace: return ANDROID_LOG_VERBOSE;
        case Level::Debug: return ANDROID_LOG_DEBUG;
        case Level::Info:  return ANDROID_LOG_INFO;
        case Level::Warn:  return ANDROID_LOG_WARN;
        case Level::Error: return ANDROID_LOG_ERROR;
    }
    return ANDROID_LOG_DEFAULT;
}
#endif

void write(Level level, std::string_view tag, std::string_view message) {
    if (level < s_min_level) return;

#ifdef ULDUM_PLATFORM_ANDROID
    // logcat prepends its own timestamp and level glyph, and filters by tag.
    // Feed it the raw message with our own tag so `adb logcat -s <tag>:*` works.
    // NUL-terminate the views since __android_log_* needs C strings.
    std::string tag_z(tag);
    std::string msg_z(message);
    __android_log_write(android_priority(level), tag_z.c_str(), msg_z.c_str());
#else
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::floor<std::chrono::milliseconds>(now);

    auto line = std::format("[{:%H:%M:%S}] [{}] [{}] {}\n", time, level_str(level), tag, message);

    std::print("{}", line);

    if (auto& f = log_file(); f) {
        f << line;
        f.flush();
    }

#ifdef ULDUM_PLATFORM_WINDOWS
    OutputDebugStringA(line.c_str());
#endif
#endif
}

} // namespace uldum::log
