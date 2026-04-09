#include "simulation/systems.h"
#include "simulation/world.h"
#include "simulation/type_registry.h"
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

// Helper: find the closest unit blocking the path ahead.
// Returns the blocker's id (UINT32_MAX if clear).
static u32 find_blocker_id(World& world, const SpatialGrid& grid, u32 self_id,
                            glm::vec3 pos, glm::vec3 dir, f32 self_radius, f32 look_ahead) {
    f32 check_radius = self_radius * 2.0f + look_ahead;
    UnitFilter filter;
    filter.exclude_buildings = true;
    auto nearby = grid.units_in_range(world, pos, check_radius, filter);

    u32 best_id = UINT32_MAX;
    f32 best_dist = check_radius;

    for (auto& other : nearby) {
        if (other.id == self_id) continue;
        if (world.dead_states.has(other.id)) continue;

        auto* ot = world.transforms.get(other.id);
        if (!ot) continue;
        auto* om = world.movements.get(other.id);
        f32 other_radius = om ? om->collision_radius : 16.0f;
        f32 combined = self_radius + other_radius;

        glm::vec3 to_other = ot->position - pos;
        to_other.z = 0;
        f32 d = glm::length(to_other);
        if (d < 0.01f || d > check_radius) continue;

        // Is it ahead of us?
        f32 dot = glm::dot(to_other / d, dir);
        if (dot < 0.3f) continue;

        // Is it close enough laterally to block?
        f32 forward_dist = d * dot;
        f32 lateral = std::sqrt(std::max(0.0f, d * d - forward_dist * forward_dist));
        if (lateral < combined && d < best_dist) {
            best_id = other.id;
            best_dist = d;
        }
    }
    return best_id;
}

// Helper: compute avoidance direction — steer to the committed side of the blocker.
static glm::vec3 compute_avoidance_dir(glm::vec3 pos, glm::vec3 forward, Transform* blocker_t, i8 side) {
    // Perpendicular to forward direction
    glm::vec3 perp = (side > 0) ? glm::vec3{forward.y, -forward.x, 0}
                                 : glm::vec3{-forward.y, forward.x, 0};
    // Blend: mostly sideways, some forward to keep progressing
    return glm::normalize(perp * 0.6f + forward * 0.4f);
}

