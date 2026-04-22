#pragma once

#include <RmlUi/Core/SystemInterface.h>

#include <chrono>

namespace uldum::ui {

// Bridges RmlUi to engine logging + wall clock. No windowing concerns —
// RmlUi doesn't need them, the platform layer handles clipboard / cursor
// if we ever wire those.
class SystemInterface final : public Rml::SystemInterface {
public:
    SystemInterface();

    // RmlUi needs a monotonic elapsed-seconds clock for animations and
    // transitions. We use steady_clock anchored at construction.
    double GetElapsedTime() override;

    // RmlUi diagnostics → our log system.
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

private:
    std::chrono::steady_clock::time_point m_start;
};

} // namespace uldum::ui
