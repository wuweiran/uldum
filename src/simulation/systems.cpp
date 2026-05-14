#include "simulation/systems.h"
#include "simulation/world.h"
#include "simulation/type_registry.h"
#include "simulation/ability_def.h"
#include "simulation/pathfinding.h"
#include "simulation/spatial_query.h"
#include "map/terrain_data.h"
#include "core/log.h"

#include <glm/geometric.hpp>
#include <chrono>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace uldum::simulation {

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

// Local steering: adjust direction to avoid a nearby unit blocking the path.
// Returns adjusted direction. If no blocker, returns original forward.
// prefer_left: deterministic side preference based on unit ID + time.
static glm::vec3 local_steer(const World& world, const SpatialGrid& grid, const Pathfinder& pathfinder,
                              u32 self_id, glm::vec3 pos, glm::vec3 forward, f32 self_radius,
                              MoveType move_type, bool prefer_left) {
    f32 look_ahead = self_radius * 4.0f;
    UnitFilter filter;
    filter.exclude_buildings = true;
    auto nearby = grid.units_in_range(world, pos, look_ahead, filter);

    // Find closest blocker ahead
    f32 best_dist = look_ahead;
    const Transform* blocker_t = nullptr;

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
        if (d < 0.01f || d > look_ahead) continue;

        f32 dot = glm::dot(to_other / d, forward);
        if (dot < 0.3f) continue;  // not ahead

        f32 forward_dist = d * dot;
        f32 lateral = std::sqrt(std::max(0.0f, d * d - forward_dist * forward_dist));
        if (lateral < combined && d < best_dist) {
            best_dist = d;
            blocker_t = ot;
        }
    }

    if (!blocker_t) return forward;

    // Steer to the preferred side (deterministic per unit per time window)
    glm::vec3 perp = prefer_left ? glm::vec3{-forward.y, forward.x, 0}
                                  : glm::vec3{forward.y, -forward.x, 0};
    glm::vec3 steer = glm::normalize(perp * 0.6f + forward * 0.4f);

    // Validate: steered position must be on a passable tile
    f32 test_step = self_radius;
    f32 test_x = pos.x + steer.x * test_step;
    f32 test_y = pos.y + steer.y * test_step;
    if (!pathfinder.can_move_to(pos.x, pos.y, test_x, test_y, move_type)) {
        // Try the other side
        steer = glm::normalize(-perp * 0.6f + forward * 0.4f);
        test_x = pos.x + steer.x * test_step;
        test_y = pos.y + steer.y * test_step;
        if (!pathfinder.can_move_to(pos.x, pos.y, test_x, test_y, move_type)) {
            return forward;  // both sides blocked by terrain — just go forward
        }
    }

    return steer;
}

