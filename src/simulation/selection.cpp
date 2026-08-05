#include "simulation/selection.h"
#include "simulation/world.h"

#include <algorithm>

namespace uldum::simulation {

void SelectionState::select(Widget widget) {
    m_selected.clear();
    m_selected.push_back(widget);
    if (on_change) on_change();
}

void SelectionState::select_multiple(std::vector<Unit> units) {
    if (units.size() > MAX_SELECTION)
        units.resize(MAX_SELECTION);
    m_selected.assign(units.begin(), units.end());   // Unit → Widget (upcast)
    if (on_change) on_change();
}

void SelectionState::toggle(Unit unit) {
    Widget w{unit.id};
    auto it = std::find(m_selected.begin(), m_selected.end(), w);
    if (it != m_selected.end()) {
        m_selected.erase(it);
    } else if (m_selected.size() < MAX_SELECTION) {
        m_selected.push_back(w);
    }
    if (on_change) on_change();
}

void SelectionState::clear() {
    m_selected.clear();
    if (on_change) on_change();
}

std::vector<Unit> SelectionState::selected_units(const World& world) const {
    // Own-units shape only (foreign/crate selection → no recipients). Selection
    // is homogeneous, so the first element decides.
    if (m_selected.empty()) return {};
    const auto* lead_owner = world.owners.get(m_selected.front().id);
    if (!lead_owner || lead_owner->id != m_player.id) return {};
    std::vector<Unit> out;
    out.reserve(m_selected.size());
    for (Widget w : m_selected) {
        const auto* info = world.handle_infos.get(w.id);
        if (info && !info->hidden) out.push_back(Unit{w.id});
    }
    return out;
}

bool SelectionState::is_selected(Widget widget) const {
    return std::find(m_selected.begin(), m_selected.end(), widget) != m_selected.end();
}

void SelectionState::assign_group(const World& world, u32 group) {
    if (group >= NUM_CONTROL_GROUPS) return;
    m_groups[group] = selected_units(world);
}

void SelectionState::recall_group(u32 group) {
    if (group >= NUM_CONTROL_GROUPS) return;
    // Unassigned groups leave the current selection alone — RTS
    // convention. Overwriting with an empty vector would look like
    // pressing the key deselects, which was confusing.
    if (m_groups[group].empty()) return;
    m_selected.assign(m_groups[group].begin(), m_groups[group].end());   // Unit → Widget
    if (on_change) on_change();
}

void SelectionState::add_to_group(const World& world, u32 group) {
    if (group >= NUM_CONTROL_GROUPS) return;
    for (Unit u : selected_units(world)) {
        if (m_groups[group].size() >= MAX_SELECTION) break;
        auto it = std::find(m_groups[group].begin(), m_groups[group].end(), u);
        if (it == m_groups[group].end()) {
            m_groups[group].push_back(u);
        }
    }
}

} // namespace uldum::simulation
