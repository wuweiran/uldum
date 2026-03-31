#include "simulation/systems.h"
#include "simulation/world.h"
#include "core/log.h"

namespace uldum::simulation {

static bool s_first_tick = true;

void system_health(World& world, float dt) {
    // Apply regeneration
    auto ids = world.healths.ids();
    auto data = world.healths.data();
    for (u32 i = 0; i < world.healths.count(); ++i) {
        auto& hp = data[i];
        if (hp.current > 0 && hp.current < hp.max && hp.regen_per_sec > 0) {
            hp.current = std::min(hp.current + hp.regen_per_sec * dt, hp.max);
        }
    }
}

void system_movement(World& world, float /*dt*/) {
    if (s_first_tick) {
        log::trace("Systems", "movement (stub) — pathfinding pending");
    }
    // TODO: read OrderQueue for Move orders, update Transform based on Movement speed
}

void system_combat(World& world, float /*dt*/) {
    if (s_first_tick) {
        log::trace("Systems", "combat (stub) — target acquisition, attack logic pending");
    }
    // TODO: read OrderQueue for Attack orders, check range, apply damage, spawn projectiles
}

void system_ability(World& world, float dt) {
    // Tick cooldowns
    for (auto& aset : world.ability_sets.data()) {
        for (auto& ability : aset.abilities) {
            if (ability.cooldown_remaining > 0) {
                ability.cooldown_remaining = std::max(0.0f, ability.cooldown_remaining - dt);
            }
        }
    }
}

void system_buff(World& world, float dt) {
    // Tick buff durations, remove expired
    for (auto& blist : world.buff_lists.data()) {
        std::erase_if(blist.active, [dt](BuffInstance& buff) {
            if (buff.remaining_duration <= 0) return false; // permanent
            buff.remaining_duration -= dt;
            return buff.remaining_duration <= 0;
        });
    }
}

void system_projectile(World& world, float /*dt*/) {
    if (s_first_tick) {
        log::trace("Systems", "projectile (stub) — movement, hit detection pending");
        s_first_tick = false;
    }
    // TODO: move projectiles toward target, check hit, apply damage, destroy on hit
}

} // namespace uldum::simulation