void system_movement(World& world, float dt, const Pathfinder& pathfinder,
                     const SpatialGrid& grid, const map::TerrainData* terrain) {
    static u32 steer_tick = 0;
    ++steer_tick;

    for (u32 i = 0; i < world.movements.count(); ++i) {
        u32 id = world.movements.ids()[i];
        auto& mov = world.movements.data()[i];

        auto* transform = world.transforms.get(id);
        auto* oq = world.order_queues.get(id);
        if (!transform || !oq) continue;

        // Status-flag gates: paused / stunned / rooted all suppress
        // movement. Combat and cast handle their own gates separately.
        if (auto* sf = world.status_flags.get(id)) {
            if (sf->flags & (status::Paused | status::Stunned | status::Rooted)) {
                continue;
            }
        }

        // ── MoveDirection short-circuit ──────────────────────────────────
        // Action-preset continuous move. No pathfinding: desired velocity
        // is `dir * speed`; slide axis-aligned on collision. The order is
        // *consumed each tick* — the action preset re-emits a fresh
        // MoveDirection per render frame while the player is holding
        // movement keys. If the player releases, no fresh order arrives,
        // oq->current stays empty, and the unit naturally idles. Removes
        // the need for an explicit Stop and the latch-tracking that
        // previously caused Stop to fire spuriously and override Casts.
        if (oq->current) {
            if (auto* md = std::get_if<orders::MoveDirection>(&oq->current->payload)) {
                glm::vec2 dir = md->dir;
                f32 mag = glm::length(dir);
                if (mag > 0.001f) {
                    if (mag > 1.0f) dir /= mag;   // clamp speed multiplier at 1
                    // Face the movement direction — turn rate applies so
                    // the hero doesn't snap instantly.
                    f32 desired = std::atan2(dir.y, dir.x);
                    f32 diff    = angle_diff(transform->facing, desired);
                    f32 max_turn = mov.turn_rate * dt;
                    if (std::abs(diff) > max_turn) {
                        transform->facing += (diff > 0 ? max_turn : -max_turn);
                    } else {
                        transform->facing = desired;
                    }
                    transform->facing = normalize_angle(transform->facing);

                    f32 step  = mov.speed * dt;
                    f32 new_x = transform->position.x + dir.x * step;
                    f32 new_y = transform->position.y + dir.y * step;
                    if (pathfinder.can_move_to(transform->position.x, transform->position.y, new_x, new_y, mov.type)) {
                        transform->position.x = new_x;
                        transform->position.y = new_y;
                    } else {
                        // Axis-aligned slide — same trick the goal-based
                        // code uses. Natural behavior: diagonal into a
                        // vertical wall slides vertically, diagonal into
                        // a horizontal wall slides horizontally, corner
                        // impact stops movement.
                        if (pathfinder.can_move_to(transform->position.x, transform->position.y, new_x, transform->position.y, mov.type)) {
                            transform->position.x = new_x;
                        }
                        if (pathfinder.can_move_to(transform->position.x, transform->position.y, transform->position.x, new_y, mov.type)) {
                            transform->position.y = new_y;
                        }
                    }
                    mov.moving = true;
                } else {
                    mov.moving = false;
                }
                // Clear any latent pathfinding state from a previous goal
                // so switching back to Move later re-plans cleanly.
                mov.corridor.clear();
                mov.has_waypoint = false;

                if (terrain) {
                    transform->position.z = map::sample_height(*terrain, transform->position.x, transform->position.y);
                }
                mov.cliff_level = pathfinder.cliff_level_at(transform->position.x, transform->position.y);
                // Consume the order. The preset re-emits a fresh
                // MoveDirection next frame if the player is still
                // holding keys; otherwise the unit naturally idles.
                oq->current.reset();
                continue;
            }
        }

        // ── Determine movement goal ──────────────────────────────────────
        glm::vec2 pos2d{transform->position.x, transform->position.y};
        glm::vec2 goal2d{0};
        bool has_goal = false;
        bool is_approach = false;
        // Effective stop radius for the active goal. 0 (the default)
        // means "exact arrival within the tile-half tolerance" the
        // Move-order branch uses below. >0 is set by Move with a
        // target_unit (Follow) or by Approach (combat/cast/pickup).
        f32  goal_range = 0.0f;
        // Did this tick's goal come from a Move order targeting a
        // unit? Distinguishes Follow ("arrived → stop, keep order")
        // from point-Move ("arrived → end order"). Stuck-detection
        // also keys off this — Follow never gives up.
        bool is_follow = false;

        // Priority 1: Move / AttackMove order
        if (oq->current) {
            if (auto* m = std::get_if<orders::Move>(&oq->current->payload)) {
                // Resolve target each tick. With target_unit valid, the
                // goal moves with the unit — that's Follow, no extra
                // order kind needed. On invalid target, end the order;
                // matches WC3's behavior when the followed unit dies
                // or vanishes from sight (vision check is implicit:
                // the picker filters foreign units in fog out at
                // smart-click time, so we already won't have started a
                // Follow on someone we can't see).
                if (m->target_unit.is_valid()) {
                    if (!world.validate(m->target_unit)) {
                        oq->current.reset();
                        if (!oq->queued.empty()) {
                            oq->current = std::move(oq->queued.front());
                            oq->queued.pop_front();
                        }
                        mov.moving = false;
                        mov.stuck_timer = 0;
                        continue;
                    }
                    auto* tt = world.transforms.get(m->target_unit.id);
                    if (tt) {
                        goal2d = {tt->position.x, tt->position.y};
                        has_goal = true;
                        is_follow = true;
                        goal_range = m->range;
                    }
                } else {
                    goal2d = {m->target.x, m->target.y};
                    has_goal = true;
                    goal_range = m->range;
                }
            } else if (auto* am = std::get_if<orders::AttackMove>(&oq->current->payload)) {
                // AttackMove with active combat target: defer to approach
                auto* combat = world.combats.get(id);
                if (combat && combat->target.is_valid()) {
                    // Don't set goal from order — fall through to approach
                } else {
                    goal2d = {am->target.x, am->target.y};
                    has_goal = true;
                }
            }
        }

        // Priority 2: Approach mode (set by combat/cast)
        if (!has_goal && mov.approach_range > 0) {
            if (mov.approach_target.is_valid()) {
                if (world.validate(mov.approach_target)) {
                    auto* ft = world.transforms.get(mov.approach_target.id);
                    if (ft) {
                        goal2d = {ft->position.x, ft->position.y};
                        has_goal = true;
                        is_approach = true;
                    }
                } else {
                    // Target dead/invalid — stop approaching
                    mov.approach_target = Unit{};
                    mov.approach_range = 0;
                }
            } else {
                // Fixed position approach (point-targeted spell)
                goal2d = mov.approach_goal;
                has_goal = true;
                is_approach = true;
            }

            // Already in range? Don't move.
            if (has_goal && is_approach) {
                f32 dist = glm::length(goal2d - pos2d);
                if (dist <= mov.approach_range) {
                    mov.moving = false;
                    continue;
                }
            }
        }

        if (!has_goal) {
            mov.moving = false;
            continue;
        }

        // ── Re-path: compute corridor + straight-line waypoint ──────────
        mov.repath_timer -= dt;
        // Repath only on the timer or a moved goal. "no waypoint" and
        // "empty corridor" don't trigger repath any more — they'd cause
        // a fresh A* every tick after a failure (spam) and are already
        // covered by the natural startup case where repath_timer is 0
        // until the first repath runs. Mid-corridor advances are handled
        // by find_straight_waypoint and never set has_waypoint=false.
        f32 dest_drift = glm::length(goal2d - mov.path_dest);
        bool rp_timer = mov.repath_timer <= 0;
        bool rp_drift = dest_drift > pathfinder.tile_size();
        bool need_repath = rp_drift || rp_timer;

        if (need_repath) {
            mov.repath_timer = Movement::REPATH_INTERVAL;
            mov.path_dest = goal2d;
            auto corridor = pathfinder.find_corridor(pos2d, goal2d, mov.cliff_level, mov.type, &world, id);
            if (corridor.valid && !corridor.cells.empty()) {
                mov.corridor = std::move(corridor.cells);
                mov.waypoint = pathfinder.find_straight_waypoint(pos2d, mov.corridor, mov.collision_radius, mov.cliff_level, mov.type);
                mov.has_waypoint = true;
                mov.moving = true;
            } else {
                mov.corridor.clear();
                mov.has_waypoint = false;
                mov.moving = false;
                // Path failed — clear approach so combat can auto-acquire a reachable target
                mov.approach_target = simulation::Unit{};
                mov.approach_range = 0;
            }
        }

        // Stuck detection runs FIRST — before the no-waypoint early-out
        // — so an A* that keeps failing accumulates stuck-time too. If
        // the unit hasn't progressed STUCK_PROGRESS_EPS units in
        // STUCK_TIMEOUT seconds, give up the order. Point-Move only
        // (Follow / Approach are exempt: those orders are about
        // tracking, not arriving — keep the timer reset so the next
        // point-Move starts from a clean slate).
        if (!is_approach && !is_follow) {
            f32 progress = glm::length(pos2d - mov.stuck_anchor);
            if (progress >= Movement::STUCK_PROGRESS_EPS) {
                mov.stuck_anchor = pos2d;
                mov.stuck_timer  = 0;
            } else {
                mov.stuck_timer += dt;
                if (mov.stuck_timer >= Movement::STUCK_TIMEOUT) {
                    mov.corridor.clear();
                    mov.has_waypoint = false;
                    mov.moving = false;
                    mov.stuck_timer = 0;
                    oq->current.reset();
                    if (!oq->queued.empty()) {
                        oq->current = std::move(oq->queued.front());
                        oq->queued.pop_front();
                    }
                    continue;
                }
            }
        } else {
            mov.stuck_anchor = pos2d;
            mov.stuck_timer  = 0;
        }

        // Face toward goal even if no path
        if (!mov.has_waypoint) {
            glm::vec2 to_goal = goal2d - pos2d;
            f32 dist = glm::length(to_goal);
            if (dist > 1.0f) {
                f32 desired = std::atan2(to_goal.y, to_goal.x);
                f32 diff = angle_diff(transform->facing, desired);
                f32 max_turn = mov.turn_rate * dt;
                if (std::abs(diff) > max_turn) {
                    transform->facing += (diff > 0 ? max_turn : -max_turn);
                } else {
                    transform->facing = desired;
                }
                transform->facing = normalize_angle(transform->facing);
            }
            continue;
        }

        // Check arrival at destination (orders only — approach stops via range check above).
        // Follow (Move with target_unit + range>0) doesn't end on
        // arrival — the order stays active so we resume tracking when
        // the target moves. Point-Move (range=0) ends as before.
        if (!is_approach) {
            f32 goal_dist = glm::length(goal2d - pos2d);
            f32 stop_dist = std::max(goal_range, pathfinder.tile_size() * 0.5f);
            if (goal_dist < stop_dist) {
                mov.corridor.clear();
                mov.has_waypoint = false;
                mov.moving = false;
                mov.stuck_timer = 0;
                if (!is_follow) {
                    oq->current.reset();
                    if (!oq->queued.empty()) {
                        oq->current = std::move(oq->queued.front());
                        oq->queued.pop_front();
                    }
                } else {
                    // Follow in range: keep turning to face the followed
                    // target. The next tick's re-path immediately flips
                    // has_waypoint back on, which short-circuits the
                    // existing "face toward goal when no waypoint" block
                    // — so we update facing here, before the `continue`.
                    glm::vec2 to_goal = goal2d - pos2d;
                    f32 d = glm::length(to_goal);
                    if (d > 1.0f) {
                        f32 desired = std::atan2(to_goal.y, to_goal.x);
                        f32 diff = angle_diff(transform->facing, desired);
                        f32 max_turn = mov.turn_rate * dt;
                        if (std::abs(diff) > max_turn) {
                            transform->facing += (diff > 0 ? max_turn : -max_turn);
                        } else {
                            transform->facing = desired;
                        }
                        transform->facing = normalize_angle(transform->facing);
                    }
                }
                continue;
            }
        }

        // (Stuck detection moved above the no-waypoint early-return.)

        // ── Move toward waypoint ─────────────────────────────────────────
        glm::vec2 to_wp = mov.waypoint - pos2d;
        f32 wp_dist = glm::length(to_wp);

        if (wp_dist < 16.0f) {
            mov.waypoint = pathfinder.find_straight_waypoint(pos2d, mov.corridor, mov.collision_radius, mov.cliff_level, mov.type);
            to_wp = mov.waypoint - pos2d;
            wp_dist = glm::length(to_wp);
            if (wp_dist < 1.0f) {
                // We've walked to the last cell of the corridor without
                // the arrival check (earlier in this tick) firing —
                // meaning the corridor ended SHORT of the goal. That
                // happens whenever A* hit its cap and returned a
                // partial path. Don't cancel the order: clear the
                // waypoint and let the natural cycle take over.
                //
                // If we *did* progress meaningfully during this corridor
                // (current position is more than a tile away from where
                // A* started), reset repath_timer so the next tick gets
                // a fresh A* from this closer vantage. If we barely
                // moved (truly stuck), don't reset — let the full 1.5s
                // pass before the next attempt, and let stuck_timer
                // (3s) give up if all retries fail.
                if (!mov.corridor.empty()) {
                    glm::vec2 corridor_start = pathfinder.cell_center(
                        mov.corridor.front().x, mov.corridor.front().y);
                    f32 walked = glm::length(pos2d - corridor_start);
                    if (walked > pathfinder.tile_size()) {
                        mov.repath_timer = 0;
                    }
                }
                mov.has_waypoint = false;
                mov.moving = false;
                continue;
            }
        }

        glm::vec3 forward{to_wp.x / wp_dist, to_wp.y / wp_dist, 0.0f};

        // Local steering: avoid nearby units (1/4 frequency, staggered by ID)
        glm::vec3 dir = forward;
        if ((id + steer_tick) % 4 == 0) {
            // Deterministic side preference: stable per unit for ~0.3s
            u32 time_bucket = steer_tick / 10;  // flips every ~10 ticks (~0.3s at 32Hz)
            bool prefer_left = ((id + time_bucket) % 2) == 0;
            dir = local_steer(world, grid, pathfinder, id,
                              transform->position, forward, mov.collision_radius,
                              mov.type, prefer_left);
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

        // Step forward (only if roughly facing the right direction)
        if (std::abs(face_diff) < glm::half_pi<f32>()) {
            f32 step = mov.speed * dt;
            if (step > wp_dist) step = wp_dist;
            f32 new_x = transform->position.x + dir.x * step;
            f32 new_y = transform->position.y + dir.y * step;

            if (pathfinder.can_move_to(transform->position.x, transform->position.y, new_x, new_y, mov.type)) {
                transform->position.x = new_x;
                transform->position.y = new_y;
            } else {
                bool slid = false;
                if (pathfinder.can_move_to(transform->position.x, transform->position.y, new_x, transform->position.y, mov.type)) {
                    transform->position.x = new_x;
                    slid = true;
                }
                if (pathfinder.can_move_to(transform->position.x, transform->position.y, transform->position.x, new_y, mov.type)) {
                    transform->position.y = new_y;
                    slid = true;
                }
                if (!slid) {
                    mov.corridor.clear();
                    mov.has_waypoint = false;
                }
            }
        }

        // Update visual Z and cliff level
        if (terrain) {
            transform->position.z = map::sample_height(*terrain, transform->position.x, transform->position.y);
        }
        mov.cliff_level = pathfinder.cliff_level_at(transform->position.x, transform->position.y);
        mov.moving = true;
    }
}

// ── Helper: deal attack damage ────────────────────────────────────────────
// Uses the unified deal_damage() from world.cpp which fires the on_damage callback.

static void deal_attack_damage(World& world, Unit source, Unit target, f32 amount) {
    deal_damage(world, source, target, amount, "attack");
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

void system_combat(World& world, float dt, const SpatialGrid& grid) {
    for (u32 i = 0; i < world.combats.count(); ++i) {
        u32 id = world.combats.ids()[i];
        auto& combat = world.combats.data()[i];

        auto* transform = world.transforms.get(id);
        auto* oq = world.order_queues.get(id);
        if (!transform || !oq) continue;

        // Status-flag gates. Stunned / Paused skip the unit entirely;
        // Disarmed prevents auto-attack target acquisition AND active
        // attack swings — the unit can still take orders and cast, it
        // just can't strike.
        if (auto* sf = world.status_flags.get(id)) {
            if (sf->flags & (status::Stunned | status::Paused | status::Disarmed)) {
                if (sf->flags & status::Disarmed) {
                    // Drop the current attack target so the unit visibly
                    // disengages rather than holding a stale aim.
                    combat.target = Unit{};
                    combat.attack_state = AttackState::Idle;
                }
                continue;
            }
        }

        // Determine attack target: explicit Attack order, or auto-acquired during AttackMove/idle
        auto* attack_order = oq->current ? std::get_if<orders::Attack>(&oq->current->payload) : nullptr;
        bool is_casting = oq->current && std::get_if<orders::Cast>(&oq->current->payload);
        bool is_moving  = oq->current && std::get_if<orders::Move>(&oq->current->payload);
        bool is_holding = oq->current && std::get_if<orders::HoldPosition>(&oq->current->payload);

        // Unit has a Move order — don't attack, obey the order
        if (is_moving) {
            combat.target = Unit{};
            combat.attack_state = AttackState::Idle;
            continue;
        }

        // Acquire disabled (NoAcquire flag is set by some active buff —
        // e.g. Wind Walk): drop the auto-acquired target (explicit Attack
        // orders pass through) and snap any pre-strike state back to
        // Idle. Backswing / Cooldown finish naturally so an
        // already-committed swing still lands.
        bool no_acquire = false;
        if (auto* sf = world.status_flags.get(id)) {
            no_acquire = (sf->flags & status::NoAcquire) != 0;
        }
        if (no_acquire && !attack_order) {
            combat.target = Unit{};
            if (combat.attack_state == AttackState::WindUp ||
                combat.attack_state == AttackState::TurningToFace ||
                combat.attack_state == AttackState::MovingToTarget) {
                combat.attack_state = AttackState::Idle;
            }
        }

        // Get current target: from Attack order, or from combat.target (auto-acquired/fight-back)
        Unit target;
        if (attack_order) {
            target = attack_order->target;
        } else {
            target = combat.target;
        }

        // Validate target. Rejected for:
        //   • invalid handle / dead
        //   • the unit itself (no self-attack, regardless of how the
        //     order got here — A-click on self, Lua misuse, etc.)
        //   • no longer visible to the attacker (e.g. target Wind-Walked
        //     mid-fight — same Vision rules as the auto-acquire scan, so
        //     invisibility, fog, and true-sight all behave consistently
        //     for "drop target" as well as "pick up new target").
        // Friendly fire is intentionally allowed: A-click on an ally
        // is a force-attack and the engine honors it.
        bool target_valid = target.is_valid() && world.validate(target);
        if (target_valid && target.id == id) target_valid = false;
        if (target_valid) {
            auto* target_hp = world.healths.get(target.id);
            if (!target_hp || target_hp->current <= 0) target_valid = false;
        }
        if (target_valid) {
            auto* owner = world.owners.get(id);
            if (owner && !grid.is_visible_to(world, target.id, owner->player)) {
                target_valid = false;
            }
        }

        if (!target_valid) {
            // Target dead or gone — let backswing/cooldown finish before clearing
            combat.target = Unit{};
            if (combat.attack_state != AttackState::Idle &&
                combat.attack_state != AttackState::Backswing &&
                combat.attack_state != AttackState::Cooldown) {
                combat.attack_state = AttackState::Idle;
            }

            // Clear approach so movement system stops chasing the dead target
            // — but NOT when the unit is mid-cast: the cast pump owns
            // approach_range / approach_target while a Cast order is active
            // (walk-to-target until in range). Clearing them here every tick
            // pinned the unit after one step of out-of-range cast walk-up.
            auto* mov = world.movements.get(id);
            if (mov && !is_casting) {
                mov->approach_target = Unit{};
                mov->approach_range = 0;
            }

            if (attack_order) {
                // Explicit Attack order finished — advance queue
                oq->current.reset();
                if (!oq->queued.empty()) {
                    oq->current = std::move(oq->queued.front());
                    oq->queued.pop_front();
                }
            }
            // AttackMove: order stays — movement system picks up the destination next tick

            // Auto-acquire: scan for nearby enemies. Hold Position restricts
            // scanning to the unit's own attack range so the unit never picks
            // up a target it would then need to chase.
            f32 acquire_r = is_holding ? combat.range : combat.acquire_range;
            if (!no_acquire && !is_casting && !is_moving && acquire_r > 0 && combat.damage > 0) {
                auto* owner = world.owners.get(id);
                if (owner) {
                    UnitFilter filter;
                    filter.enemy_of   = owner->player;
                    filter.visible_to = owner->player;  // skip fogged / unexplored
                    filter.alive_only = true;
                    auto enemies = grid.units_in_range(world, transform->position, acquire_r, filter);
                    Unit best;
                    f32 best_dist = acquire_r + 1;
                    for (auto& e : enemies) {
                        // Skip targets the engine won't let us hit.
                        // The spatial filter already excluded
                        // Untargetable; Invulnerable + Unattackable
                        // aren't in UnitFilter (they don't affect
                        // spatial queries) so we check here.
                        auto* sf = world.status_flags.get(e.id);
                        if (sf && (sf->flags & (status::Invulnerable |
                                                status::Unattackable))) continue;
                        auto* et = world.transforms.get(e.id);
                        if (!et) continue;
                        f32 d = glm::length(et->position - transform->position);
                        if (d < best_dist) { best = e; best_dist = d; }
                    }
                    if (best.is_valid()) {
                        // Auto-acquire: set combat target only, no order created.
                        // This matches WC3 behavior — auto-attack is not an order.
                        combat.target = best;
                        target = best;
                        target_valid = true;
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

        // WC3-style center-to-edge range: attacker can fire when the
        // distance from its center to the target's *edge* is within
        // its attack_range. Without the target-radius term, a melee
        // attacker can never get close enough to a large building
        // (center-to-center distance always exceeds attack_range
        // because the building itself is wider than the gap).
        f32 target_radius = 0.0f;
        if (auto* tm = world.movements.get(target.id)) target_radius = tm->collision_radius;
        f32 effective_range = combat.range + target_radius;

        switch (combat.attack_state) {
        case AttackState::Idle:
        case AttackState::MovingToTarget: {
            if (dist > effective_range) {
                if (is_holding) {
                    // Hold Position: never pursue. Drop the target so next
                    // tick's auto-acquire picks up only enemies already in
                    // attack range. Leaving it latched would cause the
                    // movement system to keep chasing via approach_target.
                    combat.target = Unit{};
                    combat.attack_state = AttackState::Idle;
                    auto* mov = world.movements.get(id);
                    if (mov) {
                        mov->approach_range = 0;
                        mov->approach_target = Unit{};
                    }
                } else {
                    // Delegate movement to the movement system via approach
                    combat.attack_state = AttackState::MovingToTarget;
                    auto* mov = world.movements.get(id);
                    if (mov) {
                        mov->approach_target = target;
                        mov->approach_range = effective_range;
                    }
                }
            } else {
                // In range — stop approaching, begin attack
                auto* mov = world.movements.get(id);
                if (mov) {
                    mov->approach_range = 0;
                    mov->approach_target = Unit{};
                    mov->corridor.clear();
                    mov->has_waypoint = false;
                }
                combat.attack_state = AttackState::TurningToFace;
            }
            break;
        }

        case AttackState::TurningToFace: {
            if (dist > effective_range * 1.2f) {
                if (is_holding) {
                    combat.target = Unit{};
                    combat.attack_state = AttackState::Idle;
                } else {
                    combat.attack_state = AttackState::MovingToTarget;
                }
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
                if (target_valid && dist <= effective_range) {
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
        // Expired instances release their flag refcounts before erase
        // so the StatusFlags overlay drops back in lockstep. Permanent
        // (-1) durations are skipped untouched. We also collect their
        // ability_ids so the post-pass can fire on_ability_removed —
        // the host wires that to AbilityRemove broadcasts so clients
        // drop the buff at the same tick.
        std::vector<std::vector<std::string>> expired_flag_lists;
        std::vector<std::string> expired_ids;
        std::erase_if(aset.abilities,
            [dt, &modifiers_changed, &expired_flag_lists, &expired_ids](AbilityInstance& a) {
                if (a.remaining_duration < 0) return false;
                a.remaining_duration -= dt;
                if (a.remaining_duration <= 0) {
                    modifiers_changed = true;
                    if (!a.active_flags.empty()) {
                        expired_flag_lists.push_back(std::move(a.active_flags));
                    }
                    expired_ids.push_back(std::move(a.ability_id));
                    return true;
                }
                return false;
            });
        for (auto& flags : expired_flag_lists) {
            flag_refcount_delta(world, id, flags, -1);
        }
        if (modifiers_changed) {
            recalculate_modifiers(world, id);
        }
        if (world.on_ability_removed && !expired_ids.empty()) {
            Unit u;
            u.id = id;
            auto* info = world.handle_infos.get(id);
            u.generation = info ? info->generation : 0;
            for (auto& aid : expired_ids) {
                world.on_ability_removed(u, aid);
            }
        }

        // Status-flag gates. Cooldown / duration ticking above runs
        // for ALL units — even paused / stunned ones tick passive
        // durations down. The cast state machine below is what we
        // actually freeze.
        //
        // Stunned mid-cast: cancel cleanly (matches the "new order"
        // cancel rule — no cooldown applied, no endcast event since
        // we never reached it on purpose).
        if (auto* sf = world.status_flags.get(id)) {
            if (sf->flags & (status::Stunned | status::Paused | status::Silenced)) {
                if (aset.cast_state != CastState::None &&
                    (sf->flags & (status::Stunned | status::Silenced))) {
                    aset.cast_state = CastState::None;
                    aset.cast_timer = 0;
                    aset.casting_id.clear();
                    aset.cast_target_unit = Unit{};
                    aset.cast_target_pos  = glm::vec3{0};
                    aset.cast_source_item = Item{};
                    if (auto* oq2 = world.order_queues.get(id)) {
                        oq2->current.reset();
                    }
                }
                continue;
            }
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
                        aset.cast_source_item = cast_order->source_item;

                        auto& lvl = def->level_data(inst->level);
                        bool needs_range = (def->form == AbilityForm::Target);
                        if (needs_range && lvl.range > 0) {
                            aset.cast_state = CastState::MovingToTarget;
                            // Delegate approach to movement system. A widget
                            // pick (cast_target_unit valid) approaches the
                            // widget; otherwise the cast is point-only and
                            // approaches the ground location.
                            auto* mov = world.movements.get(id);
                            if (mov) {
                                mov->approach_range = lvl.range;
                                if (world.validate(cast_order->target_unit)) {
                                    mov->approach_target = cast_order->target_unit;
                                } else {
                                    mov->approach_goal = {cast_order->target_pos.x, cast_order->target_pos.y};
                                }
                            }
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

                    // Resolve target position. When the cast snapped to
                    // a widget, follow that widget; otherwise use the
                    // authored ground point.
                    glm::vec3 target_pos = aset.cast_target_pos;
                    if (world.validate(aset.cast_target_unit)) {
                        auto* tt = world.transforms.get(aset.cast_target_unit.id);
                        if (tt) target_pos = tt->position;
                    }

                    glm::vec3 to_target = target_pos - transform->position;
                    to_target.z = 0;
                    f32 dist = glm::length(to_target);

                    switch (aset.cast_state) {
                    case CastState::MovingToTarget: {
                        if (dist <= lvl.range) {
                            // In range — stop approaching, begin cast
                            auto* mov = world.movements.get(id);
                            if (mov) { mov->approach_range = 0; mov->approach_target = Unit{}; }
                            aset.cast_state = CastState::TurningToFace;
                        }
                        // Movement system handles the actual approach
                        break;
                    }
                    case CastState::TurningToFace: {
                        // Instant leaves target_pos at (0,0,0); a
                        // self-targeted widget cast has nowhere to face.
                        // The dist guard catches a point cast at the
                        // caster's own feet (avoids atan2(0,0)).
                        bool is_immediate = (def->form == AbilityForm::Instant);
                        bool is_self = (world.validate(aset.cast_target_unit) &&
                                        aset.cast_target_unit.id == id);
                        if (!is_immediate && !is_self && dist > 0.001f) {
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
                        aset.cast_state = CastState::Foreswing;
                        aset.cast_timer = lvl.cast_time;
                        aset.foreswing_secs      = lvl.cast_time;
                        aset.channel_secs        = lvl.channel_time;
                        aset.cast_backswing_secs = lvl.backsw_time;
                        break;
                    }
                    case CastState::Foreswing:
                        aset.cast_timer -= dt;
                        if (aset.cast_timer <= 0) {
                            Unit caster;
                            caster.id = id;
                            auto* info = world.handle_infos.get(id);
                            caster.generation = info ? info->generation : 0;

                            if (lvl.channel_time > 0) {
                                // Channelled cast: hand off to Channeling.
                                // Fire CHANNEL (start) here; EFFECT will fire
                                // at natural channel completion. Cooldown is
                                // held back to natural channel completion so a
                                // cancel doesn't put the ability on cooldown.
                                if (world.on_ability_channel) {
                                    world.on_ability_channel(caster, aset.casting_id,
                                                             aset.cast_target_unit, target_pos,
                                                             aset.cast_source_item);
                                }
                                aset.cast_state = CastState::Channeling;
                                aset.cast_timer = lvl.channel_time;
                            } else {
                                // Non-channeled: effect fires now, cooldown
                                // begins, transition to Backswing.
                                if (world.on_ability_effect) {
                                    world.on_ability_effect(caster, aset.casting_id,
                                                            aset.cast_target_unit, target_pos,
                                                            aset.cast_source_item);
                                }
                                inst->cooldown_remaining = lvl.cooldown;
                                aset.cast_state = CastState::Backswing;
                                aset.cast_timer = lvl.backsw_time;
                            }
                        }
                        break;
                    case CastState::Channeling: {
                        aset.cast_timer -= dt;
                        // Interrupt edges. Any newly-issued non-cast order
                        // (move / attack / stop / etc.) replaces oq->current
                        // when it gets executed — but during a channel we
                        // hold oq->current=Cast, so the new order sits in
                        // oq->queued. Treat a non-empty queue as the user's
                        // signal to break the channel.
                        bool interrupted = oq && !oq->queued.empty();
                        Unit caster;
                        caster.id = id;
                        auto* info = world.handle_infos.get(id);
                        caster.generation = info ? info->generation : 0;

                        if (interrupted) {
                            // ENDCAST fires for both natural completion and
                            // interruption (matches WC3 SPELL_ENDCAST).
                            // EFFECT does NOT fire on interrupt — the spell
                            // never resolved.
                            if (world.on_ability_endcast) {
                                world.on_ability_endcast(caster, aset.casting_id,
                                                         aset.cast_target_unit, target_pos,
                                                         aset.cast_source_item);
                            }
                            // Cancel: no cooldown, no backswing — drop the
                            // cast cleanly so the next order takes over.
                            aset.cast_state = CastState::None;
                            aset.casting_id.clear();
                            oq->current.reset();
                            break;
                        }
                        if (aset.cast_timer <= 0) {
                            // Natural completion. ENDCAST fires first, then
                            // EFFECT (matches WC3: SPELL_ENDCAST followed by
                            // SPELL_EFFECT). Cooldown begins now, backswing
                            // follows.
                            if (world.on_ability_endcast) {
                                world.on_ability_endcast(caster, aset.casting_id,
                                                         aset.cast_target_unit, target_pos,
                                                         aset.cast_source_item);
                            }
                            if (world.on_ability_effect) {
                                world.on_ability_effect(caster, aset.casting_id,
                                                        aset.cast_target_unit, target_pos,
                                                        aset.cast_source_item);
                            }
                            inst->cooldown_remaining = lvl.cooldown;
                            aset.cast_state = CastState::Backswing;
                            aset.cast_timer = lvl.backsw_time;
                        }
                        break;
                    }
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

// ── Item system — pickup walk-then-claim, drop ─────────────────────────────
//
// Keeps the cast / combat pattern symmetric: the order ("walk to item,
// then take it") is split between this loop's claim check and the
// movement system's approach machinery. We set `approach_goal` to the
// item position with `approach_range = pickup_radius`; movement walks
// the unit there, this system fires the actual transfer once the unit
// is in range.
//
// Drops happen instantly — no walk — so DropItem is a one-shot per
// tick.

void system_items(World& world, float /*dt*/) {
    if (!world.types) return;

    for (u32 i = 0; i < world.order_queues.count(); ++i) {
        u32 id = world.order_queues.ids()[i];
        auto& oq = world.order_queues.data()[i];
        if (!oq.current) continue;

        // ── PickupItem ───────────────────────────────────────────────
        if (auto* po = std::get_if<orders::PickupItem>(&oq.current->payload)) {
            // Validate target item still exists.
            if (!world.validate(po->item)) {
                oq.current.reset();
                if (auto* mov = world.movements.get(id)) {
                    mov->approach_range = 0;
                    mov->approach_target = Unit{};
                }
                continue;
            }
            // Already carried (by anyone) → cancel the pickup quietly.
            if (auto* car = world.carriables.get(po->item.id);
                car && car->carried_by.is_valid()) {
                oq.current.reset();
                if (auto* mov = world.movements.get(id)) {
                    mov->approach_range = 0;
                    mov->approach_target = Unit{};
                }
                continue;
            }

            const auto* tf_unit = world.transforms.get(id);
            const auto* tf_item = world.transforms.get(po->item.id);
            if (!tf_unit || !tf_item) {
                oq.current.reset();
                continue;
            }

            // Compute pickup radius from the item type def.
            f32 pickup_r = 48.0f;
            if (auto* info = world.item_infos.get(po->item.id)) {
                if (auto* def = world.types->get_item_type(info->type_id)) {
                    pickup_r = def->pickup_radius;
                }
            }

            f32 dx = tf_item->position.x - tf_unit->position.x;
            f32 dy = tf_item->position.y - tf_unit->position.y;
            f32 dist2 = dx*dx + dy*dy;

            // In range → claim into first free inventory slot.
            if (dist2 <= pickup_r * pickup_r) {
                Unit unit_h{ id, world.handle_infos.get(id) ?
                                  world.handle_infos.get(id)->generation : 0 };
                i32 slot = give_item_to_unit(world, unit_h, po->item);
                // slot < 0 = inventory full; the claim failed but we
                // still pop the order so the unit doesn't stand on
                // top of the item forever. Map Lua can re-issue if it
                // wants different behavior.
                if (auto* mov = world.movements.get(id)) {
                    mov->approach_range = 0;
                    mov->approach_target = Unit{};
                }
                if (slot >= 0 && world.on_item_picked_up) {
                    world.on_item_picked_up(unit_h, po->item, slot);
                }
                oq.current.reset();
                continue;
            }

            // Out of range → ensure movement is approaching the item.
            if (auto* mov = world.movements.get(id)) {
                mov->approach_target = Unit{};
                mov->approach_goal   = { tf_item->position.x, tf_item->position.y };
                mov->approach_range  = pickup_r;
            }
            // Stay in this order; next tick re-checks distance.
        }

        // ── DropItem ─────────────────────────────────────────────────
        // WC3-style: the carrier must be within DROP_RANGE of the
        // requested drop point. If the click was further, the order
        // stays current and the unit walks toward the point — same
        // pattern as PickupItem above. The order completes once the
        // carrier is in range, the item lands, and an item_dropped
        // event fires.
        else if (auto* d = std::get_if<orders::DropItem>(&oq.current->payload)) {
            constexpr f32 DROP_RANGE = 150.0f;     // engine-wide drop reach (game units)

            Unit unit_h{ id, world.handle_infos.get(id) ?
                              world.handle_infos.get(id)->generation : 0 };

            // Find which slot holds this item.
            i32 slot = -1;
            if (auto* inv = world.inventories.get(id)) {
                for (i32 s = 0; s < static_cast<i32>(inv->slots.size()); ++s) {
                    if (inv->slots[s].id == d->item.id &&
                        inv->slots[s].generation == d->item.generation) {
                        slot = s; break;
                    }
                }
            }
            if (slot < 0) { oq.current.reset(); continue; }

            const auto* tf = world.transforms.get(id);
            if (!tf) { oq.current.reset(); continue; }

            // Resolve drop point. (0,0,0) → "drop in place" (Lua /
            // legacy callers): the carrier's current pos is always
            // in range so the in-range branch below executes.
            glm::vec3 pos = d->pos;
            if (pos.x == 0.0f && pos.y == 0.0f) pos = tf->position;

            f32 dx = pos.x - tf->position.x;
            f32 dy = pos.y - tf->position.y;
            f32 dist2 = dx*dx + dy*dy;
            if (dist2 <= DROP_RANGE * DROP_RANGE) {
                if (auto* mov = world.movements.get(id)) {
                    mov->approach_range  = 0;
                    mov->approach_target = Unit{};
                }
                drop_item_from_unit(world, unit_h, slot, pos);
                if (world.on_item_dropped) {
                    world.on_item_dropped(unit_h, d->item);
                }
                oq.current.reset();
            } else {
                // Out of reach — walk toward the drop point. Stay in
                // this order so the next tick re-checks distance.
                if (auto* mov = world.movements.get(id)) {
                    mov->approach_target = Unit{};
                    mov->approach_goal   = { pos.x, pos.y };
                    mov->approach_range  = DROP_RANGE;
                }
            }
        }

        // ── SwapInventorySlot ────────────────────────────────────────
        // Pure rearrange — no abilities re-grant, no drop / pickup
        // events. Both slots already belong to this unit so the
        // ability set is unchanged.
        else if (auto* sw = std::get_if<orders::SwapInventorySlot>(&oq.current->payload)) {
            if (auto* inv = world.inventories.get(id)) {
                i32 n = static_cast<i32>(inv->slots.size());
                if (sw->slot_a >= 0 && sw->slot_a < n &&
                    sw->slot_b >= 0 && sw->slot_b < n &&
                    sw->slot_a != sw->slot_b) {
                    std::swap(inv->slots[sw->slot_a], inv->slots[sw->slot_b]);
                }
            }
            oq.current.reset();
        }
    }
}

// ── Projectile system ─────────────────────────────────────────────────────

void system_projectile(World& world, float dt) {
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
    world.sights.remove(h.id);
    world.order_queues.remove(h.id);
    world.ability_sets.remove(h.id);
    world.classifications.remove(h.id);
    world.inventories.remove(h.id);
    world.buildings.remove(h.id);
    world.constructions.remove(h.id);
    world.destructables.remove(h.id);
    world.doodads.remove(h.id);
    if (world.on_pathing_unblock) {
        auto* pb = world.pathing_blockers.get(h.id);
        if (pb) world.on_pathing_unblock(pb->cx, pb->cy, pb->w, pb->h);
    }
    world.pathing_blockers.remove(h.id);
    world.item_infos.remove(h.id);
    world.carriables.remove(h.id);
    world.projectiles.remove(h.id);
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

        const auto* self_owner = world.owners.get(id);
        for (auto& other : nearby) {
            if (other.id <= id) continue;
            if (world.dead_states.has(other.id)) continue;

            // Push only between units owned by the same player. Enemy /
            // neutral units pass through each other at this layer — combat
            // does the gating that matters there. Without this, a kiting
            // enemy could shove your held units off-position, and two
            // hostile mobs collide-jostling looks awful.
            const auto* other_owner = world.owners.get(other.id);
            if (!self_owner || !other_owner ||
                self_owner->player.id != other_owner->player.id) {
                continue;
            }

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
                    if (pathfinder.can_move_to(transform->position.x, transform->position.y, ax, ay, mov->type)) {
                        transform->position.x = ax;
                        transform->position.y = ay;
                    }
                    MoveType other_type = other_mov ? other_mov->type : MoveType::Ground;
                    if (pathfinder.can_move_to(other_t->position.x, other_t->position.y, bx, by, other_type)) {
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
            auto* info = world.handle_infos.get(id);
            if (!info) continue;
            if (info->category == Category::Item) continue;

            // EVENT_UNIT_DYING. Fires before HP is pinned to 0 / before
            // any corpse conversion. Handlers may SetUnitHealth the unit
            // back up (Reincarnation, Phoenix Fire, Cheat Death). After
            // the callback we re-read HP — if it's positive, the unit
            // survived and we skip reaping. Only fires for Units; for
            // destructables / other widgets we still go straight to
            // the corpse path. Re-fetch the Health pointer afterwards
            // because the SparseSet may have shifted on add/remove.
            if (world.on_dying && info->category == Category::Unit) {
                Unit dying;
                dying.id = id;
                dying.generation = info->generation;
                // TODO: track actual killer from last damage source
                world.on_dying(dying, Unit{});
                auto* hp_after = world.healths.get(id);
                if (hp_after && hp_after->current >= DEATH_THRESHOLD) {
                    // Unit was healed during the dying handler — cancel
                    // the death. Loop continues; the sparse-set index
                    // may now be stale (the handler could have spawned
                    // entities), but the bounds check on the next
                    // iteration handles that.
                    continue;
                }
                // Handler ran but didn't save the unit — pointer might
                // have shifted if other entities were added; refresh.
                auto* hp_now = world.healths.get(id);
                if (!hp_now) continue;
                hp_now->current = 0;
            } else {
                hp.current = 0;
            }

            log::info("Combat", "{} has died (id={})", info->type_id, id);

            // Play death sound
            if (world.on_sound) {
                auto* def = world.types->get_unit_type(info->type_id);
                if (def && !def->sound_death.empty()) {
                    auto* t = world.transforms.get(id);
                    if (t) world.on_sound(def->sound_death, t->position);
                }
            }

            // Fire death callback for script engine — units only. The
            // callback is typed as Unit; destructables share the handle
            // layout but the script side fires unit_death events that
            // would be confusing for environment objects.
            if (world.on_death && info->category == Category::Unit) {
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
            world.sights.remove(id);

            // Drop the runtime pathing block immediately. WC3-style:
            // the moment a building dies, its rubble is walkable —
            // even though the corpse renderable lingers for a few
            // seconds before despawning. Without this, pathfinding
            // still treats the dead building as an obstacle until the
            // entity is fully destroyed.
            if (auto* pb = world.pathing_blockers.get(id)) {
                if (world.on_pathing_unblock) {
                    world.on_pathing_unblock(pb->cx, pb->cy, pb->w, pb->h);
                }
                world.pathing_blockers.remove(id);
            }

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
        remove_all_components_and_free(world, h);
    }
}

// Returns true if `pos` lies inside any of the region's shapes (rects
// or circles). A region with no shapes never contains anything.
static bool region_contains_point(const World::Region& r, glm::vec3 pos) {
    for (const auto& rect : r.rects) {
        if (pos.x >= rect.x0 && pos.x <= rect.x1 &&
            pos.y >= rect.y0 && pos.y <= rect.y1) {
            return true;
        }
    }
    for (const auto& c : r.circles) {
        f32 dx = pos.x - c.cx;
        f32 dy = pos.y - c.cy;
        if (dx * dx + dy * dy <= c.r * c.r) return true;
    }
    return false;
}

void system_regions(World& world) {
    if (world.regions.empty()) return;
    if (!world.on_region_enter && !world.on_region_leave) {
        // No script consumer wired — keep `contained` in sync anyway
        // so IsUnitInRegion queries return correct answers, but skip
        // the diff dispatch.
    }

    // Reused across regions to avoid reallocation.
    std::unordered_set<u32> current;

    for (auto& [rid, region] : world.regions) {
        if (!region.alive) continue;

        current.clear();
        // Cheap O(N) over alive units. Region authoring is sparse and
        // unit counts are small; fine for v1. If maps start authoring
        // many regions and many units, switch to a spatial-grid query
        // sized to the region's bounding box.
        for (u32 i = 0; i < world.transforms.count(); ++i) {
            u32 uid = world.transforms.ids()[i];
            if (world.dead_states.has(uid)) continue;
            if (region_contains_point(region, world.transforms.data()[i].position)) {
                current.insert(uid);
            }
        }

        // Resolve each diff to a stable Unit handle (with generation)
        // so the script side can validate before reading components.
        for (u32 uid : current) {
            if (region.contained.count(uid)) continue;
            if (!world.on_region_enter) continue;
            const auto* hi = world.handle_infos.get(uid);
            Unit u; u.id = uid;
            if (hi) u.generation = hi->generation;
            world.on_region_enter(rid, u);
        }
        for (u32 uid : region.contained) {
            if (current.count(uid)) continue;
            if (!world.on_region_leave) continue;
            const auto* hi = world.handle_infos.get(uid);
            Unit u; u.id = uid;
            if (hi) u.generation = hi->generation;
            world.on_region_leave(rid, u);
        }

        region.contained = std::move(current);
        current.clear();  // moved-from state may be unspecified; restore.
    }
}

} // namespace uldum::simulation
