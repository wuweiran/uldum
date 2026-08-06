#pragma once

// DevApp — the engine-built-in App that powers `uldum_dev`.
//
// Holds the existing ImGui-based DevConsole and translates its user
// actions (start session, claim slot, leave lobby, etc.) into the
// Engine state transitions they correspond to. After the de-friending
// step DevApp reaches Engine through Engine's public verbs only — the
// same surface a future SampleGameApp would use.
//
// The whole file exists so Engine's lifecycle path goes through one
// `std::unique_ptr<App>` regardless of build, with no `#ifdef
// ULDUM_DEV_UI` branches around dev-console-specific calls.

#include "app/app.h"

#include <memory>
#include <string>

namespace uldum {

class Engine;
class DevConsole;

class DevApp final : public App {
public:
    DevApp();
    ~DevApp() override;

    void on_init(Engine&) override;
    void on_update(f32 dt) override;
    void on_render(rhi::CommandList& cmd) override;
    void on_session_ended() override;

    // Locale-change subscriber routes the active locale into the dev
    // console so its text input reflects what the engine has applied.
    // Stays as a free method (not part of App) — only DevApp needs it,
    // and it's called from engine.cpp's existing settings subscription.
    void set_active_locale(std::string code);

private:
    Engine*                      m_engine = nullptr;
    std::unique_ptr<DevConsole>  m_console;

    // Dev convenience: a joining client auto-claims the first open slot so
    // you don't have to click Claim every time. Throttled by the last slot
    // we attempted, so a lost race (someone else grabbed it) just retries
    // the next open slot on the following snapshot instead of spamming.
    u32 m_auto_claim_attempted = 0xFFFFFFFFu;  // UINT32_MAX = none attempted
};

} // namespace uldum
