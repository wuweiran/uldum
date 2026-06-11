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
#include <optional>

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

// Move-time hard block against OTHER PLAYERS' units. Units are transient
// (collision radius only, no pathfinding footprint — buildings own that),
// so the planner never routes around them; instead a unit simply may not
// step into a foreign unit's collision circle. Same-player units are NOT
// blocked here — they're allowed to crowd and the collision push spreads
// them out afterward. Neutral/unowned movers don't block (no owner to
// compare).
//
// Escape valve: a step is only rejected if it would sit inside the
// foreign circle AND lands no further from that unit's center than the
// current position. A unit that starts overlapping (teleport / knockback
// / SetUnitX) can therefore always walk outward — it just can't push
// deeper in. Without this a shoved-into-enemy unit would freeze.
static bool foreign_unit_blocks(const World& world, const SpatialGrid& grid,
                                u32 self_id, const Owner* self_owner,
                                f32 self_radius, glm::vec2 from, glm::vec2 to,
                                MoveType move_type) {
    if (!self_owner) return false;          // unowned movers ignore this layer
    if (move_type == MoveType::Air) return false;

    UnitFilter filter;
    filter.exclude_buildings = true;
    auto nearby = grid.units_in_range(world, glm::vec3{to.x, to.y, 0.0f},
                                      self_radius * 4.0f, filter);
    for (auto& other : nearby) {
        if (other.id == self_id) continue;
        if (world.dead_states.has(other.id)) continue;

        const auto* other_owner = world.owners.get(other.id);
        if (other_owner && other_owner->player.id == self_owner->player.id) continue;

        auto* ot = world.transforms.get(other.id);
        if (!ot) continue;
        const auto* om = world.movements.get(other.id);
        f32 other_radius = om ? om->collision_radius : 32.0f;
        f32 min_dist = self_radius + other_radius;

        glm::vec2 oc{ot->position.x, ot->position.y};
        f32 d_to   = glm::length(to   - oc);
        if (d_to >= min_dist) continue;     // step stays clear of this unit
        f32 d_from = glm::length(from - oc);
        if (d_to < d_from) return true;     // would push deeper in → block
        // else: already overlapping but moving outward → allow (escape)
    }
    return false;
}

