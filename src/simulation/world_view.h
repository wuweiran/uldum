#pragma once

#include "core/types.h"
#include "simulation/components.h"
#include "simulation/sparse_set.h"
#include "simulation/type_registry.h"

#include <map>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace uldum::simulation {

// Fog render-classification of a VIEW entity — a pure membership fact, NOT a
// visibility query. "Visible" (WC3 gameplay truth) is is_unit_visible_to on the
// AUTH World; this is only "how does the view draw it":
//   Live    — currently in live sight: render + animate normally.
//   Memory  — a static shown from the player's memory (snapshot, out of live
//             sight): dim (kFoggedMemoryDim) + freeze animation on last frame.
//   Hidden  — not in the view at all (never iterated; the enum value exists so
//             fog_mode is total over any id).
enum class FogVis : u8 { Hidden, Memory, Live };

// The read/scratch surface the renderer / picker / HUD bind as `const
// IWorldView&` (reads) or `IWorldView&` (the 3 draw-time mutators), so the same
// downstream code runs regardless of what backs it. Two implementations:
//   • LocalView — the fog projection every runtime mode renders (see below).
//   • WorldView — a thin adapter over one plain World; used by the editor, which
//                 has no fog and renders its world directly.
// Every getter returns a const pointer (null if the entity lacks that
// component). The iteration spans are the only pools any VIEW consumer walks.
struct IWorldView {
    virtual ~IWorldView() = default;

    // ── Per-entity reads ────────────────────────────────────────────────
    virtual const Transform*              transform(u32 id)      const = 0;
    virtual const Renderable*             renderable(u32 id)     const = 0;
    virtual const Health*                 health(u32 id)         const = 0;
    virtual const Player*                 owner(u32 id)          const = 0;
    virtual const HandleInfo*             handle_info(u32 id)    const = 0;
    virtual const Movement*               movement(u32 id)       const = 0;
    virtual const Combat*                 combat(u32 id)         const = 0;
    virtual const Construction*           construction(u32 id)   const = 0;
    virtual const Selectable*             selectable(u32 id)     const = 0;
    // Death is a derived view of health (see health_is_dead) — NOT a component.
    // The client only ever receives health, so this is the one predicate that
    // agrees on host and client. Renderer/HUD read it for corpse pose + material.
    virtual bool                          is_dead(u32 id)        const = 0;
    virtual const StatusFlags*            status(u32 id)         const = 0;
    virtual const UnitClassificationComp* classification(u32 id) const = 0;
    virtual const AbilitySet*             ability_set(u32 id)    const = 0;
    virtual const StateBlock*             state_block(u32 id)    const = 0;
    virtual const ProjectileComp*         projectile(u32 id)     const = 0;
    virtual const ForcedVisibility*       forced_visibility(u32 id) const = 0;
    virtual const TrueSightVisibility*    true_sight(u32 id)     const = 0;
    virtual const DestructableComp*       destructable(u32 id)   const = 0;
    virtual const DoodadComp*             doodad(u32 id)         const = 0;
    virtual const ItemInfo*               item_info(u32 id)      const = 0;
    virtual const Carriable*              carriable(u32 id)      const = 0;
    virtual const Inventory*              inventory(u32 id)      const = 0;
    virtual const AnimQueue*              anim_queue(u32 id)     const = 0;

    virtual bool                contains(u32 id) const = 0;
    virtual const TypeRegistry* type_registry()  const = 0;

    // Render-classification for id (Live / Memory / Hidden). A pure membership
    // read — no vision query. See FogVis. WorldView (auth / editor, no fog) is
    // always Live; LocalView answers from its visible/snapshot membership.
    virtual FogVis fog_mode(u32 id) const = 0;

    // ── Iteration (the only pools a VIEW consumer walks) ────────────────
    virtual std::span<const u32> renderable_ids() const = 0;
    virtual std::span<const u32> transform_ids()  const = 0;
    virtual std::span<const u32> selectable_ids() const = 0;
    virtual std::span<const u32> item_info_ids()  const = 0;

    // ── Draw-time writes (per-viewer scratch, NOT world truth) ──────────
    // These hit pools LocalView OWNS (its own selectables / anim_queues); on
    // World they hit the real pools (host authoring path unchanged).
    virtual void       size_selectable(u32 id, f32 radius, f32 height) = 0;
    virtual void       clear_anim_queue(u32 id) = 0;
    virtual AnimQueue* anim_queue_mut(u32 id) = 0;
};

struct World;

