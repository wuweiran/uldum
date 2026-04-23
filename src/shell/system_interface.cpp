#include "shell/system_interface.h"
#include "core/log.h"

namespace uldum::shell {

static constexpr const char* TAG = "UI";

SystemInterface::SystemInterface()
    : m_start(std::chrono::steady_clock::now()) {}

double SystemInterface::GetElapsedTime() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - m_start).count();
}

bool SystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message) {
    switch (type) {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            log::error(TAG, "{}", message.c_str());
            break;
        case Rml::Log::LT_WARNING:
            log::warn(TAG, "{}", message.c_str());
            break;
        case Rml::Log::LT_INFO:
            log::info(TAG, "{}", message.c_str());
            break;
        case Rml::Log::LT_DEBUG:
            log::debug(TAG, "{}", message.c_str());
            break;
        case Rml::Log::LT_ALWAYS:
        case Rml::Log::LT_MAX:
        default:
            log::info(TAG, "{}", message.c_str());
            break;
    }
    return true;  // keep processing
}

} // namespace uldum::shell
