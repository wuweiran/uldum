#pragma once

#include "simulation/command_system.h"
#include "simulation/selection.h"
#include "input/picking.h"
#include "input/input_bindings.h"
#include "platform/platform.h"
#include "render/camera.h"
#include "simulation/simulation.h"
#include "core/types.h"

#include <glm/vec3.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace uldum::hud { class Hud; }

namespace uldum::input {

// Everything an input preset needs to do its job.
struct InputContext {
    const platform::InputState& input;
    simulation::SelectionState& selection;
    simulation::CommandSystem&  commands;
    Picker&                     picker;
    render::Camera&             camera;
    const InputBindings&        bindings;
    const simulation::Simulation& simulation;
    u32 screen_w;
    u32 screen_h;
    // True when the HUD has captured pointer input for this frame (hover
    // over a slot or tree widget, or a press ongoing over one). Presets
    // use this to skip pointer-initiated gameplay actions — selection
    // box drag, right-click orders — that would otherwise fight the UI.
    // Keyboard-only actions (hotkeys, camera pan) are unaffected.
    bool hud_captured = false;
    // True when the pointer is over the minimap panel this frame. The minimap
    // is a world proxy: ground orders / ground-target abilities commit at the
    // clicked point (the picker maps it through the minimap transform). So a
    // preset lets pointer-driven ORDERS through even though hud_captured is
    // set — but keeps SELECTION suppressed (no box-select starting on the
    // minimap). Desktop-only in practice; touch uses the minimap for camera
    // drag, which runs on a separate path.
    bool hud_minimap_hovered = false;
    // True for the duration of a minimap drag (press to release). Presets
    // that auto-follow a unit (ActionPreset) suspend the follow while
    // this is set, so the player can pan the camera by dragging on the
    // minimap without the follow snapping it back every frame.
    bool hud_minimap_dragging = false;
    // True when *any* finger is captured by the virtual joystick. Used
    // by ActionPreset's two-finger pan handler to suppress pan/pinch
    // when one finger is driving the stick — otherwise "joystick +
    // ability button" reads as a two-finger pan and lurches the camera.
    // hud_captured already covers joystick-on-primary-slot, but a
    // secondary finger on the stick (joystick_rt.captured_slot != 0)
    // wouldn't trip hud_captured.
    bool hud_joystick_active = false;
    // Sub-tick interpolation factor (0..1) for the current frame — same
    // value the renderer uses. Presets that follow entity positions
    // (e.g. the Action preset's hero-tracking camera) read this so
    // they stay visually in sync with the interpolated unit render.
    f32 alpha = 1.0f;
    // Virtual-stick output for this frame, normalized to [-1, 1]². Zero
    // when the HUD joystick composite isn't present or no finger is on
    // it. Screen-space axes — presets decide what they mean (the RTS
    // preset pans the camera; the action preset drives unit movement).
    // `(0, 0)` is center / idle; `(+x, +y)` is the screen's bottom-right
    // quadrant.
    f32 joystick_x = 0.0f;
    f32 joystick_y = 0.0f;

    // Optional HUD handle for presets that maintain HUD-side state
    // (Action preset's focus_target lives in HUD, not the sim, since
    // it's purely a local view concept). May be null on dedicated
    // server / non-rendering paths.
    hud::Hud* hud = nullptr;

    // Callback fired when a right-click resolves to a unit / item /
    // destructable / ground-attack-move target. The app uses it to
    // flash a brief WC3-style "target acquired" ring at the target.
    // Color comes from `kind` — Hostile=red, Friendly=green, Item=yellow.
    // Purely visual — no gameplay effect. May be empty; presets check
    // before calling. `unit` is invalid when the click was a ground
    // target (no entity was hit), in which case the renderer uses
    // `world_pos` directly.
    // Colors a target ping by intent relative to the commanding player.
    // Neutral covers everything that isn't an owned unit — items, ownerless
    // destructables (crates, trees), neutral-passive entities — they all
    // share the neutral palette entry rather than each carrying a kind.
    enum class TargetPingKind : u8 { Enemy, Ally, Neutral };
    std::function<void(simulation::Unit unit, glm::vec3 world_pos, TargetPingKind kind)> target_ping_fn;
};

// Base class for input presets. Each preset translates raw input into
// selections, commands, and camera movement.
class InputPreset {
public:
    virtual ~InputPreset() = default;
    virtual void update(const InputContext& ctx, f32 dt) = 0;