// Adapter presenting a plain World through the IWorldView read/scratch surface.
// This is what active_world() hands the renderer / picker / HUD on the network
// client (wrapping the client's mirror World) and, for now, on host/offline too
// (wrapping the copy view-world). It holds a bare World* — same codegen as
// direct pool access, zero per-entity cost — and keeps World a plain aggregate
// (no vtable). The host/offline zero-copy projection is LocalView (a different
// IWorldView impl, added later). Anything that runs on a remote client — and
// the host's mirror of that same logic — reads through this; anything that
// needs authoritative truth (simulation, scripting, the server-side send gate)
// takes `World&` directly. Bodies live in world_view.cpp (needs the full World).
struct WorldView final : IWorldView {
    explicit WorldView(World& w) : m_world(&w) {}
    World& world() const { return *m_world; }

    const Transform*              transform(u32 id)      const override;
    const Renderable*             renderable(u32 id)     const override;
    const Health*                 health(u32 id)         const override;
    const Player*                 owner(u32 id)          const override;
    const HandleInfo*             handle_info(u32 id)    const override;
    const Movement*               movement(u32 id)       const override;
    const Combat*                 combat(u32 id)         const override;
    const Construction*           construction(u32 id)   const override;
    const Selectable*             selectable(u32 id)     const override;
    bool                          is_dead(u32 id)        const override;
    const StatusFlags*            status(u32 id)         const override;
    const UnitClassificationComp* classification(u32 id) const override;
    const AbilitySet*             ability_set(u32 id)    const override;
    const StateBlock*             state_block(u32 id)    const override;
    const ProjectileComp*         projectile(u32 id)     const override;
    const ForcedVisibility*       forced_visibility(u32 id) const override;
    const TrueSightVisibility*    true_sight(u32 id)     const override;
    const DestructableComp*       destructable(u32 id)   const override;
    const DoodadComp*             doodad(u32 id)         const override;
    const ItemInfo*               item_info(u32 id)      const override;
    const Carriable*              carriable(u32 id)      const override;
    const Inventory*              inventory(u32 id)      const override;
    const AnimQueue*              anim_queue(u32 id)     const override;

    bool                contains(u32 id) const override;
    const TypeRegistry* type_registry()  const override;

    std::span<const u32> renderable_ids() const override;
    std::span<const u32> transform_ids()  const override;
    std::span<const u32> selectable_ids() const override;
    std::span<const u32> item_info_ids()  const override;

    void       size_selectable(u32 id, f32 radius, f32 height) override;
    void       clear_anim_queue(u32 id) override;
    AnimQueue* anim_queue_mut(u32 id) override;

    FogVis fog_mode(u32 /*id*/) const override { return FogVis::Live; }  // no fog

private:
    World* m_world;
};

// ── LocalView ────────────────────────────────────────────────────────────────
// The fog projection the renderer/picker/HUD read in EVERY mode. Live entities
// resolve straight to `source` (the authoritative World on host/offline, the
// network mirror on the client) — no per-tick copy; only statics that left live
// sight carry a frozen `snapshot`. project_local_view() drives the stores each
// tick. Reads are GATED, never a blind fall-through:
//   snapshotted(id) → frozen snapshot   (source may have deleted the entity)
//   visible(id)     → source's live component
//   otherwise       → nullptr           (fogged entities never leak through)
struct LocalView final : IWorldView {
    LocalView() = default;

    // Rebuilt each tick by project_local_view. Public so the projector populates
    // them without a wide friend surface.
    World*                  source = nullptr;   // auth World (host/offline) or network mirror (client)
    std::unordered_set<u32> visible;            // ids live-visible this tick → read source

    // Frozen last-seen copies for statics out of live sight. Only the pools a
    // snapshotted static is read through on a view path (others resolve to null
    // when snapshotted). owner/health/is_dead/doodad are intentionally absent:
    // every path that reads them first skips non-live entities, and the renderer
    // FREEZES a fog-memory static's animation (so "dead" needs no snapshot — the
    // last-seen frame holds the pose).
    struct Snapshot {
        SparseSet<Transform>              transforms;     // placement, pick, reveal-check pos
        SparseSet<Renderable>             renderables;    // model + visible flag
        SparseSet<HandleInfo>             handle_infos;   // membership anchor + category
        SparseSet<UnitClassificationComp> classifications;// is_static_remembered_entity (dim/cull)
        SparseSet<DestructableComp>       destructables;  // right-click-attack + tree filter
    } snapshot;

