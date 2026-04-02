#include "simulation/systems.h"
#include "simulation/world.h"
#include "simulation/ability_def.h"
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

        // Only process Move orders (Attack movement is handled by CombatSystem)
        if (!oq->current) {
            mov.moving = false;
            continue;
        }

        auto* move_order = std::get_if<orders::Move>(&oq->current->payload);
        if (!move_order) {
            // Don't clear moving flag — CombatSystem may be controlling this unit
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

// ── Helper: deal damage to a target ───────────────────────────────────────

static void deal_damage(World& world, Unit source, Unit target, f32 amount) {
    auto* hp = world.healths.get(target.id);
    if (!hp || hp->current <= 0) return;

    hp->current -= amount;
    if (hp->current < 0) hp->current = 0;

    // Scale pulse on attacker (visual feedback)
    auto* pulse = world.scale_pulses.get(source.id);
    if (pulse) {
        pulse->current_scale = 1.3f;
        pulse->timer = 0.15f;
    }

    auto* src_info = world.handle_infos.get(source.id);
    auto* tgt_info = world.handle_infos.get(target.id);
    log::trace("Combat", "{} deals {:.0f} damage to {} (HP: {:.0f}/{:.0f})",
               src_info ? src_info->type_id : "?", amount,
               tgt_info ? tgt_info->type_id : "?",
               hp->current, hp->max);
}

// ── Helper: spawn projectile ──────────────────────────────────────────────

static void spawn_projectile(World& world, Unit source, Unit target, f32 damage, f32 speed) {
    auto* src_t = world.transforms.get(source.id);
    if (!src_t) return;

    Handle h = world.handles.allocate();
    u32 id = h.id;

    world.transforms.add(id, Transform{src_t->position, 0, 0.3f});
    world.handle_infos.add(id, HandleInfo{"projectile", Category::Projectile, h.generation});
    world.projectiles.add(id, ProjectileComp{source, target, {}, speed, damage, "", true});
    world.renderables.add(id, Renderable{"projectile", true});
}

// ── Combat system ─────────────────────────────────────────────────────────

void system_combat(World& world, float dt, const Pathfinder& pathfinder) {
    for (u32 i = 0; i < world.combats.count(); ++i) {
        u32 id = world.combats.ids()[i];
        auto& combat = world.combats.data()[i];

        auto* transform = world.transforms.get(id);
        auto* oq = world.order_queues.get(id);
        if (!transform || !oq) continue;

        // Check for Attack order
        if (!oq->current) {
            combat.attack_state = AttackState::Idle;
            continue;
        }

        auto* attack_order = std::get_if<orders::Attack>(&oq->current->payload);
        if (!attack_order) {
            if (combat.attack_state != AttackState::Idle) {
                combat.attack_state = AttackState::Idle;
            }
            continue;
        }


        Unit target = attack_order->target;

        // Validate target is alive
        auto* target_hp = world.healths.get(target.id);
        if (!world.validate(target) || !target_hp || target_hp->current <= 0) {
            oq->current.reset();
            combat.attack_state = AttackState::Idle;
            if (!oq->queued.empty()) {
                oq->current = std::move(oq->queued.front());
                oq->queued.pop_front();
            }
            continue;
        }

        auto* target_t = world.transforms.get(target.id);
        if (!target_t) continue;

        glm::vec3 to_target = target_t->position - transform->position;
        to_target.z = 0;
        f32 dist = glm::length(to_target);

        switch (combat.attack_state) {
        case AttackState::Idle:
        case AttackState::MovingToTarget: {
            if (dist > combat.range) {
                // Chase target directly (simple steering, no pathfinding)
                combat.attack_state = AttackState::MovingToTarget;
                auto* mov = world.movements.get(id);
                if (mov && mov->speed > 0) {
                    // Turn toward target
                    f32 desired = std::atan2(-to_target.x, to_target.y);
                    f32 diff = angle_diff(transform->facing, desired);
                    f32 max_turn = mov->turn_rate * dt;
                    if (std::abs(diff) > max_turn) {
                        transform->facing += (diff > 0 ? max_turn : -max_turn);
                    } else {
                        transform->facing = desired;
                    }
                    transform->facing = normalize_angle(transform->facing);

                    // Move toward target
                    if (std::abs(diff) < glm::half_pi<f32>()) {
                        f32 step = mov->speed * dt;
                        if (step > dist) step = dist;
                        glm::vec3 dir = glm::normalize(to_target);
                        transform->position.x += dir.x * step;
                        transform->position.y += dir.y * step;
                        transform->position.z = pathfinder.sample_height(
                            transform->position.x, transform->position.y);
                    }
                }
            } else {
                // In range — start turning to face
                combat.attack_state = AttackState::TurningToFace;
            }
            break;
        }

        case AttackState::TurningToFace: {
            if (dist > combat.range * 1.2f) {
                combat.attack_state = AttackState::MovingToTarget;
                break;
            }
            f32 desired = std::atan2(-to_target.x, to_target.y);
            f32 diff = angle_diff(transform->facing, desired);
            auto* mov = world.movements.get(id);
            f32 turn_rate = mov ? mov->turn_rate : 3.0f;
            f32 max_turn = turn_rate * dt;

            if (std::abs(diff) > max_turn) {
                transform->facing += (diff > 0 ? max_turn : -max_turn);
                transform->facing = normalize_angle(transform->facing);
            } else {
                transform->facing = desired;
                combat.attack_state = AttackState::WindUp;
                combat.attack_timer = combat.cast_point;
            }
            break;
        }

        case AttackState::WindUp:
            combat.attack_timer -= dt;
            if (combat.attack_timer <= 0) {
                // FIRE — deal damage or spawn projectile
                Unit self;
                self.id = id;
                self.generation = world.handle_infos.get(id)->generation;

                if (combat.is_ranged) {
                    spawn_projectile(world, self, target, combat.damage, combat.projectile_speed);
                } else {
                    deal_damage(world, self, target, combat.damage);
                }
                combat.attack_state = AttackState::Backswing;
                combat.attack_timer = combat.backswing;
            }
            break;

        case AttackState::Backswing:
            combat.attack_timer -= dt;
            if (combat.attack_timer <= 0) {
                combat.attack_state = AttackState::Cooldown;
                combat.attack_timer = combat.attack_cooldown - combat.cast_point - combat.backswing;
                if (combat.attack_timer < 0) combat.attack_timer = 0;
            }
            break;

        case AttackState::Cooldown:
            combat.attack_timer -= dt;
            if (combat.attack_timer <= 0) {
                combat.attack_state = AttackState::Idle;  // loops back to check range
            }
            break;
        }
    }
}

// ── Aura scan counter (uniform interval for all auras) ────────────────────
static u32 s_aura_tick_counter = 0;
static constexpr u32 AURA_SCAN_INTERVAL = 8;  // every 8 ticks = 0.25s at 32Hz

void system_ability(World& world, float dt, const AbilityRegistry& abilities, const SpatialGrid& grid) {
    bool aura_scan_tick = (++s_aura_tick_counter % AURA_SCAN_INTERVAL == 0);

    for (u32 i = 0; i < world.ability_sets.count(); ++i) {
        u32 id = world.ability_sets.ids()[i];
        auto& aset = world.ability_sets.data()[i];

        // Tick cooldowns
        for (auto& ability : aset.abilities) {
            if (ability.cooldown_remaining > 0) {
                ability.cooldown_remaining = std::max(0.0f, ability.cooldown_remaining - dt);
            }
        }

        // Tick durations — remove expired applied abilities
        bool modifiers_changed = false;
        std::erase_if(aset.abilities, [dt, &modifiers_changed](AbilityInstance& a) {
            if (a.remaining_duration < 0) return false;  // permanent
            a.remaining_duration -= dt;
            if (a.remaining_duration <= 0) {
                modifiers_changed = true;
                return true;
            }
            return false;
        });

        if (modifiers_changed) {
            recalculate_modifiers(world, id);
        }

        // Aura scanning (only on scan ticks)
        if (aura_scan_tick) {
            auto* transform = world.transforms.get(id);
            auto* owner = world.owners.get(id);
            if (!transform || !owner) continue;

            for (auto& ability : aset.abilities) {
                const auto* def = abilities.get(ability.ability_id);
                if (!def || def->form != AbilityForm::Aura) continue;

                auto& lvl = def->level_data(ability.level);
                if (lvl.aura_radius <= 0 || lvl.aura_ability.empty()) continue;

                // Find units in aura radius
                UnitFilter filter;
                if (def->target_filter.ally)  filter.owner = owner->player;
                if (def->target_filter.enemy) filter.enemy_of = owner->player;
                auto nearby = grid.units_in_range(world, transform->position, lvl.aura_radius, filter);

                Unit source_unit;
                source_unit.id = id;
                auto* info = world.handle_infos.get(id);
                source_unit.generation = info ? info->generation : 0;

                // Apply the aura's passive ability to each nearby unit
                f32 aura_duration = (AURA_SCAN_INTERVAL + 2) * (1.0f / 32.0f);  // slightly longer than scan interval
                for (auto& target : nearby) {
                    if (target.id == id && !def->target_filter.self_) continue;
                    apply_passive_ability(world, abilities, target, lvl.aura_ability, source_unit, aura_duration);
                }
            }
        }
    }
}

// ── Projectile system ─────────────────────────────────────────────────────

void system_projectile(World& world, float dt, const Pathfinder& pathfinder) {
    // Collect IDs to destroy after iteration (can't modify sparse set during iteration)
    std::vector<u32> to_destroy;

    for (u32 i = 0; i < world.projectiles.count(); ++i) {
        u32 id = world.projectiles.ids()[i];
        auto& proj = world.projectiles.data()[i];
        auto* transform = world.transforms.get(id);
        if (!transform) continue;

        // Get target position
        glm::vec3 target_pos;
        if (proj.homing && world.validate(proj.target)) {
            auto* target_t = world.transforms.get(proj.target.id);
            if (target_t) {
                target_pos = target_t->position;
                target_pos.z += 1.0f;  // aim at center of unit, not feet
            } else {
                to_destroy.push_back(id);
                continue;
            }
        } else {
            target_pos = proj.target_pos;
        }

        glm::vec3 to_target = target_pos - transform->position;
        f32 dist = glm::length(to_target);

        if (dist < 0.5f) {
            // Hit — deal damage
            if (world.validate(proj.target)) {
                deal_damage(world, proj.source, proj.target, proj.damage);
            }
            to_destroy.push_back(id);
            continue;
        }

        // Move toward target
        glm::vec3 dir = to_target / dist;
        f32 step = proj.speed * dt;
        if (step > dist) step = dist;
        transform->position += dir * step;
    }

    // Destroy hit/expired projectiles
    for (u32 id : to_destroy) {
        auto* info = world.handle_infos.get(id);
        if (info) {
            Handle h;
            h.id = id;
            h.generation = info->generation;
            world.transforms.remove(id);
            world.handle_infos.remove(id);
            world.projectiles.remove(id);
            world.renderables.remove(id);
            world.handles.free(h);
        }
    }
}

// ── Death + corpse system ─────────────────────────────────────────────────

static void remove_all_components_and_free(World& world, Handle h) {
    world.transforms.remove(h.id);
    world.handle_infos.remove(h.id);
    world.healths.remove(h.id);
    world.state_blocks.remove(h.id);
    world.attribute_blocks.remove(h.id);
    world.selectables.remove(h.id);
    world.owners.remove(h.id);
    world.movements.remove(h.id);
    world.combats.remove(h.id);
    world.visions.remove(h.id);
    world.order_queues.remove(h.id);
    world.ability_sets.remove(h.id);
    world.classifications.remove(h.id);
    world.heroes.remove(h.id);
    world.inventories.remove(h.id);
    world.buildings.remove(h.id);
    world.constructions.remove(h.id);
    world.destructables.remove(h.id);
    world.pathing_blockers.remove(h.id);
    world.item_infos.remove(h.id);
    world.carriables.remove(h.id);
    world.projectiles.remove(h.id);
    world.scale_pulses.remove(h.id);
    world.dead_states.remove(h.id);
    world.renderables.remove(h.id);
    world.handles.free(h);
}

void system_death(World& world) {
    // Phase 1: transition newly dead units to corpse state
    for (u32 i = 0; i < world.healths.count(); ++i) {
        u32 id = world.healths.ids()[i];
        auto& hp = world.healths.data()[i];

        // Units below this HP threshold are considered dead (avoids floating point edge cases)
        static constexpr f32 DEATH_THRESHOLD = 0.05f;
        if (hp.current < DEATH_THRESHOLD && hp.max > 0 && !world.dead_states.has(id)) {
            hp.current = 0;
            auto* info = world.handle_infos.get(id);
            if (!info) continue;
            if (info->category == Category::Item) continue;

            log::info("Combat", "{} has died (id={})", info->type_id, id);

            // Become a corpse: stop all gameplay activity
            world.movements.remove(id);
            world.combats.remove(id);
            world.order_queues.remove(id);
            world.ability_sets.remove(id);
            world.visions.remove(id);

            // Add dead state — corpse visible for duration, then cleaned up
            world.dead_states.add(id, DeadState{});
        }
    }

    // Phase 2: tick corpse timers, hide then destroy
    std::vector<Handle> to_destroy;

    for (u32 i = 0; i < world.dead_states.count(); ++i) {
        u32 id = world.dead_states.ids()[i];
        auto& dead = world.dead_states.data()[i];

        dead.corpse_timer += 1.0f / 32.0f;  // approximate dt (called once per tick)

        // Hide corpse after corpse_duration
        if (dead.corpse_visible && dead.corpse_timer >= dead.corpse_duration) {
            dead.corpse_visible = false;
            auto* r = world.renderables.get(id);
            if (r) r->visible = false;
            log::trace("Combat", "Corpse hidden (id={})", id);
        }

        // Fully destroy after cleanup_delay
        if (dead.corpse_timer >= dead.cleanup_delay) {
            auto* info = world.handle_infos.get(id);
            if (info) {
                Handle h;
                h.id = id;
                h.generation = info->generation;
                to_destroy.push_back(h);
            }
        }
    }

    for (auto h : to_destroy) {
        log::trace("Combat", "Entity cleaned up (id={})", h.id);
        remove_all_components_and_free(world, h);
    }
}

// ── Scale pulse system (visual feedback) ──────────────────────────────────

void system_scale_pulse(World& world, float dt) {
    for (u32 i = 0; i < world.scale_pulses.count(); ++i) {
        u32 id = world.scale_pulses.ids()[i];
        auto& pulse = world.scale_pulses.data()[i];

        if (pulse.timer > 0) {
            pulse.timer -= dt;
            if (pulse.timer <= 0) {
                pulse.current_scale = 1.0f;
            }
        }

        // Apply scale to transform
        auto* transform = world.transforms.get(id);
        if (transform && pulse.current_scale != 1.0f) {
            // Lerp back toward 1.0
            pulse.current_scale += (1.0f - pulse.current_scale) * dt * 10.0f;
            if (std::abs(pulse.current_scale - 1.0f) < 0.01f) {
                pulse.current_scale = 1.0f;
            }
        }

        // Shrink unit based on HP, or flatten if dead (corpse)
        auto* hp = world.healths.get(id);
        if (hp && hp->max > 0 && transform) {
            if (world.dead_states.has(id)) {
                // Corpse: flatten to 0.3 scale
                transform->scale = 0.3f;
            } else {
                f32 hp_fraction = hp->current / hp->max;
                f32 hp_scale = 0.5f + 0.5f * hp_fraction;
                transform->scale = hp_scale * pulse.current_scale;
            }
        }
    }
}

} // namespace uldum::simulation
