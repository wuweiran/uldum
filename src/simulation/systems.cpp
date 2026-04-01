#include "simulation/systems.h"
#include "simulation/world.h"
#include "simulation/pathfinding.h"
#include "simulation/spatial_query.h"
#include "core/log.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace uldum::simulation {

static bool s_first_tick = true;

void system_health(World& world, float dt) {
    for (u32 i = 0; i < world.healths.count(); ++i) {
        auto& hp = world.healths.data()[i];
        if (hp.current > 0 && hp.current < hp.max && hp.regen_per_sec > 0) {
            hp.current = std::min(hp.current + hp.regen_per_sec * dt, hp.max);
        }
    }
}

void system_state(World& world, float dt) {
    for (u32 i = 0; i < world.state_blocks.count(); ++i) {
        auto& sb = world.state_blocks.data()[i];
        for (auto& [id, state] : sb.states) {
            if (state.current < state.max && state.regen_per_sec > 0) {
                state.current = std::min(state.current + state.regen_per_sec * dt, state.max);
            }
        }
    }
}

// ── Angle helpers ─────────────────────────────────────────────────────────

static f32 normalize_angle(f32 a) {
    while (a > glm::pi<f32>())  a -= glm::two_pi<f32>();
    while (a < -glm::pi<f32>()) a += glm::two_pi<f32>();
    return a;
}

static f32 angle_diff(f32 from, f32 to) {
    return normalize_angle(to - from);
}

// ── Movement system ───────────────────────────────────────────────────────

void system_movement(World& world, float dt, const Pathfinder& pathfinder, const SpatialGrid& grid) {
    static constexpr f32 SEPARATION_RADIUS = 2.5f;
    static constexpr f32 SEPARATION_FORCE  = 4.0f;

    for (u32 i = 0; i < world.movements.count(); ++i) {
        u32 id = world.movements.ids()[i];
        auto& mov = world.movements.data()[i];

        auto* transform = world.transforms.get(id);
        auto* oq = world.order_queues.get(id);
        if (!transform || !oq) continue;

        // Only process Move orders
        if (!oq->current) {
            mov.moving = false;
            continue;
        }

        auto* move_order = std::get_if<orders::Move>(&oq->current->payload);
        if (!move_order) {
            continue;
        }

        // Compute path if we don't have one yet
        if (mov.path.empty()) {
            auto result = pathfinder.find_path(transform->position, move_order->target, mov.type);
            if (result.valid && result.waypoints.size() > 1) {
                mov.path = std::move(result.waypoints);
                mov.path_index = 1;
                mov.moving = true;
            } else {
                oq->current.reset();
                mov.moving = false;
                continue;
            }
        }

        if (mov.path_index >= mov.path.size()) {
            mov.path.clear();
            mov.path_index = 0;
            mov.moving = false;
            oq->current.reset();
            if (!oq->queued.empty()) {
                oq->current = std::move(oq->queued.front());
                oq->queued.pop_front();
            }
            continue;
        }

        // Current target waypoint
        glm::vec3 wp = mov.path[mov.path_index];
        glm::vec3 to_wp = wp - transform->position;
        to_wp.z = 0;
        f32 dist = glm::length(to_wp);

        if (dist < 0.3f) {
            mov.path_index++;
            continue;
        }

        // Turn toward waypoint
        f32 desired_facing = std::atan2(-to_wp.x, to_wp.y);
        f32 diff = angle_diff(transform->facing, desired_facing);
        f32 max_turn = mov.turn_rate * dt;

        if (std::abs(diff) > max_turn) {
            transform->facing += (diff > 0 ? max_turn : -max_turn);
        } else {
            transform->facing = desired_facing;
        }
        transform->facing = normalize_angle(transform->facing);

        // Move forward (only if roughly facing the target)
        if (std::abs(diff) < glm::half_pi<f32>()) {
            f32 step = mov.speed * dt;
            if (step > dist) step = dist;

            glm::vec3 dir = glm::normalize(to_wp);
            transform->position.x += dir.x * step;
            transform->position.y += dir.y * step;
        }

        // Collision avoidance: push away from nearby units
        Unit self;
        self.id = id;
        self.generation = world.handle_infos.get(id)->generation;

        UnitFilter filter;
        filter.exclude_buildings = true;
        auto nearby = grid.units_in_range(world, transform->position, SEPARATION_RADIUS, filter);

        glm::vec3 separation{0.0f};
        for (auto& other : nearby) {
            if (other.id == id) continue;
            auto* other_t = world.transforms.get(other.id);
            if (!other_t) continue;

            glm::vec3 away = transform->position - other_t->position;
            away.z = 0;
            f32 d = glm::length(away);
            if (d > 0.01f && d < SEPARATION_RADIUS) {
                separation += (away / d) * (1.0f - d / SEPARATION_RADIUS);
            }
        }

        if (glm::length(separation) > 0.01f) {
            separation = glm::normalize(separation) * SEPARATION_FORCE * dt;
            transform->position.x += separation.x;
            transform->position.y += separation.y;
        }

        // Update Z from terrain height
        transform->position.z = pathfinder.sample_height(transform->position.x, transform->position.y);
        mov.moving = true;
    }
}

void system_combat(World& world, float /*dt*/) {
    if (s_first_tick) {
        log::trace("Systems", "combat (stub) — target acquisition, attack logic pending");
    }
}

void system_ability(World& world, float dt) {
    for (u32 i = 0; i < world.ability_sets.count(); ++i) {
        auto& aset = world.ability_sets.data()[i];
        for (auto& ability : aset.abilities) {
            if (ability.cooldown_remaining > 0) {
                ability.cooldown_remaining = std::max(0.0f, ability.cooldown_remaining - dt);
            }
        }
        std::erase_if(aset.abilities, [dt](AbilityInstance& a) {
            if (a.remaining_duration < 0) return false;
            a.remaining_duration -= dt;
            return a.remaining_duration <= 0;
        });
    }
}

void system_projectile(World& world, float /*dt*/) {
    if (s_first_tick) {
        log::trace("Systems", "projectile (stub) — movement, hit detection pending");
        s_first_tick = false;
    }
}

} // namespace uldum::simulation