    // Ids whose tile the client fog has observed HIDDEN since the snapshot was
    // taken. Arms edge-triggered "remove on reveal": drop only on a real
    // HIDDEN→VISIBLE re-scout, never in the post-S_HIDE window where the client's
    // interpolated (one-tick-behind) fog still reads the tile live.
    std::unordered_set<u32> snapshot_hidden_seen;

    // Per-viewer render/pick scratch (NOT source): the renderer sizes selectables
    // and advances anim queues here so it never mutates the authoritative world.
    SparseSet<Selectable> own_selectables;
    SparseSet<AnimQueue>  own_anim_queues;

    // Iteration id-lists, rebuilt each tick (visible ∪ snapshotted, per pool).
    std::vector<u32> iter_renderables;
    std::vector<u32> iter_transforms;
    std::vector<u32> iter_item_infos;

    bool snapshotted(u32 id) const { return snapshot.handle_infos.has(id); }

    // The ONE place the snapshot pool list lives; both fills (offline projection +
    // client S_HIDE handler) call it. Idempotent. `src` = auth world or mirror.
    void snapshot_from(const World& src, u32 id);
    void drop_snapshot(u32 id);   // remove the frozen copy (on reveal / destroy)

    // Wipe every owned store — the snapshots, the visible set, the render/pick
    // scratch, and the per-tick iteration lists. Called on scene switch /
    // end_session (in lockstep with the auth world's clear_entities). Leaves
    // `auth` pointing where it was; project_local_view re-wires it.
    void clear();

    const Transform*              transform(u32 id)      const override;
    const Renderable*             renderable(u32 id)     const override;
    const Health*                 health(u32 id)         const override;
    const Player*                 owner(u32 id)          const override;
    const HandleInfo*             handle_info(u32 id)    const override;
    const Movement*               movement(u32 id)       const override;
    const Combat*                 combat(u32 id)         const override;
    const Construction*           construction(u32 id)   const override;
    const Selectable*             selectable(u32 id)     const override;
    bool                          is_dead(u32 id)        const override;
    const StatusFlags*            status(u32 id)         const override;
    const UnitClassificationComp* classification(u32 id) const override;
    const AbilitySet*             ability_set(u32 id)    const override;
    const StateBlock*             state_block(u32 id)    const override;
    const ProjectileComp*         projectile(u32 id)     const override;
    const ForcedVisibility*       forced_visibility(u32 id) const override;
    const TrueSightVisibility*    true_sight(u32 id)     const override;
    const DestructableComp*       destructable(u32 id)   const override;
    const DoodadComp*             doodad(u32 id)         const override;
    const ItemInfo*               item_info(u32 id)      const override;
    const Carriable*              carriable(u32 id)      const override;
    const Inventory*              inventory(u32 id)      const override;
    const AnimQueue*              anim_queue(u32 id)     const override;

    bool                contains(u32 id) const override;
    const TypeRegistry* type_registry()  const override;

    std::span<const u32> renderable_ids() const override;
    std::span<const u32> transform_ids()  const override;
    std::span<const u32> selectable_ids() const override;
    std::span<const u32> item_info_ids()  const override;

    void       size_selectable(u32 id, f32 radius, f32 height) override;
    void       clear_anim_queue(u32 id) override;
    AnimQueue* anim_queue_mut(u32 id) override;

    // snapshotted → Memory; live-visible → Live; else Hidden. Pure membership.
    FogVis fog_mode(u32 id) const override {
        if (snapshotted(id)) return FogVis::Memory;
        if (visible.count(id)) return FogVis::Live;
        return FogVis::Hidden;
    }
};

// ── View-side (IWorldView) overloads ─────────────────────────────────────────
// The view twins of the cross-layer helpers whose auth (`const World&`) versions
// live in world.h / simulation.h. Declared here beside the interface they read
// through; bodies are in world_view.cpp. Consumers that hold an IWorldView (the
// renderer / picker / HUD) resolve to these; auth callers holding a World& bind
// to the twins in world.h. (World is not an IWorldView, so a `World&` never
// picks these — the split is by type, not preference.)
bool is_static_remembered_entity(const IWorldView& world, u32 entity_id);
f32  unit_fly_height(const IWorldView& world, u32 id);
bool ability_can_afford(const IWorldView& world, u32 unit_id,
                        const std::map<std::string, f32>& cost,
                        std::string* out_lacking = nullptr);
bool can_attack_target(const IWorldView& world, u8 target_mask, Widget target);

} // namespace uldum::simulation
