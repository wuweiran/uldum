#include "input/selection.h"

#include <algorithm>

namespace uldum::input {

void SelectionState::select(simulation::Unit unit) {
    m_selected.clear();
    m_selected.push_back(unit);
    if (on_change) on_change();
}

void SelectionState::select_multiple(std::vector<simulation::Unit> units) {
    if (units.size() > MAX_SELECTION)
        units.resize(MAX_SELECTION);
    m_selected = std::move(units);
    if (on_change) on_change();
}

void SelectionState::toggle(simulation::Unit unit) {
    auto it = std::find(m_selected.begin(), m_selected.end(), unit);
    if (it != m_selected.end()) {
        m_selected.erase(it);
    } else if (m_selected.size() < MAX_SELECTION) {
        m_selected.push_back(unit);
    }
    if (on_change) on_change();
}

void SelectionState::clear() {
    m_selected.clear();
    if (on_change) on_change();
}

bool SelectionState::is_selected(simulation::Unit unit) const {
    return std::find(m_selected.begin(), m_selected.end(), unit) != m_selected.end();
}

void SelectionState::assign_group(u32 group) {
    if (group >= NUM_CONTROL_GROUPS) return;
    m_groups[group] = m_selected;
}

void SelectionState::recall_group(u32 group) {
    if (group >= NUM_CONTROL_GROUPS) return;
    m_selected = m_groups[group];
    if (on_change) on_change();
}

void SelectionState::add_to_group(u32 group) {
    if (group >= NUM_CONTROL_GROUPS) return;
    for (auto& u : m_selected) {
        if (m_groups[group].size() >= MAX_SELECTION) break;
        auto it = std::find(m_groups[group].begin(), m_groups[group].end(), u);
        if (it == m_groups[group].end()) {
            m_groups[group].push_back(u);
        }
    }
}

} // namespace uldum::input