void system_movement(World& world, float dt, const Pathfinder& pathfinder,
                     const SpatialGrid& grid, const map::TerrainData* terrain) {

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

        // A step is permitted only if terrain/buildings allow it (cell
        // pathing) AND it doesn't drive the unit into a different player's
        // unit (foreign collision circle). Same-player crowding is allowed
        // through — the collision push resolves it. Every position commit
        // below routes through this so the two gates stay in lockstep.
        const Owner* self_owner = world.owners.get(id);
        f32 self_radius = mov.collision_radius;
        auto can_step = [&](f32 ox, f32 oy, f32 nx, f32 ny) -> bool {
            if (!pathfinder.can_move_to(ox, oy, nx, ny, mov.type)) return false;
            if (foreign_unit_blocks(world, grid, id, self_owner, self_radius,
                                    {ox, oy}, {nx, ny}, mov.type)) return false;
            return true;
        };

        // Local-avoidance probe: when the straight line to the current
        // target is blocked by a foreign unit, fan candidate headings
        // outward from `heading` (straight first, then ±15°, ±30°, …
        // ±120°, right-before-left) and return a detour point along the
        // FIRST direction the unit can actually STEP in.
        //
        // The candidate test is a step-sized move (≈ one tick / one body
        // radius), NOT a far endpoint: can_step / foreign_unit_blocks is a
        // point test (is the destination inside a foreign circle?), so a
        // far endpoint *past* the blocker reads as "clear" even though the
        // straight line to it crosses the blocker — which made the unit set
        // a detour pointing straight through a single blocker and then
        // stall, unable to take the first step. Testing a step-sized move
        // makes point≈segment, so a direction is accepted only if the unit
        // can immediately move that way — the first such direction is the
        // real sidestep (~90° when a unit sits dead ahead). nullopt → boxed
        // in → caller forces an early repath.
        auto find_bypass = [&](glm::vec2 from, glm::vec2 heading) -> std::optional<glm::vec2> {
            auto rot = [](glm::vec2 v, f32 a) -> glm::vec2 {
                f32 c = std::cos(a), s = std::sin(a);
                return { v.x * c - v.y * s, v.x * s + v.y * c };
            };

            // Step-sized probe so the point test approximates the swept
            // step; at least one real tick's travel so a validated
            // direction is actually steppable this tick.
            const f32 test  = std::max(self_radius, mov.speed * dt);
            const f32 reach = std::max(self_radius * 3.0f, test * 2.0f);
            auto step_clear = [&](glm::vec2 d) -> bool {
                glm::vec2 p = from + d * test;
                return can_step(from.x, from.y, p.x, p.y);
            };

            constexpr f32 MAX_DEFLECT = 2.0943951f;             // 120°: round a wall end
            constexpr f32 STEP_DEFLECT = 0.2617994f;            // 15°
            for (f32 a = 0.0f; a <= MAX_DEFLECT + 1e-3f; a += STEP_DEFLECT) {
                glm::vec2 r = rot(heading, -a);                 // right (CW) first
                if (step_clear(r)) return from + r * reach;
                if (a > 0.0f) {
                    glm::vec2 l = rot(heading, a);              // then left (CCW)
                    if (step_clear(l)) return from + l * reach;
                }
            }
            return std::nullopt;
        };

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
                    if (can_step(transform->position.x, transform->position.y, new_x, new_y)) {
                        transform->position.x = new_x;
                        transform->position.y = new_y;
                    } else {
                        // Axis-aligned slide — same trick the goal-based
                        // code uses. Natural behavior: diagonal into a
                        // vertical wall slides vertically, diagonal into
                        // a horizontal wall slides horizontally, corner
                        // impact stops movement.
                        if (can_step(transform->position.x, transform->position.y, new_x, transform->position.y)) {
                            transform->position.x = new_x;
                        }
                        if (can_step(transform->position.x, transform->position.y, transform->position.x, new_y)) {
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
                        oq->advance();
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
            // Adaptive interval:
            //   * stuck units repath sooner so they recover from a
            //     transient block fast.
            //   * a small id-based jitter spreads N units' next repaths
            //     across a window — without it, a box-select issued on
            //     a single tick has all N units' timers expire on the
            //     same later tick, spiking the pathfinder.
            f32 base = Movement::REPATH_INTERVAL;
            if (mov.stuck_timer > 0.5f) base = 0.5f;
            const f32 jitter = static_cast<f32>(id % 16) * (1.0f / 16.0f) * 0.25f;
            mov.repath_timer = base + jitter;
            mov.path_dest = goal2d;
            // Fresh A* route → any local detour splice is stale. Drop it;
            // the new corridor reflects current blockers (an early repath
            // forced by find_bypass returning none re-plans around them).
            mov.has_detour = false;
            auto corridor = pathfinder.find_corridor(pos2d, goal2d, mov.cliff_level, mov.type);
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
                    oq->advance();
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
                    oq->advance();
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

        // ── Local avoidance: transient detour splice ─────────────────────
        // A* gave us the corridor (terrain + buildings). Foreign units are
        // NOT in it, so the straight line to the waypoint can be blocked by
        // an enemy. Each tick we probe one step toward the current target
        // (detour if set, else waypoint):
        //   * clear  → walk it; if on a detour, drop it once reached or the
        //              straight line to the waypoint is clear again.
        //   * blocked→ (re)pick a detour toward the WAYPOINT via find_bypass
        //              — EVERY tick it's blocked, even if a detour already
        //              exists, so a committed detour that seals up is
        //              replaced instead of leaving the unit to walk into it
        //              and stall. No local way around → force an early A*
        //              repath. The detour is wiped on repath.
        {
            // Step-sized lookahead: the slice we test must be the distance
            // the unit can actually MOVE this tick (point test ≈ swept step).
            // A long probe would jump its endpoint *past* a near blocker and
            // read "clear", so find_bypass never fires and the small real
            // step stalls into the blocker. One body radius (or a tick's
            // travel, whichever is larger) keeps detection honest.
            f32 probe_dist = std::max(mov.collision_radius, mov.speed * dt);

            // Heading toward the real corridor waypoint (the actual goal).
            glm::vec2 wp_to = mov.waypoint - pos2d;
            f32 wp_d = glm::length(wp_to);
            glm::vec2 wp_head = (wp_d > 1e-3f) ? wp_to / wp_d
                                               : glm::vec2{forward.x, forward.y};

            // Current steering target + heading (detour overrides waypoint).
            glm::vec2 target = mov.has_detour ? mov.detour : mov.waypoint;
            glm::vec2 to_t = target - pos2d;
            f32 t_dist = glm::length(to_t);
            glm::vec2 heading = (t_dist > 1e-3f) ? to_t / t_dist : wp_head;
            glm::vec2 probe = pos2d + heading * std::min(probe_dist, t_dist);

            if (can_step(pos2d.x, pos2d.y, probe.x, probe.y)) {
                // Step toward the current target is clear. If on a detour,
                // drop it once reached or the straight line to the waypoint
                // is clear again (blocker moved off) — rejoin the corridor.
                if (mov.has_detour) {
                    bool reached = t_dist < std::max(8.0f, probe_dist * 0.5f);
                    glm::vec2 wp_probe = pos2d + wp_head * std::min(probe_dist, wp_d);
                    bool corridor_clear = can_step(pos2d.x, pos2d.y, wp_probe.x, wp_probe.y);
                    if (reached || corridor_clear) mov.has_detour = false;
                }
            } else {
                // Step toward the current target is BLOCKED. Re-pick a detour
                // EVERY tick this happens — even if one is already set (the
                // committed detour just sealed up). Without this, a stale
                // detour is never replaced and the unit walks into it and
                // stalls. Always re-pick toward the WAYPOINT so the fresh
                // detour heads at the goal, not around the dead direction.
                if (auto bp = find_bypass(pos2d, wp_head)) {
                    mov.detour = *bp;
                    mov.has_detour = true;
                } else {
                    mov.has_detour = false;   // no way around locally → let A* retry
                    mov.repath_timer = 0;     // force an early repath
                }
            }
        }

        // Resolve the steering target after the detour decision.
        glm::vec2 steer_target = mov.has_detour ? mov.detour : mov.waypoint;
        glm::vec2 steer_to = steer_target - pos2d;
        f32 steer_dist = glm::length(steer_to);
        glm::vec3 dir = (steer_dist > 1e-3f)
            ? glm::vec3{steer_to.x / steer_dist, steer_to.y / steer_dist, 0.0f}
            : forward;

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
            if (step > steer_dist) step = steer_dist;   // don't overshoot detour/waypoint
            f32 new_x = transform->position.x + dir.x * step;
            f32 new_y = transform->position.y + dir.y * step;

            if (can_step(transform->position.x, transform->position.y, new_x, new_y)) {
                transform->position.x = new_x;
                transform->position.y = new_y;
            } else {
                bool slid = false;
                if (can_step(transform->position.x, transform->position.y, new_x, transform->position.y)) {
                    transform->position.x = new_x;
                    slid = true;
                }
                if (can_step(transform->position.x, transform->position.y, transform->position.x, new_y)) {
                    transform->position.y = new_y;
                    slid = true;
                }
                if (!slid) {
                    mov.corridor.clear();
                    mov.has_waypoint = false;
                    mov.has_detour = false;   // splice is stale if we can't move at all
                }
            }
        }

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

// ── Helpers: create + emit projectile ────────────────────────────────────

// CreateProjectile: allocate the handle, attach components, but DON'T
// configure path / speed / target yet — that's the EmitProjectile* step.
// Between Create and Emit, the projectile sits at the source position
// and doesn't move or collide; this is the window where Lua sets
// side-table state, attaches PROJECTILE_HIT / PROJECTILE_DESTROYED
// triggers, and (for the engine) marks is_attack + damage.
Unit create_projectile(World& world, Unit source, const std::string& model) {
    auto* src_t = world.transforms.get(source.id);
    if (!src_t) return Unit{};
    Handle h = world.handles.allocate();
    ProjectileComp proj;
    proj.source     = source;
    proj.spawn_pos  = src_t->position;
    proj.target_pos = src_t->position;
    // Render scale: placeholder mesh is a tiny stub; glTF projectiles
    // render at their authored size (model authors enlarge in Blender
    // rather than relying on an engine-side multiplier).
    f32 scale = model.empty() ? 0.3f : 1.0f;
    Transform t;
    t.position      = src_t->position;
    t.prev_position = src_t->position;   // prevent first-frame interpolation from world origin
    t.scale         = scale;
    world.transforms.add(h.id, std::move(t));
    world.handle_infos.add(h.id, HandleInfo{"projectile", Category::Projectile, h.generation});
    world.projectiles.add(h.id, std::move(proj));
    world.renderables.add(h.id, Renderable{model.empty() ? "projectile" : model, true});
    // Loop the model's "idle" clip while the projectile is alive. The
    // renderer's script-controlled-anim path handles the lookup; if the
    // model has no "idle" clip it logs a warning and falls back to the
    // bind pose. begin_destroy_projectile overrides this with the
    // "death" clip when the projectile resolves.
    AnimQueue aq;
    aq.clips.push_back("idle");
    aq.looping = true;
    world.anim_queues.add(h.id, std::move(aq));
    Unit u; u.id = h.id; u.generation = h.generation;
    return u;
}

void emit_projectile_target(World& world, Unit proj_unit, Unit target, f32 speed, f32 /*arc_height*/) {
    auto* p = world.projectiles.get(proj_unit.id);
    if (!p) return;
    p->path     = ProjectileComp::Path::Homing;
    p->target   = target;
    p->speed    = speed;
    p->emitted  = true;
    if (auto* tt = world.transforms.get(target.id)) p->target_pos = tt->position;
}

void emit_projectile_loc(World& world, Unit proj_unit, glm::vec3 loc, f32 speed,
                         f32 hit_radius, f32 max_distance) {
    auto* p = world.projectiles.get(proj_unit.id);
    if (!p) return;
    p->path         = ProjectileComp::Path::Linear;
    p->target       = Unit{};
    p->target_pos   = loc;
    p->speed        = speed;
    p->hit_radius   = hit_radius;
    p->max_distance = max_distance;
    p->emitted      = true;
}

// Back-compat for auto-attack call site below — spawns + emits in one
// step with is_attack=true. Will be inlined / removed once the combat
// system uses the full Lua-style API directly.
static Unit spawn_attack_projectile(World& world, Unit source, Unit target,
                                     f32 damage, f32 speed) {
    Unit u = create_projectile(world, source, "");  // default "projectile" mesh
    if (!u.is_valid()) return u;
    auto* p = world.projectiles.get(u.id);
    p->damage    = damage;
    p->is_attack = true;
    emit_projectile_target(world, u, target, speed, 0);
    return u;
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
                oq->advance();
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
                Unit self = world.unit(id);

                // Snapshot the attacker position BEFORE spawning the
                // projectile. spawn_attack_projectile → create_projectile
                // → world.transforms.add() can reallocate the dense
                // transform vector, dangling the `transform` pointer
                // cached at the top of this loop. We only need the
                // position for the attack sound below, so capture it now.
                const glm::vec3 attack_pos = transform->position;

                if (combat.is_ranged) {
                    spawn_attack_projectile(world, self, target, combat.damage, combat.projectile_speed);
                } else {
                    deal_attack_damage(world, self, target, combat.damage);
                }

                // Play attack sound
                if (world.on_sound) {
                    auto* info = world.handle_infos.get(id);
                    auto* def = info ? world.types->get_unit_type(info->type_id) : nullptr;
                    if (def && !def->sound_attack.empty()) {
                        world.on_sound(def->sound_attack, attack_pos);
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
            Unit u = world.unit(id);
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
                        // Engine-enforced ability cost. WC3 semantics:
                        // mana / state cost is spent when the spell's
                        // EFFECT fires (foreswing completes / channel
                        // resolves), NOT at cast start — an interrupted
                        // foreswing or a cancelled channel costs nothing.
                        // We still gate the START on can_afford so a cast
                        // that could never be paid never begins; the
                        // actual deduction happens at the effect points
                        // below. cost is empty for free abilities.
                        auto& lvl = def->level_data(inst->level);
                        if (!simulation::ability_can_afford(world, id, lvl.cost)) {
                            oq->current.reset();  // can't afford — drop the order
                        } else {
                            aset.casting_id       = cast_order->ability_id;
                            aset.cast_target_unit = cast_order->target_unit;
                            aset.cast_target_pos  = cast_order->target_pos;
                            aset.cast_source_item = cast_order->source_item;

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
                            Unit caster = world.unit(id);

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
                                // begins, transition to Backswing. Pay the
                                // cost here (effect point), not at cast start.
                                simulation::ability_pay_cost(world, id, lvl.cost);
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
                        Unit caster = world.unit(id);

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
                            // Channel resolved → effect fires. Pay the cost
                            // here (effect point); an interrupted channel
                            // above returns without paying.
                            simulation::ability_pay_cost(world, id, lvl.cost);
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

                Unit source_unit = world.unit(id);

                // Duration must outlast the scan interval so a unit that
                // stays in radius is refreshed before its buff lapses.
                // The applied ability's remaining_duration is decremented
                // by dt (game-speed-scaled) each tick, so express the
                // window in the same dt units — `(scan_interval + slack)`
                // ticks worth of dt — otherwise at >1x game speed the
                // buff expires mid-interval and flickers.
                f32 aura_duration = static_cast<f32>(AURA_SCAN_INTERVAL + 2) * dt;
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
                Unit unit_h = world.unit(id);
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

            Unit unit_h = world.unit(id);

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
//
// Two paths share the same per-tick loop:
//   • Homing  — re-aim at target each tick; on proximity, fire hit +
//               auto-destroy.
//   • Linear  — fixed velocity toward target_pos; pierce-by-default
//               (every unit within hit_radius along the path fires
//               PROJECTILE_HIT once); destroy on max_distance or
//               lifetime cap.
//
// PROJECTILE_HIT fires via world.on_projectile_hit before any engine-
// side damage handler. PROJECTILE_DESTROYED fires via
// world.on_projectile_destroyed once per projectile, on every destroy
// path. The engine's own attack-damage routing (is_attack projectiles)
// happens AFTER the hit callback so Lua interception lands first.

static void destroy_projectile_entity(World& world, u32 id) {
    auto* info = world.handle_infos.get(id);
    if (!info) return;
    Handle h; h.id = id; h.generation = info->generation;
    world.transforms.remove(id);
    world.handle_infos.remove(id);
    world.projectiles.remove(id);
    world.renderables.remove(id);
    world.anim_queues.remove(id);
    world.handles.free(h);
}

// Window the projectile lingers in the Dying state so its death clip
// can play out before teardown. Generous enough for typical glTF
// death clips (most well under 1.5 s); for static / non-skinned
// projectiles this is just the cleanup grace period and unused
// visually. Long clips (> 1.5 s) get cut short — author should keep
// the death clip brief or we should plumb the model's actual clip
// duration in (TODO: render→sim query for clip length on Dying entry).
static constexpr f32 kProjectileDeathAnimSecs = 1.5f;

// Begin destruction. Fires PROJECTILE_DESTROYED once (gameplay end),
// queues the "death" clip on the renderer's anim queue, marks the
// projectile dying, sets the teardown timer. The actual entity removal
// happens in system_projectile after death_timer drains.
static void begin_destroy_projectile(World& world, u32 id) {
    auto* p = world.projectiles.get(id);
    if (!p || p->dying) return;
    // Size the linger window to the actual death clip duration so the
    // projectile vanishes exactly when the animation finishes. Falls
    // back to the small constant if the renderer hasn't installed the
    // resolver or the model has no "death" clip.
    f32 timer = kProjectileDeathAnimSecs;
    if (world.get_clip_duration) {
        auto* r = world.renderables.get(id);
        if (r && !r->model_path.empty()) {
            f32 dur = world.get_clip_duration(r->model_path, "death");
            if (dur > 0.0f) timer = dur;
        }
    }
    Unit pu = world.unit(id);
    if (world.on_projectile_destroyed) world.on_projectile_destroyed(pu);
    p->dying       = true;
    p->death_timer = timer;
    // Queue the "death" clip. If the model has a "death" clip the
    // renderer plays it; if not, the renderer falls back to the bind
    // pose / current clip — harmless either way.
    AnimQueue aq;
    aq.clips.push_back("death");
    aq.looping = false;
    if (auto* q = world.anim_queues.get(id)) {
        *q = std::move(aq);
    } else {
        world.anim_queues.add(id, std::move(aq));
    }
}

// Public entry — explicit DestroyProjectile from Lua.
void destroy_projectile(World& world, Unit proj_unit) {
    if (!world.validate(proj_unit)) return;
    if (!world.projectiles.get(proj_unit.id)) return;
    begin_destroy_projectile(world, proj_unit.id);
}

void system_projectile(World& world, float dt) {
    std::vector<u32> to_teardown;

    for (u32 i = 0; i < world.projectiles.count(); ++i) {
        u32 id = world.projectiles.ids()[i];
        auto& proj = world.projectiles.data()[i];
        auto* transform = world.transforms.get(id);
        if (!transform) continue;

        // Dying: gameplay has ended; entity persists for the death
        // animation. Tick the death timer; when it hits zero, queue
        // the entity for actual teardown.
        if (proj.dying) {
            proj.death_timer -= dt;
            if (proj.death_timer <= 0) to_teardown.push_back(id);
            continue;
        }

        // Pre-emit projectiles sit at the source point — no movement,
        // no collision. Lua is still configuring side state.
        if (!proj.emitted) continue;

        proj.elapsed += dt;
        if (proj.elapsed >= proj.max_lifetime) { begin_destroy_projectile(world, id); continue; }

        if (proj.path == ProjectileComp::Path::Homing) {
            // Re-aim each tick. Target loss → drop on last known position
            // (the simplest of the on_target_lost options; configurable
            // variants can be added later).
            glm::vec3 aim;
            if (world.validate(proj.target)) {
                auto* tt = world.transforms.get(proj.target.id);
                if (tt) { aim = tt->position; aim.z += 32.0f; proj.target_pos = aim; }
                else    { begin_destroy_projectile(world, id); continue; }
            } else {
                aim = proj.target_pos;
            }
            glm::vec3 to = aim - transform->position;
            f32 dist = glm::length(to);
            if (dist < proj.hit_radius) {
                Unit pu = world.unit(id);
                if (world.validate(proj.target) && world.on_projectile_hit) {
                    world.on_projectile_hit(pu, proj.target);
                }
                if (proj.is_attack && world.validate(proj.target)) {
                    deal_attack_damage(world, proj.source, proj.target, proj.damage);
                }
                begin_destroy_projectile(world, id);
                continue;
            }
            f32 step = proj.speed * dt;
            if (step > dist) step = dist;
            transform->position += (to / dist) * step;
            // Face direction of travel so the model points along its
            // path — important for arrows / arms etc.
            transform->facing = std::atan2(to.y, to.x);
            proj.traveled += step;
        } else {
            // Linear: fly toward target_pos. Pierce — every unit within
            // hit_radius that we haven't hit yet fires the hit callback.
            glm::vec3 to = proj.target_pos - transform->position;
            f32 dist = glm::length(to);
            glm::vec3 dir = (dist > 1e-3f) ? (to / dist) : glm::vec3{1, 0, 0};
            f32 step = proj.speed * dt;
            if (step > dist) step = dist;
            transform->position += dir * step;
            transform->facing = std::atan2(dir.y, dir.x);
            proj.traveled += step;

            // Per-tick scan: any unit within hit_radius that isn't
            // already in the already_hit list and isn't the source.
            // All identity comparisons are generation-checked: a raw-id
            // match could alias a recycled handle (unit removed mid-
            // flight, id reused for a new unit) and wrongly skip the new
            // occupant.
            Unit pu = world.unit(id);
            for (u32 j = 0; j < world.transforms.count(); ++j) {
                u32 oid = world.transforms.ids()[j];
                if (oid == id) continue;
                auto* info = world.handle_infos.get(oid);
                if (!info || info->category != Category::Unit) continue;

                // Build the candidate's full handle once for identity
                // checks below.
                Unit cand;
                cand.id = oid;
                cand.generation = info->generation;

                // Skip the source — but only if THIS unit is genuinely
                // the source (same generation). A recycled source id
                // belonging to a different unit must still be hittable.
                if (cand.id == proj.source.id && cand.generation == proj.source.generation) continue;

                auto* hp = world.healths.get(oid);
                if (!hp || hp->current <= 0) continue;
                bool already = false;
                for (const Unit& done : proj.already_hit) {
                    if (done.id == cand.id && done.generation == cand.generation) { already = true; break; }
                }
                if (already) continue;
                const auto& tf = world.transforms.data()[j];
                glm::vec3 delta = tf.position - transform->position;
                delta.z = 0;
                if (glm::length(delta) <= proj.hit_radius) {
                    Unit hit = world.unit(oid);
                    if (world.on_projectile_hit) world.on_projectile_hit(pu, hit);
                    if (proj.is_attack) {
                        deal_attack_damage(world, proj.source, hit, proj.damage);
                    }
                    proj.already_hit.push_back(hit);
                }
            }

            // Expire on max_distance or on reaching target_pos with no
            // remaining travel.
            if ((proj.max_distance > 0 && proj.traveled >= proj.max_distance) ||
                dist <= 1e-3f) {
                begin_destroy_projectile(world, id);
                continue;
            }
        }
    }

    for (u32 id : to_teardown) {
        destroy_projectile_entity(world, id);
    }
}

// ── Death + corpse system ─────────────────────────────────────────────────

static void remove_all_components_and_free(World& world, Handle h) {
    // Route through the canonical teardown so this corpse-cleanup path
    // can't drift from the master pool list. (It used to be a hand-
    // maintained copy that had silently lost status_flags / anim_queues
    // / true_sight_vis / forced_vis — a dead unit's stun/silence/invis
    // refcounts bled onto whatever new unit recycled its handle id.)
    remove_all_components(world, h.id);
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

            // Push resolves ANY overlap regardless of owner. Move-time
            // blocking (foreign_unit_blocks in system_movement) already
            // stops a unit walking INTO a different player's unit, so the
            // only overlaps that reach here are same-player crowding and
            // residuals from teleport / knockback / SetUnitX|Y — all of
            // which must de-overlap no matter who owns the two units.
            auto* other_t = world.transforms.get(other.id);
            if (!other_t) continue;
            auto* other_mov = world.movements.get(other.id);
            f32 other_radius = other_mov ? other_mov->collision_radius : 32.0f;
            f32 min_dist = self_radius + other_radius;

            glm::vec3 diff = transform->position - other_t->position;
            diff.z = 0;
            f32 d = glm::length(diff);

            if (d < min_dist) {
                const Owner* self_owner  = world.owners.get(id);
                const Owner* other_owner = world.owners.get(other.id);
                MoveType other_type = other_mov ? other_mov->type : MoveType::Ground;
                // A push is just another position change, so it obeys the
                // SAME rule as a voluntary step: never drive a unit DEEPER
                // into a foreign unit's circle. Without this, a chain of
                // same-player pushes (A→B) can shove B into an enemy C, then
                // the B↔C pair shoves C — letting a player push enemies by
                // proxy. foreign_unit_blocks' escape valve still lets a unit
                // teleported ONTO an enemy push outward to de-overlap.
                auto push_ok = [&](glm::vec2 from, glm::vec2 to, const Owner* owner,
                                   f32 radius, MoveType mt) -> bool {
                    if (!pathfinder.can_move_to(from.x, from.y, to.x, to.y, mt)) return false;
                    if (foreign_unit_blocks(world, grid, /*self_id unused for push*/ UINT32_MAX,
                                            owner, radius, from, to, mt)) return false;
                    return true;
                };

                if (d > 0.01f) {
                    glm::vec3 n = diff / d;
                    f32 half = (min_dist - d) * 0.5f;
                    f32 ax = transform->position.x + n.x * half;
                    f32 ay = transform->position.y + n.y * half;
                    f32 bx = other_t->position.x - n.x * half;
                    f32 by = other_t->position.y - n.y * half;
                    if (push_ok({transform->position.x, transform->position.y}, {ax, ay},
                                self_owner, self_radius, mov->type)) {
                        transform->position.x = ax;
                        transform->position.y = ay;
                    }
                    if (push_ok({other_t->position.x, other_t->position.y}, {bx, by},
                                other_owner, other_radius, other_type)) {
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

void system_death(World& world, float dt) {
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
                // Same exposure for `info`: on_dying could have spawned
                // a unit (Reincarnation / summon-on-death), reallocating
                // the handle_infos dense vector and dangling the pointer
                // cached above. Everything below reads info->type_id /
                // category / generation, so refresh it here too.
                info = world.handle_infos.get(id);
                if (!info) continue;
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

        dead.corpse_timer += dt;  // game-speed-scaled; corpses linger in game-time, not real-time

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
    // Note: `contained` is kept in sync below even when no script
    // consumer (on_region_enter / on_region_leave) is wired, so
    // IsUnitInRegion queries stay correct; only the diff dispatch is
    // skipped per-region when there's no callback.

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
            world.on_region_enter(rid, world.unit(uid));
        }
        for (u32 uid : region.contained) {
            if (current.count(uid)) continue;
            if (!world.on_region_leave) continue;
            world.on_region_leave(rid, world.unit(uid));
        }

        region.contained = std::move(current);
        current.clear();  // moved-from state may be unspecified; restore.
    }
}

} // namespace uldum::simulation
