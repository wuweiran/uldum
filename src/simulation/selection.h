#pragma once

#include "simulation/entity_types.h"
#include "core/types.h"

#include <array>
#include <functional>
#include <vector>

namespace uldum::simulation {

struct World;

static constexpr u32 MAX_SELECTION = 24;
static constexpr u32 NUM_CONTROL_GROUPS = 10;

// Per-player selection state. Lives on the client side — the server
// doesn't track which units a given player has selected; selection is
// pure UI intent. Server's script engine references the type through
// a nullable pointer so headless builds (no UI) leave it unset.
class SelectionState {
public:
    void set_player(Player player) { m_player = player; }
    Player player() const { return m_player; }

    // ── Selection ─────────────────────────────────────────────────────────
    // Two shapes, enforced at the input layer: N of your own units
    // (commandable), or exactly one other widget (foreign unit / crate,
    // view-only). Never mixed — so the first element decides for the whole set.

    const std::vector<Widget>& selected() const { return m_selected; }
    bool empty() const { return m_selected.empty(); }
    u32  count() const { return static_cast<u32>(m_selected.size()); }

    // Command recipients: the selection as units, but empty unless the first
    // element is one of the local player's own units. This is why orders never
    // reach a view-only widget — no recipients.
    std::vector<Unit> selected_units(const World& world) const;

    void select(Widget widget);                    // single widget (any)
    void select_multiple(const World& world, std::vector<Unit> units); // own-units group
    void toggle(const World& world, Unit unit);    // shift-click own unit
    void clear();
    bool is_selected(Widget widget) const;

    // Selection change callback (fired after any mutation).
    std::function<void()> on_change;

    // ── Control groups ────────────────────────────────────────────────────
    // Groups hold own units only, so mutators take the world to resolve
    // the current selection to its command recipients.
    void assign_group(const World& world, u32 group);
    void recall_group(u32 group);
    void add_to_group(const World& world, u32 group);

private:
    Player m_player;
    std::vector<Widget> m_selected;
    std::array<std::vector<Unit>, NUM_CONTROL_GROUPS> m_groups;
};

} // namespace uldum::simulation
