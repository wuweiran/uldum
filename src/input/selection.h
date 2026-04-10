#pragma once

#include "simulation/handle_types.h"
#include "core/types.h"

#include <array>
#include <functional>
#include <vector>

namespace uldum::input {

static constexpr u32 MAX_SELECTION = 24;
static constexpr u32 NUM_CONTROL_GROUPS = 10;

class SelectionState {
public:
    void set_player(simulation::Player player) { m_player = player; }
    simulation::Player player() const { return m_player; }

    // ── Selection ─────────────────────────────────────────────────────────

    const std::vector<simulation::Unit>& selected() const { return m_selected; }
    bool empty() const { return m_selected.empty(); }
    u32  count() const { return static_cast<u32>(m_selected.size()); }

    // Replace selection with a single unit.
    void select(simulation::Unit unit);

    // Replace selection with multiple units (clamped to MAX_SELECTION).
    void select_multiple(std::vector<simulation::Unit> units);

    // Toggle a unit in/out of selection (shift-click).
    void toggle(simulation::Unit unit);

    // Clear selection.
    void clear();

    // Check if a unit is selected.
    bool is_selected(simulation::Unit unit) const;

    // Selection change callback (fired after any mutation).
    std::function<void()> on_change;

    // ── Control groups ────────────────────────────────────────────────────

    // Assign current selection to group N (0-9).
    void assign_group(u32 group);

    // Recall group N — replace current selection.
    void recall_group(u32 group);

    // Add current selection to group N.
    void add_to_group(u32 group);

private:
    simulation::Player m_player;
    std::vector<simulation::Unit> m_selected;
    std::array<std::vector<simulation::Unit>, NUM_CONTROL_GROUPS> m_groups;
};

} // namespace uldum::input