    // Ask the preset to trigger an ability as if the user had pressed
    // its hotkey. Fired from outside the input loop (HUD slot click).
    // The app orders HUD pointer dispatch before the preset update each
    // frame, so a click queued here is consumed by the same update()
    // call's trailing flush — zero extra latency. Default impl no-ops
    // for presets that don't support ability casts.
    virtual void queue_ability(std::string_view /*ability_id*/) {}

    // Ask the preset to run one of the engine-built-in commands —
    // `"stop"`, `"hold_position"`, `"attack"`, `"attack_move"`,
    // `"move"`, `"patrol"`. Instant ones submit orders immediately;
    // targeting ones enter a mode where the next world-click commits.
    // Same zero-latency processing as queue_ability.
    virtual void queue_command(std::string_view /*command_id*/) {}

    // Id of the ability currently in "pick a target" mode, or empty
    // when the preset isn't waiting for a cast target. Used by the HUD
    // to highlight the armed slot so the player knows which ability
    // is about to fire. Default: never armed.
    virtual std::string_view targeting_ability_id() const { return {}; }

    // Command id currently in targeting mode — mirror of the above,
    // for the command_bar composite. Empty when no command is armed.
    // Today: "attack" / "attack_move" (→ m_attack_move_mode), "move"
    // (→ m_move_targeting_mode). Instant commands (stop / hold) never
    // stay armed. Default: never armed.
    virtual std::string_view active_command_id() const { return {}; }

    // Whether ground-plane selection rings should render for this
    // preset. RTS-style presets return true (you're commanding any
    // number of units and need to see which are selected); action-
    // style presets return false (the camera already tracks the
    // player hero, so the ring is just clutter). Default: true.
    virtual bool show_selection_circles() const { return true; }

    // ── Targeting modes ────────────────────────────────────────────
    // The preset can be in one of several "waiting for a target"
    // modes (cast an ability, pick a move-to point, pick an attack-
    // move point). Together with the HUD's held-item state they
    // form the engine's full set of "next-click pending" UI states;
    // every entry path mutually excludes the others.
    //
    // `is_targeting()` is the union test the app uses to mirror
    // mutual exclusion against the HUD: when this turns true (any
    // preset targeting mode just entered), the app cancels the
    // HUD's held item; when the HUD lifts an item, the app calls
    // `cancel_targeting()` so the preset relinquishes any active
    // mode. Default: never targeting.
    virtual bool is_targeting() const { return false; }
    virtual void cancel_targeting() {}

    // Frame-selection drag state. `active == true` while the player is
    // dragging a rectangle across the world to box-select units. Coords
    // are screen-space physical pixels (the same space the pointer
    // hit-tests HUD atoms in). App reads this each frame and draws a
    // marquee via the HUD. Default: never dragging.
    struct BoxSelection {
        bool active = false;
        f32  x0 = 0, y0 = 0;   // drag start
        f32  x1 = 0, y1 = 0;   // current pointer
    };
    virtual BoxSelection box_selection() const { return {}; }
};

// Create an input preset by name ("rts", future: "action_rpg").
std::unique_ptr<InputPreset> create_preset(std::string_view name);

// The WC3-style "target acquired" ping is feedback for *local input* only —
// it must never fire on script / AI orders, so it's derived here at the
// input layer (not in CommandSystem::submit, which scripts also go through)
// and emitted once at each human-input commit site. The color is a property
// of *what you targeted*, not which order you issued: pull the target out of
// the order, then color by its relationship to the commanding player.
//   • target is an enemy unit → Enemy  (red)
//   • target is an ally  unit → Ally   (green)
//   • target is an item       → Item   (yellow)
//   • no target (ground move / attack-move / non-target cast) → no ping
// Coloring by target (not order kind) means move/attack/cast onto the same
// unit all ping the same — and new targeted order types are handled for free.
// Position is sampled from the target's current transform. Returns nullopt
// when the order isn't target-bound or the target has no transform.
struct DerivedPing {
    simulation::Unit unit;
    glm::vec3        pos;
    InputContext::TargetPingKind kind;
};
std::optional<DerivedPing> derive_target_ping(const simulation::GameCommand& cmd,
                                              const simulation::Simulation& sim);

} // namespace uldum::input
