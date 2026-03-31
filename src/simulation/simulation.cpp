#include "simulation/simulation.h"
#include "script/script.h"
#include "core/log.h"

namespace uldum::simulation {

static constexpr const char* TAG = "Simulation";
static bool s_first_tick = true;

bool Simulation::init(script::ScriptEngine& script) {
    (void)script;
    log::info(TAG, "Simulation initialized (stub) — ECS, units, pathfinding, AI pending");
    return true;
}

void Simulation::shutdown() {
    log::info(TAG, "Simulation shut down (stub)");
}

void Simulation::tick(float dt) {
    if (s_first_tick) {
        log::trace(TAG, "tick (stub) dt={:.4f}s — will run movement, combat, ability systems here", dt);
        s_first_tick = false;
    }
}

} // namespace uldum::simulation
