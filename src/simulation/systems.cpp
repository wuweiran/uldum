#include "simulation/systems.h"
#include "simulation/world.h"
#include "core/log.h"

namespace uldum::simulation {

static bool s_first_tick = true;

void system_health(World& world, float dt) {
    // Apply HP regeneration
    for (u32 i = 0; i < world.healths.count(); ++i) {
        auto& hp = world.healths.data()[i];
        if (hp.current > 0 && hp.current < hp.max && hp.regen_per_sec > 0) {
            hp.current = std::min(hp.current + hp.regen_per_sec * dt, hp.max);
        }
    }
}

void system_state(World& world, float dt) {
    // Tick regen for map-defined states (mana, energy, etc.)
    for (u32 i = 0; i < world.state_blocks.count(); ++i) {
        auto& sb = world.state_blocks.data()[i];
        for (auto& [id, state] : sb.states) {
            if (state.current < state.max && state.regen_per_sec > 0) {
                state.current = std::min(state.current + state.regen_per_sec * dt, state.max);
            }
        }
    }
}

void system_movement(World& world, float /*dt*/) {
    if (s_first_tick) {
        log::trace("Systems", "movement (stub) — pathfinding pending");
    }
}

void system_combat(World& world, float /*dt*/) {
    if (s_first_tick) {
        log::trace("Systems", "combat (stub) — target acquisition, attack logic pending");
    }
}

void system_ability(World& world, float dt) {
    // Tick cooldowns + applied ability durations
    for (u32 i = 0; i < world.ability_sets.count(); ++i) {
        auto& aset = world.ability_sets.data()[i];
        // Tick cooldowns
        for (auto& ability : aset.abilities) {
            if (ability.cooldown_remaining > 0) {
                ability.cooldown_remaining = std::max(0.0f, ability.cooldown_remaining - dt);
            }
        }
        // Tick durations and remove expired applied abilities
        std::erase_if(aset.abilities, [dt](AbilityInstance& a) {
            if (a.remaining_duration < 0) return false;  // permanent / innate
            a.remaining_duration -= dt;
            return a.remaining_duration <= 0;
        });
    }
    // TODO: aura scanning, cast execution, periodic effects
}

void system_projectile(World& world, float /*dt*/) {
    if (s_first_tick) {
        log::trace("Systems", "projectile (stub) — movement, hit detection pending");
        s_first_tick = false;
    }
}

} // namespace uldum::simulation
