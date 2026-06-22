#include "input/input_preset.h"
#include "input/rts_preset.h"
#include "input/action_preset.h"
#include "simulation/world.h"
#include "core/log.h"

namespace uldum::input {

std::optional<DerivedPing> derive_target_ping(const simulation::GameCommand& cmd,
                                              const simulation::Simulation& sim) {
    using Kind = InputContext::TargetPingKind;
    namespace o = simulation::orders;
    const auto& world = sim.world();

    // Pull the target unit/item out of whatever order this is. Each order
    // stores its target in a different slot; we only care that there IS one.
    simulation::Unit target{};
    bool is_item = false;
    if (const auto* a = std::get_if<o::Attack>(&cmd.order)) {
        target = a->target;
    } else if (const auto* m = std::get_if<o::Move>(&cmd.order)) {
        target = m->target_unit;                 // invalid for plain ground move
    } else if (const auto* c = std::get_if<o::Cast>(&cmd.order)) {
        target = c->target_unit;                 // invalid for point/no-target casts
    } else if (const auto* p = std::get_if<o::PickupItem>(&cmd.order)) {
        target = simulation::Unit{p->item};
        is_item = true;
    }
    if (!target.is_valid()) return std::nullopt;  // ground / non-target order

    const auto* t = world.transforms.get(target.id);
    if (!t) return std::nullopt;                  // target vanished this frame

    // Color by what the target IS, relative to the commanding player.
    Kind kind = Kind::Ally;
    if (is_item) {
        kind = Kind::Item;
    } else if (const auto* owner = world.owners.get(target.id)) {
        kind = sim.is_enemy(cmd.player, *owner) ? Kind::Enemy : Kind::Ally;
    }
    return DerivedPing{target, t->position, kind};
}

std::unique_ptr<InputPreset> create_preset(std::string_view name) {
    if (name == "rts" || name.empty()) {
        return std::make_unique<RtsPreset>();
    }
    if (name == "action") {
        return std::make_unique<ActionPreset>();
    }
    log::warn("Input", "Unknown input preset '{}', falling back to RTS", name);
    return std::make_unique<RtsPreset>();
}

} // namespace uldum::input