void system_movement(World& world, float dt, const Pathfinder& pathfinder, const SpatialGrid& grid) {

    for (u32 i = 0; i < world.movements.count(); ++i) {
        u32 id = world.movements.ids()[i];
        auto& mov = world.movements.data()[i];

        auto* transform = world.transforms.get(id);
        auto* oq = world.order_queues.get(id);
        if (!transform || !oq) continue;

        // Only process Move / AttackMove orders
        if (!oq->current) {
            mov.moving = false;
            mov.avoid_id = UINT32_MAX;
            mov.avoid_side = 0;
            continue;
        }

        // If AttackMove and combat has an active target, pause path-following
        // (combat system handles the chase). Resume when target is cleared.
        auto* combat = world.combats.get(id);
        if (combat && combat->target.is_valid()) {
            if (auto* am = std::get_if<orders::AttackMove>(&oq->current->payload)) {
                // Keep path alive but don't advance — combat is in control
                continue;
            }
        }

        glm::vec3 move_target{0.0f};
        bool has_move = false;
        if (auto* m = std::get_if<orders::Move>(&oq->current->payload)) {
            move_target = m->target; has_move = true;
        } else if (auto* am = std::get_if<orders::AttackMove>(&oq->current->payload)) {
            move_target = am->target; has_move = true;
        }
        if (!has_move) continue;

        // Compute path if we don't have one yet
        if (mov.path.empty()) {
            auto result = pathfinder.find_path(transform->position, move_target, mov.type);
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
            mov.avoid_id = UINT32_MAX;
            mov.avoid_side = 0;
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

        if (dist < 16.0f) {
            mov.path_index++;
            continue;
        }

        glm::vec3 forward = glm::normalize(to_wp);

        // ── Avoidance: detect blocker, commit to a side, steer until clear ──
        glm::vec3 dir = forward;

        // Tick avoidance lock
        if (mov.avoid_lock > 0) mov.avoid_lock -= dt;

        // Check if we're currently avoiding someone
        if (mov.avoid_id != UINT32_MAX) {
            auto* blocker_t = world.transforms.get(mov.avoid_id);
            if (blocker_t && !world.dead_states.has(mov.avoid_id)) {
                glm::vec3 to_blocker = blocker_t->position - transform->position;
                to_blocker.z = 0;
                f32 bd = glm::length(to_blocker);
                f32 dot = (bd > 0.01f) ? glm::dot(to_blocker / bd, forward) : 0;

                if (dot > 0.1f && bd < mov.collision_radius * 4.0f) {
                    // Still blocking — keep steering around
                    dir = compute_avoidance_dir(transform->position, forward, blocker_t, mov.avoid_side);
                } else {
                    // Cleared the blocker
                    mov.avoid_id = UINT32_MAX;
                    // Keep avoid_side and lock — reuse same side for next blocker
                }
            } else {
                mov.avoid_id = UINT32_MAX;
            }
        }

        // If not currently avoiding, check for new blockers
        if (mov.avoid_id == UINT32_MAX) {
            f32 look_ahead = mov.speed * 0.4f;
            u32 blocker_id = find_blocker_id(world, grid, id, transform->position, forward, mov.collision_radius, look_ahead);
            if (blocker_id != UINT32_MAX) {
                mov.avoid_id = blocker_id;

                // If lock is active, reuse the same side. Otherwise pick a new one.
                if (mov.avoid_lock <= 0) {
                    auto* blocker_t = world.transforms.get(blocker_id);
                    if (blocker_t) {
                        glm::vec3 to_blocker = blocker_t->position - transform->position;
                        to_blocker.z = 0;
                        glm::vec3 left{-forward.y, forward.x, 0};
                        f32 side_dot = glm::dot(to_blocker, left);
                        mov.avoid_side = (side_dot > 0) ? 1 : -1;
                    }
                }
                mov.avoid_lock = 0.5f;  // lock this side for 0.5 seconds
                dir = compute_avoidance_dir(transform->position, forward,
                    world.transforms.get(blocker_id), mov.avoid_side);
            } else {
                // No blocker — clear side once lock expires
                if (mov.avoid_lock <= 0) {
                    mov.avoid_side = 0;
                }
            }
        }

        // If avoidance direction would cross a cliff, ignore avoidance
        if (dir != forward) {
            f32 test_step = mov.speed * dt;
            f32 test_x = transform->position.x + dir.x * test_step;
            f32 test_y = transform->position.y + dir.y * test_step;
            if (!pathfinder.can_move_to(transform->position.x, transform->position.y, test_x, test_y, mov.cliff_level, mov.type)) {
                dir = forward;
                mov.avoid_id = UINT32_MAX;
            }
        }

        // Turn toward movement direction
        f32 desired_facing = std::atan2(dir.y, dir.x);
        f32 face_diff = angle_diff(transform->facing, desired_facing);
        f32 max_turn = mov.turn_rate * dt;

        if (std::abs(face_diff) > max_turn) {
            transform->facing += (face_diff > 0 ? max_turn : -max_turn);
        } else {
            transform->facing = desired_facing;
        }
        transform->facing = normalize_angle(transform->facing);

        // Step forward
        if (std::abs(face_diff) < glm::half_pi<f32>()) {
            f32 step = mov.speed * dt;
            if (step > dist) step = dist;
            f32 new_x = transform->position.x + dir.x * step;
            f32 new_y = transform->position.y + dir.y * step;

            // Tile-based cliff check using unit's cliff level
            if (!pathfinder.can_move_to(transform->position.x, transform->position.y, new_x, new_y, mov.cliff_level, mov.type)) {
                bool can_x = pathfinder.can_move_to(transform->position.x, transform->position.y, new_x, transform->position.y, mov.cliff_level, mov.type);
                bool can_y = pathfinder.can_move_to(transform->position.x, transform->position.y, transform->position.x, new_y, mov.cliff_level, mov.type);
                if (!can_x) new_x = transform->position.x;
                if (!can_y) new_y = transform->position.y;
                if (can_x && can_y) new_y = transform->position.y;
                mov.path.clear();
            }

            transform->position.x = new_x;
            transform->position.y = new_y;
        }

        // Update Z and cliff level
        transform->position.z = pathfinder.sample_height(transform->position.x, transform->position.y);
        mov.cliff_level = pathfinder.cliff_level_at(transform->position.x, transform->position.y);
        mov.moving = true;
    }
}

// ── Helper: deal attack damage ────────────────────────────────────────────
// Uses the unified deal_damage() from world.cpp which fires the on_damage callback.

static void deal_attack_damage(World& world, Unit source, Unit target, f32 amount) {
    auto* src_info = world.handle_infos.get(source.id);
    auto* tgt_info = world.handle_infos.get(target.id);

    deal_damage(world, source, target, amount, "attack");

    auto* hp = world.healths.get(target.id);
    log::trace("Combat", "{} deals {:.0f} damage to {} (HP: {:.0f}/{:.0f})",
               src_info ? src_info->type_id : "?", amount,
               tgt_info ? tgt_info->type_id : "?",
               hp ? hp->current : 0, hp ? hp->max : 0);
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

void system_combat(World& world, float dt, const Pathfinder& pathfinder, const SpatialGrid& grid) {
    for (u32 i = 0; i < world.combats.count(); ++i) {
        u32 id = world.combats.ids()[i];
        auto& combat = world.combats.data()[i];

        auto* transform = world.transforms.get(id);
        auto* oq = world.order_queues.get(id);
        if (!transform || !oq) continue;

        // Determine attack target: explicit Attack order, or auto-acquired during AttackMove/idle
        auto* attack_order = oq->current ? std::get_if<orders::Attack>(&oq->current->payload) : nullptr;
        bool is_attack_move = oq->current && std::get_if<orders::AttackMove>(&oq->current->payload);
        bool is_casting = oq->current && std::get_if<orders::Cast>(&oq->current->payload);
        bool is_moving  = oq->current && std::get_if<orders::Move>(&oq->current->payload);

        // Get current target: from Attack order, or from combat.target (auto-acquired)
        Unit target;
        if (attack_order) {
            target = attack_order->target;
        } else {
            target = combat.target;
        }

        // Validate target is alive
        bool target_valid = target.is_valid() && world.validate(target);
        if (target_valid) {
            auto* target_hp = world.healths.get(target.id);
            if (!target_hp || target_hp->current <= 0) target_valid = false;
        }

        if (!target_valid) {
            // Target dead or gone — let backswing/cooldown finish before clearing
            combat.target = Unit{};
            if (combat.attack_state != AttackState::Idle &&
                combat.attack_state != AttackState::Backswing &&
                combat.attack_state != AttackState::Cooldown) {
                combat.attack_state = AttackState::Idle;
            }

            if (attack_order) {
                // Explicit Attack order finished — advance queue
                oq->current.reset();
                if (!oq->queued.empty()) {
                    oq->current = std::move(oq->queued.front());
                    oq->queued.pop_front();
                }
            }
            // If AttackMove: order stays, unit resumes marching (movement system handles it)

            // Auto-acquire: scan for nearby enemies
            if (!is_casting && !is_moving && combat.acquire_range > 0 && combat.damage > 0) {
                auto* owner = world.owners.get(id);
                if (owner) {
                    UnitFilter filter;
                    filter.enemy_of = owner->player;
                    filter.alive_only = true;
                    auto enemies = grid.units_in_range(world, transform->position, combat.acquire_range, filter);
                    Unit best;
                    f32 best_dist = combat.acquire_range + 1;
                    for (auto& e : enemies) {
                        auto* et = world.transforms.get(e.id);
                        if (!et) continue;
                        f32 d = glm::length(et->position - transform->position);
                        if (d < best_dist) { best = e; best_dist = d; }
                    }
                    if (best.is_valid()) {
                        if (is_attack_move) {
                            // Keep AttackMove order, store target on combat component
                            combat.target = best;
                            target = best;
                            target_valid = true;
                        } else {
                            // Idle: create an Attack order
                            Order order;
                            order.payload = orders::Attack{best};
                            oq->current = std::move(order);
                            combat.target = best;
                            target = best;
                            target_valid = true;
                        }
                    }
                }
            }

            // No valid target — but let backswing/cooldown finish
            if (!target_valid) {
                if (combat.attack_state == AttackState::Backswing) {
                    combat.attack_timer -= dt;
                    if (combat.attack_timer <= 0) {
                        combat.attack_state = AttackState::Cooldown;
                        combat.attack_timer = combat.attack_cooldown - combat.dmg_time - combat.backsw_time;
                        if (combat.attack_timer < 0) combat.attack_timer = 0;
                    }
                } else if (combat.attack_state == AttackState::Cooldown) {
                    combat.attack_timer -= dt;
                    if (combat.attack_timer <= 0) {
                        combat.attack_state = AttackState::Idle;
                    }
                }
                continue;
            }
        }

        combat.target = target;

        auto* target_t = world.transforms.get(target.id);
        if (!target_t) continue;

        glm::vec3 to_target = target_t->position - transform->position;
        to_target.z = 0;
        f32 dist = glm::length(to_target);

        switch (combat.attack_state) {
        case AttackState::Idle:
        case AttackState::MovingToTarget: {
            if (dist > combat.range) {
                combat.attack_state = AttackState::MovingToTarget;
                auto* mov = world.movements.get(id);
                if (!mov || mov->speed <= 0) break;

                // Pathfind to target (re-path when target moves > 1 tile)
                glm::vec3 target_pos = target_t->position;
                f32 path_dest_dist = glm::length(glm::vec2(target_pos - combat.chase_path_dest));
                if (mov->path.empty() || path_dest_dist > pathfinder.tile_size()) {
                    auto result = pathfinder.find_path(transform->position, target_pos, mov->type);
                    if (result.valid && result.waypoints.size() > 1) {
                        mov->path = std::move(result.waypoints);
                        mov->path_index = 1;
                    } else {
                        mov->path.clear();
                    }
                    combat.chase_path_dest = target_pos;
                }

                // Advance waypoint when close
                if (!mov->path.empty() && mov->path_index < mov->path.size()) {
                    glm::vec3 to_wp = mov->path[mov->path_index] - transform->position;
                    to_wp.z = 0;
                    if (glm::length(to_wp) < pathfinder.tile_size() * 0.3f) {
                        mov->path_index++;
                        if (mov->path_index >= mov->path.size()) mov->path.clear();
                    }
                }

                // Move toward current waypoint, or target if no path
                glm::vec3 move_goal = (!mov->path.empty() && mov->path_index < mov->path.size())
                    ? mov->path[mov->path_index] : target_pos;
                glm::vec3 to_goal = move_goal - transform->position;
                to_goal.z = 0;
                f32 goal_dist = glm::length(to_goal);
                if (goal_dist < 0.1f) break;

                glm::vec3 forward = to_goal / goal_dist;
                glm::vec3 dir = forward;

                // Avoidance steering (validated against cliffs below)
                if (mov->avoid_lock > 0) mov->avoid_lock -= dt;
                if (mov->avoid_id != UINT32_MAX) {
                    auto* bt = world.transforms.get(mov->avoid_id);
                    if (bt && !world.dead_states.has(mov->avoid_id) && mov->avoid_id != target.id) {
                        glm::vec3 tb = bt->position - transform->position;
                        tb.z = 0;
                        f32 bd = glm::length(tb);
                        f32 dot_val = (bd > 0.01f) ? glm::dot(tb / bd, forward) : 0;
                        if (dot_val > 0.1f && bd < mov->collision_radius * 4.0f) {
                            dir = compute_avoidance_dir(transform->position, forward, bt, mov->avoid_side);
                        } else {
                            mov->avoid_id = UINT32_MAX;
                        }
                    } else {
                        mov->avoid_id = UINT32_MAX;
                    }
                }
                if (mov->avoid_id == UINT32_MAX) {
                    u32 blocker = find_blocker_id(world, grid, id, transform->position, forward, mov->collision_radius, mov->speed * 0.4f);
                    if (blocker != UINT32_MAX && blocker != target.id) {
                        mov->avoid_id = blocker;
                        if (mov->avoid_lock <= 0) {
                            auto* bt = world.transforms.get(blocker);
                            if (bt) {
                                glm::vec3 tb = bt->position - transform->position;
                                tb.z = 0;
                                glm::vec3 left{-forward.y, forward.x, 0};
                                mov->avoid_side = (glm::dot(tb, left) > 0) ? 1 : -1;
                            }
                        }
                        mov->avoid_lock = 0.5f;
                        dir = compute_avoidance_dir(transform->position, forward,
                            world.transforms.get(blocker), mov->avoid_side);
                    } else if (mov->avoid_lock <= 0) {
                        mov->avoid_side = 0;
                    }
                }

                // If avoidance direction would cross a cliff, ignore avoidance
                if (dir != forward) {
                    f32 test_step = mov->speed * dt;
                    f32 test_x = transform->position.x + dir.x * test_step;
                    f32 test_y = transform->position.y + dir.y * test_step;
                    if (!pathfinder.can_move_to(transform->position.x, transform->position.y, test_x, test_y, mov->cliff_level, mov->type)) {
                        dir = forward;
                        mov->avoid_id = UINT32_MAX;
                    }
                }

                // Turn + step
                f32 desired = std::atan2(dir.y, dir.x);
                f32 face_diff = angle_diff(transform->facing, desired);
                f32 max_turn = mov->turn_rate * dt;
                if (std::abs(face_diff) > max_turn) {
                    transform->facing += (face_diff > 0 ? max_turn : -max_turn);
                } else {
                    transform->facing = desired;
                }
                transform->facing = normalize_angle(transform->facing);

                if (std::abs(face_diff) < glm::half_pi<f32>()) {
                    f32 step = mov->speed * dt;
                    if (step > goal_dist) step = goal_dist;
                    f32 new_x = transform->position.x + dir.x * step;
                    f32 new_y = transform->position.y + dir.y * step;

                    // Tile-based cliff check using unit's cliff level
                    if (!pathfinder.can_move_to(transform->position.x, transform->position.y, new_x, new_y, mov->cliff_level, mov->type)) {
                        bool can_x = pathfinder.can_move_to(transform->position.x, transform->position.y, new_x, transform->position.y, mov->cliff_level, mov->type);
                        bool can_y = pathfinder.can_move_to(transform->position.x, transform->position.y, transform->position.x, new_y, mov->cliff_level, mov->type);
                        if (!can_x) new_x = transform->position.x;
                        if (!can_y) new_y = transform->position.y;
                        if (can_x && can_y) new_y = transform->position.y;
                        mov->path.clear();
                    }

                    transform->position.x = new_x;
                    transform->position.y = new_y;
                    transform->position.z = pathfinder.sample_height(new_x, new_y);
                    mov->cliff_level = pathfinder.cliff_level_at(new_x, new_y);
                }
            } else {
                // In range — clear chase path, begin attack
                auto* mov = world.movements.get(id);
                if (mov) mov->path.clear();
                combat.attack_state = AttackState::TurningToFace;
            }
            break;
        }

        case AttackState::TurningToFace: {
            if (dist > combat.range * 1.2f) {
                combat.attack_state = AttackState::MovingToTarget;
                break;
            }
            f32 desired = std::atan2(to_target.y, to_target.x);
            f32 diff = angle_diff(transform->facing, desired);
            auto* mov = world.movements.get(id);
            f32 turn_rate = mov ? mov->turn_rate : 3.0f;
            f32 max_turn = turn_rate * dt;

            constexpr f32 ATTACK_FACING_TOLERANCE = 0.1745f;  // ~10 degrees
            if (std::abs(diff) > max_turn) {
                transform->facing += (diff > 0 ? max_turn : -max_turn);
                transform->facing = normalize_angle(transform->facing);
            } else {
                transform->facing = desired;
            }
            if (std::abs(angle_diff(transform->facing, desired)) <= ATTACK_FACING_TOLERANCE) {
                combat.attack_state = AttackState::WindUp;
                combat.attack_timer = combat.dmg_time;
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
                    deal_attack_damage(world, self, target, combat.damage);
                }

                // Play attack sound
                if (world.on_sound) {
                    auto* info = world.handle_infos.get(id);
                    auto* def = info ? world.types->get_unit_type(info->type_id) : nullptr;
                    if (def && !def->sound_attack.empty()) {
                        world.on_sound(def->sound_attack, transform->position);
                    }
                }
                combat.attack_state = AttackState::Backswing;
                combat.attack_timer = combat.backsw_time;
            }
            break;

        case AttackState::Backswing:
            combat.attack_timer -= dt;
            if (combat.attack_timer <= 0) {
                combat.attack_state = AttackState::Cooldown;
                combat.attack_timer = combat.attack_cooldown - combat.dmg_time - combat.backsw_time;
                if (combat.attack_timer < 0) combat.attack_timer = 0;
            }
            break;

        case AttackState::Cooldown:
            combat.attack_timer -= dt;
            if (combat.attack_timer <= 0) {
                // If target still valid and in range, go straight to next swing
                if (target_valid && dist <= combat.range) {
                    combat.attack_state = AttackState::TurningToFace;
                } else {
                    combat.attack_state = AttackState::Idle;
                }
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

        // ── Cast order processing ────────────────────────────────────────
        auto* oq = world.order_queues.get(id);
        auto* transform = world.transforms.get(id);
        if (oq && transform && oq->current) {
            auto* cast_order = std::get_if<orders::Cast>(&oq->current->payload);
            if (cast_order && aset.cast_state == CastState::None) {
                // Start a new cast
                const auto* def = abilities.get(cast_order->ability_id);
                if (def) {
                    // Find the ability instance to check cooldown and level
                    AbilityInstance* inst = nullptr;
                    for (auto& a : aset.abilities) {
                        if (a.ability_id == cast_order->ability_id) { inst = &a; break; }
                    }
                    if (inst && inst->cooldown_remaining <= 0) {
                        aset.casting_id       = cast_order->ability_id;
                        aset.cast_target_unit = cast_order->target_unit;
                        aset.cast_target_pos  = cast_order->target_pos;

                        auto& lvl = def->level_data(inst->level);
                        bool needs_range = (def->form == AbilityForm::TargetUnit ||
                                           def->form == AbilityForm::TargetPoint);
                        if (needs_range && lvl.range > 0) {
                            aset.cast_state = CastState::MovingToTarget;
                        } else {
                            aset.cast_state = CastState::TurningToFace;
                        }
                    } else {
                        oq->current.reset();  // ability not available or on cooldown
                    }
                } else {
                    oq->current.reset();  // unknown ability
                }
            }

            if (aset.cast_state != CastState::None) {
                const auto* def = abilities.get(aset.casting_id);
                AbilityInstance* inst = nullptr;
                for (auto& a : aset.abilities) {
                    if (a.ability_id == aset.casting_id) { inst = &a; break; }
                }
                if (!def || !inst) {
                    aset.cast_state = CastState::None;
                    oq->current.reset();
                } else {
                    auto& lvl = def->level_data(inst->level);

                    // Resolve target position
                    glm::vec3 target_pos = aset.cast_target_pos;
                    if (def->form == AbilityForm::TargetUnit && world.validate(aset.cast_target_unit)) {
                        auto* tt = world.transforms.get(aset.cast_target_unit.id);
                        if (tt) target_pos = tt->position;
                    }

                    glm::vec3 to_target = target_pos - transform->position;
                    to_target.z = 0;
                    f32 dist = glm::length(to_target);

                    switch (aset.cast_state) {
                    case CastState::MovingToTarget: {
                        if (dist <= lvl.range) {
                            aset.cast_state = CastState::TurningToFace;
                        } else {
                            // Move toward target (simple steering)
                            auto* mov = world.movements.get(id);
                            if (mov && mov->speed > 0) {
                                glm::vec3 dir = glm::normalize(to_target);
                                f32 step = mov->speed * dt;
                                transform->position.x += dir.x * step;
                                transform->position.y += dir.y * step;
                            }
                        }
                        break;
                    }
                    case CastState::TurningToFace: {
                        if (dist > 0.1f) {
                            f32 desired = std::atan2(to_target.y, to_target.x);
                            f32 diff = angle_diff(transform->facing, desired);
                            auto* mov = world.movements.get(id);
                            f32 turn_rate = mov ? mov->turn_rate : 3.0f;
                            f32 max_turn = turn_rate * dt;
                            if (std::abs(diff) > max_turn) {
                                transform->facing += (diff > 0 ? max_turn : -max_turn);
                                transform->facing = normalize_angle(transform->facing);
                                break;
                            }
                            transform->facing = desired;
                        }
                        aset.cast_state = CastState::CastPoint;
                        aset.cast_timer = lvl.cast_point;
                        aset.cast_point_secs = lvl.cast_point;
                        aset.cast_backswing_secs = lvl.backswing;
                        break;
                    }
                    case CastState::CastPoint:
                        aset.cast_timer -= dt;
                        if (aset.cast_timer <= 0) {
                            // FIRE — ability effect
                            inst->cooldown_remaining = lvl.cooldown;
                            if (world.on_ability_effect) {
                                Unit caster;
                                caster.id = id;
                                auto* info = world.handle_infos.get(id);
                                caster.generation = info ? info->generation : 0;
                                world.on_ability_effect(caster, aset.casting_id,
                                                       aset.cast_target_unit, target_pos);
                            }
                            aset.cast_state = CastState::Backswing;
                            aset.cast_timer = lvl.backswing;
                        }
                        break;
                    case CastState::Backswing:
                        aset.cast_timer -= dt;
                        if (aset.cast_timer <= 0) {
                            aset.cast_state = CastState::None;
                            aset.casting_id.clear();
                            oq->current.reset();
                        }
                        break;
                    default: break;
                    }
                }
            }
        }

        // Aura scanning (only on scan ticks)
        // Defer applications to avoid iterator invalidation (push_back on self's abilities
        // vector while iterating it would crash).
        if (aura_scan_tick) {
            struct AuraApp { Unit target; std::string ability_id; Unit source; f32 duration; };
            std::vector<AuraApp> deferred;

            auto* aura_transform = world.transforms.get(id);
            auto* owner = world.owners.get(id);
            if (!aura_transform || !owner) continue;

            for (auto& ability : aset.abilities) {
                const auto* def = abilities.get(ability.ability_id);
                if (!def || def->form != AbilityForm::Aura) continue;

                auto& lvl = def->level_data(ability.level);
                if (lvl.aura_radius <= 0 || lvl.aura_ability.empty()) continue;

                UnitFilter filter;
                if (def->target_filter.ally)  filter.owner = owner->player;
                if (def->target_filter.enemy) filter.enemy_of = owner->player;
                auto nearby = grid.units_in_range(world, aura_transform->position, lvl.aura_radius, filter);

                Unit source_unit;
                source_unit.id = id;
                auto* info = world.handle_infos.get(id);
                source_unit.generation = info ? info->generation : 0;

                f32 aura_duration = (AURA_SCAN_INTERVAL + 2) * (1.0f / 32.0f);
                for (auto& target : nearby) {
                    if (target.id == id && !def->target_filter.self_) continue;
                    deferred.push_back({target, std::string(lvl.aura_ability), source_unit, aura_duration});
                }
            }

            // Apply after iteration is complete
            for (auto& app : deferred) {
                apply_passive_ability(world, abilities, app.target, app.ability_id, app.source, app.duration);
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
                target_pos.z += 32.0f;  // aim at center of unit, not feet
            } else {
                to_destroy.push_back(id);
                continue;
            }
        } else {
            target_pos = proj.target_pos;
        }

        glm::vec3 to_target = target_pos - transform->position;
        f32 dist = glm::length(to_target);

        if (dist < 32.0f) {
            // Hit — deal damage
            if (world.validate(proj.target)) {
                deal_attack_damage(world, proj.source, proj.target, proj.damage);
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

// ── Collision system ──────────────────────────────────────────────────────
// Prevents all overlaps. Each overlapping pair is separated to the boundary.

void system_collision(World& world, const SpatialGrid& grid, const Pathfinder& pathfinder) {
    for (u32 i = 0; i < world.movements.count(); ++i) {
        u32 id = world.movements.ids()[i];
        if (world.dead_states.has(id)) continue;

        auto* transform = world.transforms.get(id);
        if (!transform) continue;
        auto* mov = world.movements.get(id);
        if (!mov) continue;
        f32 self_radius = mov->collision_radius;

        UnitFilter filter;
        filter.exclude_buildings = true;
        auto nearby = grid.units_in_range(world, transform->position, self_radius * 4.0f, filter);

        for (auto& other : nearby) {
            if (other.id <= id) continue;
            if (world.dead_states.has(other.id)) continue;

            auto* other_t = world.transforms.get(other.id);
            if (!other_t) continue;
            auto* other_mov = world.movements.get(other.id);
            f32 other_radius = other_mov ? other_mov->collision_radius : 32.0f;
            f32 min_dist = self_radius + other_radius;

            glm::vec3 diff = transform->position - other_t->position;
            diff.z = 0;
            f32 d = glm::length(diff);

            if (d < min_dist) {
                if (d > 0.01f) {
                    glm::vec3 n = diff / d;
                    f32 half = (min_dist - d) * 0.5f;
                    f32 ax = transform->position.x + n.x * half;
                    f32 ay = transform->position.y + n.y * half;
                    f32 bx = other_t->position.x - n.x * half;
                    f32 by = other_t->position.y - n.y * half;
                    // Only push if it doesn't cross a cliff
                    if (pathfinder.can_move_to(transform->position.x, transform->position.y, ax, ay, mov->cliff_level, mov->type)) {
                        transform->position.x = ax;
                        transform->position.y = ay;
                    }
                    MoveType other_type = other_mov ? other_mov->type : MoveType::Ground;
                    u8 other_cliff = other_mov ? other_mov->cliff_level : 0;
                    if (pathfinder.can_move_to(other_t->position.x, other_t->position.y, bx, by, other_cliff, other_type)) {
                        other_t->position.x = bx;
                        other_t->position.y = by;
                    }
                } else {
                    transform->position.x += self_radius;
                    other_t->position.x -= other_radius;
                }
            }
        }
    }
}

// ── Death system ─────────────────────────────────────────────────────────

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

            // Play death sound
            if (world.on_sound) {
                auto* def = world.types->get_unit_type(info->type_id);
                if (def && !def->sound_death.empty()) {
                    auto* t = world.transforms.get(id);
                    if (t) world.on_sound(def->sound_death, t->position);
                }
            }

            // Fire death callback for script engine
            if (world.on_death) {
                Unit dying;
                dying.id = id;
                dying.generation = info->generation;
                // TODO: track actual killer from last damage source
                world.on_death(dying, Unit{});
            }

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

// Scale pulse removed — replaced by skeletal animation (attack, death states).
void system_scale_pulse(World& /*world*/, float /*dt*/) {
}

} // namespace uldum::simulation
