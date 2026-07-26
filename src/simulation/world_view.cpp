#include "simulation/world_view.h"
#include "simulation/world.h"

#include <type_traits>

namespace uldum::simulation {

const Transform*              WorldView::transform(u32 id)      const { return m_world->transforms.get(id); }
const Renderable*             WorldView::renderable(u32 id)     const { return m_world->renderables.get(id); }
const Health*                 WorldView::health(u32 id)         const { return m_world->healths.get(id); }
const Player*                 WorldView::owner(u32 id)          const { return m_world->owners.get(id); }
const HandleInfo*             WorldView::handle_info(u32 id)    const { return m_world->handle_infos.get(id); }
const Movement*               WorldView::movement(u32 id)       const { return m_world->movements.get(id); }
const Combat*                 WorldView::combat(u32 id)         const { return m_world->combats.get(id); }
const Selectable*             WorldView::selectable(u32 id)     const { return m_world->selectables.get(id); }
bool                          WorldView::is_dead(u32 id)        const { return health_is_dead(m_world->healths.get(id)); }
const StatusFlags*            WorldView::status(u32 id)         const { return m_world->status_flags.get(id); }
const UnitClassificationComp* WorldView::classification(u32 id) const { return m_world->classifications.get(id); }
const AbilitySet*             WorldView::ability_set(u32 id)    const { return m_world->ability_sets.get(id); }
const StateBlock*             WorldView::state_block(u32 id)    const { return m_world->state_blocks.get(id); }
const ProjectileComp*         WorldView::projectile(u32 id)     const { return m_world->projectiles.get(id); }
const ForcedVisibility*       WorldView::forced_visibility(u32 id) const { return m_world->forced_vis.get(id); }
const TrueSightVisibility*    WorldView::true_sight(u32 id)     const { return m_world->true_sight_vis.get(id); }
const DestructableComp*       WorldView::destructable(u32 id)   const { return m_world->destructables.get(id); }
const DoodadComp*             WorldView::doodad(u32 id)         const { return m_world->doodads.get(id); }
const ItemInfo*               WorldView::item_info(u32 id)      const { return m_world->item_infos.get(id); }
const Carriable*              WorldView::carriable(u32 id)      const { return m_world->carriables.get(id); }
const Inventory*              WorldView::inventory(u32 id)      const { return m_world->inventories.get(id); }
const AnimQueue*              WorldView::anim_queue(u32 id)     const { return m_world->anim_queues.get(id); }

bool                WorldView::contains(u32 id) const { return m_world->handle_infos.has(id); }
const TypeRegistry* WorldView::type_registry()  const { return m_world->types; }

std::span<const u32> WorldView::renderable_ids() const { return m_world->renderables.ids(); }
std::span<const u32> WorldView::transform_ids()  const { return m_world->transforms.ids(); }
std::span<const u32> WorldView::selectable_ids() const { return m_world->selectables.ids(); }
std::span<const u32> WorldView::item_info_ids()  const { return m_world->item_infos.ids(); }

void WorldView::size_selectable(u32 id, f32 radius, f32 height) {
    if (auto* s = m_world->selectables.get(id)) { s->selection_radius = radius; s->selection_height = height; }
}
void WorldView::clear_anim_queue(u32 id) { m_world->anim_queues.remove(id); }
AnimQueue* WorldView::anim_queue_mut(u32 id) { return m_world->anim_queues.get(id); }

// ── LocalView ────────────────────────────────────────────────────────────────
// Read resolver: snapshot → live auth → nullptr. `snapshot_pool` is the matching
// snapshot SparseSet, or nullptr for pools a snapshotted static doesn't carry
// (those return null when snapshotted and live-auth when visible).
namespace {
template <typename T>
const T* resolve(const LocalView& v, u32 id,
                 const SparseSet<T>* snapshot_pool,
                 const SparseSet<T>& auth_pool) {
    if (v.snapshotted(id)) return snapshot_pool ? snapshot_pool->get(id) : nullptr;
    if (v.visible.count(id)) return auth_pool.get(id);
    return nullptr;
}
} // namespace

const Transform* LocalView::transform(u32 id) const {
    return resolve(*this, id, &snapshot.transforms, source->transforms);
}
const Renderable* LocalView::renderable(u32 id) const {
    return resolve(*this, id, &snapshot.renderables, source->renderables);
}
const Health* LocalView::health(u32 id) const {
    return resolve<Health>(*this, id, nullptr, source->healths);
}
const Player* LocalView::owner(u32 id) const {
    return resolve<Player>(*this, id, nullptr, source->owners);
}
const HandleInfo* LocalView::handle_info(u32 id) const {
    return resolve(*this, id, &snapshot.handle_infos, source->handle_infos);
}
const Movement* LocalView::movement(u32 id) const {
    return resolve<Movement>(*this, id, nullptr, source->movements);
}
const Combat* LocalView::combat(u32 id) const {
    return resolve<Combat>(*this, id, nullptr, source->combats);
}
bool LocalView::is_dead(u32 id) const {
    return health_is_dead(health(id));
}
const StatusFlags* LocalView::status(u32 id) const {
    return resolve<StatusFlags>(*this, id, nullptr, source->status_flags);
}
const UnitClassificationComp* LocalView::classification(u32 id) const {
    return resolve(*this, id, &snapshot.classifications, source->classifications);
}
const AbilitySet* LocalView::ability_set(u32 id) const {
    return resolve<AbilitySet>(*this, id, nullptr, source->ability_sets);
}
const StateBlock* LocalView::state_block(u32 id) const {
    return resolve<StateBlock>(*this, id, nullptr, source->state_blocks);
}
const ProjectileComp* LocalView::projectile(u32 id) const {
    return resolve<ProjectileComp>(*this, id, nullptr, source->projectiles);
}
const ForcedVisibility* LocalView::forced_visibility(u32 id) const {
    return resolve<ForcedVisibility>(*this, id, nullptr, source->forced_vis);
}
const TrueSightVisibility* LocalView::true_sight(u32 id) const {
    return resolve<TrueSightVisibility>(*this, id, nullptr, source->true_sight_vis);
}
const DestructableComp* LocalView::destructable(u32 id) const {
    return resolve(*this, id, &snapshot.destructables, source->destructables);
}
const DoodadComp* LocalView::doodad(u32 id) const {
    return resolve<DoodadComp>(*this, id, nullptr, source->doodads);
}
const ItemInfo* LocalView::item_info(u32 id) const {
    return resolve<ItemInfo>(*this, id, nullptr, source->item_infos);
}
const Carriable* LocalView::carriable(u32 id) const {
    return resolve<Carriable>(*this, id, nullptr, source->carriables);
}
const Inventory* LocalView::inventory(u32 id) const {
    return resolve<Inventory>(*this, id, nullptr, source->inventories);
}

// Selectables + anim queues are owned scratch — read them directly (they exist
// for both live and snapshotted entities, seeded by project_local_view).
const Selectable* LocalView::selectable(u32 id) const { return own_selectables.get(id); }
const AnimQueue*  LocalView::anim_queue(u32 id) const { return own_anim_queues.get(id); }

bool LocalView::contains(u32 id) const { return snapshotted(id) || visible.count(id) != 0; }
const TypeRegistry* LocalView::type_registry() const { return source ? source->types : nullptr; }

std::span<const u32> LocalView::renderable_ids() const { return iter_renderables; }
std::span<const u32> LocalView::transform_ids()  const { return iter_transforms; }
std::span<const u32> LocalView::selectable_ids() const { return own_selectables.ids(); }
std::span<const u32> LocalView::item_info_ids()  const { return iter_item_infos; }

void LocalView::size_selectable(u32 id, f32 radius, f32 height) {
    if (auto* s = own_selectables.get(id)) { s->selection_radius = radius; s->selection_height = height; }
}
void LocalView::clear_anim_queue(u32 id) { own_anim_queues.remove(id); }
AnimQueue* LocalView::anim_queue_mut(u32 id) { return own_anim_queues.get(id); }

void LocalView::clear() {
    visible.clear();
    snapshot.transforms.clear();
    snapshot.renderables.clear();
    snapshot.handle_infos.clear();
    snapshot.classifications.clear();
    snapshot.destructables.clear();
    snapshot_hidden_seen.clear();
    own_selectables.clear();
    own_anim_queues.clear();
    iter_renderables.clear();
    iter_transforms.clear();
    iter_item_infos.clear();
}

void LocalView::snapshot_from(const World& src, u32 id) {
    auto snap = [&](auto& dst, const auto& pool) {
        if (const auto* c = pool.get(id)) {
            if (!dst.has(id)) dst.add(id, std::remove_cvref_t<decltype(*c)>{*c});
        }
    };
    snap(snapshot.handle_infos,    src.handle_infos);
    snap(snapshot.transforms,      src.transforms);
    snap(snapshot.renderables,     src.renderables);
    snap(snapshot.classifications, src.classifications);
    snap(snapshot.destructables,   src.destructables);
}

void LocalView::drop_snapshot(u32 id) {
    snapshot.handle_infos.remove(id);
    snapshot.transforms.remove(id);
    snapshot.renderables.remove(id);
    snapshot.classifications.remove(id);
    snapshot.destructables.remove(id);
    snapshot_hidden_seen.erase(id);
}

// ── Shared view-resolver free functions ──────────────────────────────────────
// The IWorldView overloads of the cross-layer helpers live here (co-located with
// the resolvers they read through), not scattered across simulation.cpp /
// world.cpp / systems.cpp. Each is self-contained — the auth `const World&`
// twin (in its own module) reads the auth pools directly. Out-params live only
// on the overload whose caller actually needs one: `can_attack_target`'s
// out_specifier on the auth twin (input reject feedback); `ability_can_afford`'s
// out_lacking on this view overload (HUD reject feedback).

bool is_static_remembered_entity(const IWorldView& world, u32 entity_id) {
    const auto* info = world.handle_info(entity_id);
    if (!info) return false;
    if (info->category == Category::Destructable) return true;
    if (info->category == Category::Doodad) return true;
    if (info->category == Category::Unit) {
        const auto* cls = world.classification(entity_id);
        if (cls && has_classification(cls->flags, "structure")) return true;
    }
    return false;
}

f32 unit_fly_height(const IWorldView& world, u32 id) {
    const auto* mov = world.movement(id);
    if (!mov || mov->type != MoveType::Fly) return 0.0f;
    const auto* types = world.type_registry();
    if (!types) return 0.0f;
    const auto* hi = world.handle_info(id);
    if (!hi) return 0.0f;
    const auto* def = types->get_unit_type(hi->type_id);
    return def ? def->fly_height : 0.0f;
}

bool ability_can_afford(const IWorldView& world, u32 unit_id,
                        const std::map<std::string, f32>& cost,
                        std::string* out_lacking) {
    if (cost.empty()) return true;
    for (const auto& [state_name, amount] : cost) {
        if (amount <= 0.0f) continue;
        if (state_name == "health") {
            const auto* hp = world.health(unit_id);
            if (!hp || hp->current <= amount) { if (out_lacking) *out_lacking = state_name; return false; }
            continue;
        }
        const auto* sb = world.state_block(unit_id);
        if (!sb) { if (out_lacking) *out_lacking = state_name; return false; }
        auto it = sb->states.find(state_name);
        if (it == sb->states.end() || it->second.current < amount) {
            if (out_lacking) *out_lacking = state_name;
            return false;
        }
    }
    return true;
}

bool can_attack_target(const IWorldView& world, u8 target_mask, Widget target) {
    u8 target_bits;
    if (const auto* d = world.destructable(target.id)) {
        target_bits = d->target_bit;
    } else if (const auto* cls = world.classification(target.id)) {
        target_bits = target_class_from_classifications(cls->flags);
    } else {
        target_bits = TARGET_BIT_GROUND;
    }
    return (target_mask & target_bits) != 0;
}

} // namespace uldum::simulation
