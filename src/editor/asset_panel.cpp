#include "editor/asset_panel.h"

#include "asset/asset.h"
#include "asset/model.h"
#include "asset/texture.h"
#include "audio/audio.h"
#include "render/gpu_texture.h"
#include "rhi/rhi.h"
#include "core/log.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>

namespace uldum::editor {

namespace fs = std::filesystem;

namespace {
constexpr const char* TAG = "AssetPanel";

// Clip names the animation state machine matches (docs/model-format.md).
bool is_engine_clip(std::string_view name) {
    static constexpr std::array<std::string_view, 6> kClips{
        "idle", "walk", "attack", "spell", "hit", "death"};
    return std::find(kClips.begin(), kClips.end(), name) != kClips.end();
}

AssetKind kind_of(std::string_view rel) {
    auto ends = [&](std::string_view s) { return rel.size() >= s.size() && rel.substr(rel.size() - s.size()) == s; };
    if (ends(".glb") || ends(".gltf")) return AssetKind::Model;
    if (ends(".ktx2"))                 return AssetKind::Texture;
    if (ends(".ogg"))                  return AssetKind::Sound;
    return AssetKind::Other;
}

bool is_asset_kind(AssetKind k) { return k != AssetKind::Other; }

// Read-only warnings the engine's loader would raise for this model. Facts
// only — no advice, no proposed fixes.
std::vector<Finding> model_warnings(const asset::ModelData& m) {
    std::vector<Finding> out;

    if (m.meshes.empty() && m.skinned_meshes.empty())
        out.push_back({Severity::Warning, "No triangle meshes — nothing will render."});

    u32 bones = static_cast<u32>(m.skeleton.bones.size());
    if (bones > 128)
        out.push_back({Severity::Warning, std::format("{} bones exceeds the 128-bone limit.", bones)});

    if (!m.animations.empty() && !m.has_skeleton())
        out.push_back({Severity::Warning, "Animations on an unskinned model are dropped."});

    for (const auto& a : m.animations)
        if (!is_engine_clip(a.name))
            out.push_back({Severity::Warning, std::format("Clip '{}' matches no engine state.", a.name)});

    for (const auto& mat : m.materials)
        if (mat.alpha_mode == asset::AlphaMode::Blend) {
            out.push_back({Severity::Warning, "A material uses alphaMode BLEND — rendered as OPAQUE."});
            break;
        }

    return out;
}

ImVec4 severity_color(Severity s) {
    return s == Severity::Warning ? ImVec4{0.95f, 0.80f, 0.35f, 1.0f}
                                  : ImVec4{0.70f, 0.80f, 0.95f, 1.0f};
}
} // namespace

// ── Tree build ───────────────────────────────────────────────────────────────

void AssetPanel::build_tree(const AssetPanelContext& ctx) {
    m_root = TreeNode{};
    m_root.is_dir = true;
    if (!ctx.assets || ctx.map_root.empty()) return;

    // Source-folder maps: walk the real tree (preserves case, shows every
    // file). Packed maps: fall back to the mounted virtual listing.
    std::vector<std::string> rels;
    if (ctx.editable && fs::is_directory(ctx.map_root)) {
        std::error_code ec;
        fs::path root(ctx.map_root);
        for (auto it = fs::recursive_directory_iterator(root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            rels.push_back(fs::relative(it->path(), root, ec).generic_string());
        }
    } else {
        std::string prefix = ctx.map_root;
        if (!prefix.empty() && prefix.back() != '/') prefix += '/';
        for (auto& full : ctx.assets->list_files(ctx.map_root)) {
            std::string_view rel = full;
            if (rel.size() >= prefix.size() && rel.substr(0, prefix.size()) == prefix)
                rel.remove_prefix(prefix.size());
            rels.emplace_back(rel);
        }
    }

    for (auto& rel : rels) insert_path(m_root, rel);
    log::info(TAG, "Indexed {} files in {}", rels.size(), ctx.map_root);
}

// Insert a map-relative file path into the tree, creating folder nodes as
// needed. Children are kept sorted (folders first, then files, each A→Z).
void AssetPanel::insert_path(TreeNode& root, const std::string& rel) {
    TreeNode* cur = &root;
    std::string_view path = rel;
    std::string accum;
    while (!path.empty()) {
        auto slash = path.find('/');
        std::string_view seg = (slash == std::string_view::npos) ? path : path.substr(0, slash);
        bool is_file = (slash == std::string_view::npos);
        if (!accum.empty()) accum += '/';
        accum.append(seg);

        auto it = std::find_if(cur->children.begin(), cur->children.end(),
            [&](const TreeNode& n) { return n.name == seg; });
        if (it == cur->children.end()) {
            TreeNode node;
            node.name   = std::string(seg);
            node.rel    = accum;
            node.is_dir = !is_file;
            node.kind   = is_file ? kind_of(seg) : AssetKind::Other;
            // Insert sorted: folders before files, then lexicographic.
            auto pos = std::find_if(cur->children.begin(), cur->children.end(),
                [&](const TreeNode& n) {
                    if (n.is_dir != node.is_dir) return node.is_dir;   // dirs sort ahead of files
                    return node.name < n.name;
                });
            it = cur->children.insert(pos, std::move(node));
        }
        cur = &*it;
        if (is_file) break;
        path.remove_prefix(slash + 1);
    }
}

// ── Draw ─────────────────────────────────────────────────────────────────────

void AssetPanel::draw(const AssetPanelContext& ctx, bool* p_open) {
    ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Map Explorer", p_open)) { ImGui::End(); return; }

    if (!ctx.map_loaded) {
        ImGui::TextDisabled("Open a map to browse its assets.");
        ImGui::End();
        return;
    }

    if (!ctx.editable)
        ImGui::TextDisabled("Packed map — read-only (open a source folder to manage files).");

    // Tree on the left, inspector on the right.
    ImGui::BeginChild("asset_tree", ImVec2(ImGui::GetContentRegionAvail().x * 0.42f, 0), true);
    draw_tree(ctx);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("asset_inspector", ImVec2(0, 0), true);
    draw_inspector(ctx);
    ImGui::EndChild();

    draw_manage_popups(ctx);
    ImGui::End();
}

void AssetPanel::draw_tree(const AssetPanelContext& ctx) {
    for (auto& child : m_root.children) draw_node(ctx, child);
}

void AssetPanel::draw_node(const AssetPanelContext& ctx, TreeNode& node) {
    if (node.is_dir) {
        bool open = ImGui::TreeNodeEx(node.name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
        // Right-click a folder → import an asset into it (source maps only).
        if (ctx.editable && ImGui::BeginPopupContextItem(node.rel.c_str())) {
            if (ImGui::MenuItem("Import asset here...")) begin_import(node.rel);
            ImGui::EndPopup();
        }
        if (open) {
            for (auto& c : node.children) draw_node(ctx, c);
            ImGui::TreePop();
        }
        return;
    }

    // File leaf.
    bool selected = (node.rel == m_selected_rel);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;
    bool manageable = ctx.editable && is_asset_kind(node.kind);
    if (!is_asset_kind(node.kind))
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));  // non-assets dimmed
    ImGui::TreeNodeEx(node.name.c_str(), flags);
    if (!is_asset_kind(node.kind)) ImGui::PopStyleColor();

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        select(ctx, node.rel, node.kind);

    // Right-click an asset file → rename / delete (never non-assets, so a
    // maker can't delete a Lua/JSON/terrain file from here).
    if (manageable && ImGui::BeginPopupContextItem(node.rel.c_str())) {
        if (ImGui::MenuItem("Rename...")) begin_rename(ctx, node.rel);
        if (ImGui::MenuItem("Delete"))    begin_delete(ctx, node.rel);
        ImGui::EndPopup();
    }
}

void AssetPanel::draw_inspector(const AssetPanelContext& ctx) {
    if (m_selected_rel.empty()) {
        ImGui::TextDisabled("Select a file to inspect.");
        return;
    }
    ImGui::TextWrapped("%s", m_selected_rel.c_str());
    ImGui::Separator();

    switch (m_selected_kind) {
        case AssetKind::Model: {
            ImGui::TextUnformatted(m_model_summary.c_str());
            if (!m_clip_names.empty()) {
                ImGui::TextDisabled("Clips:");
                for (const auto& c : m_clip_names) { ImGui::SameLine(); ImGui::TextUnformatted(c.c_str()); }
            }
            if (!m_findings.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Loader warnings:");
                for (const auto& f : m_findings)
                    ImGui::TextColored(severity_color(f.severity), "%s", f.message.c_str());
            }
            break;
        }
        case AssetKind::Texture: {
            if (m_tex_id) {
                f32 avail = ImGui::GetContentRegionAvail().x;
                f32 w = static_cast<f32>(m_tex_w), h = static_cast<f32>(m_tex_h);
                if (w > avail && w > 0) { h *= avail / w; w = avail; }
                ImGui::Image(reinterpret_cast<ImTextureID>(m_tex_id), ImVec2(w, h));
                ImGui::Text("%u x %u", m_tex_w, m_tex_h);
            } else {
                ImGui::TextColored(severity_color(Severity::Warning), "Failed to load texture.");
            }
            break;
        }
        case AssetKind::Sound: {
            if (ImGui::Button("Play") && ctx.audio) ctx.audio->play_sfx_2d(m_selected_rel);
            break;
        }
        default:
            ImGui::TextDisabled("Script / data file — not previewable.");
            break;
    }
}

// ── Selection ────────────────────────────────────────────────────────────────

void AssetPanel::select(const AssetPanelContext& ctx, const std::string& rel, AssetKind kind) {
    if (rel == m_selected_rel) return;
    clear_selection_preview(ctx);

    m_selected_rel  = rel;
    m_selected_kind = kind;

    switch (kind) {
        case AssetKind::Model:   select_model(ctx, rel);   break;
        case AssetKind::Texture: select_texture(ctx, rel); break;
        default: break;   // sounds play on demand; scripts/data show path only
    }
}

void AssetPanel::clear_selection_preview(const AssetPanelContext& ctx) {
    free_texture(ctx);
    m_model_summary.clear();
    m_clip_names.clear();
    m_findings.clear();
}

void AssetPanel::select_model(const AssetPanelContext& ctx, const std::string& rel) {
    if (!ctx.assets) return;

    auto bytes = ctx.assets->read_file_bytes(rel);
    if (bytes.empty()) {
        m_findings.push_back({Severity::Warning, "File not found in the mounted map."});
        return;
    }
    auto md = asset::load_model_from_memory(bytes.data(), static_cast<u32>(bytes.size()), rel);
    if (!md) {
        m_findings.push_back({Severity::Warning, std::format("Load failed: {}", md.error())});
        return;
    }

    u32 verts = 0;
    for (const auto& m : md->meshes)         verts += static_cast<u32>(m.vertices.size());
    for (const auto& m : md->skinned_meshes) verts += static_cast<u32>(m.vertices.size());
    m_model_summary = std::format("{} meshes | {} verts | {} bones | {} anims | {} materials",
                                  md->meshes.size() + md->skinned_meshes.size(), verts,
                                  md->skeleton.bones.size(), md->animations.size(), md->materials.size());
    for (const auto& a : md->animations) m_clip_names.push_back(a.name);
    m_findings = model_warnings(*md);
}

void AssetPanel::select_texture(const AssetPanelContext& ctx, const std::string& rel) {
    if (!ctx.assets || !ctx.rhi) return;
    auto bytes = ctx.assets->read_file_bytes(rel);
    if (bytes.empty()) return;
    auto tex = asset::load_texture_from_memory(bytes.data(), static_cast<u32>(bytes.size()));
    if (!tex) { log::warn(TAG, "texture preview failed: {}", tex.error()); return; }

    m_tex_w = tex->width;
    m_tex_h = tex->height;
    m_tex   = render::upload_texture(*ctx.rhi, *tex);
    VkImageView view = ctx.rhi->resolve_view(m_tex.texture);
    if (view != VK_NULL_HANDLE) {
        VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_tex_id = reinterpret_cast<void*>(ds);
    }
}

// ── File management (asset kinds only, source-folder maps only) ───────────────

int AssetPanel::count_references(const AssetPanelContext& ctx, const std::string& rel) const {
    // A map references assets by their map-relative path inside the type/data
    // JSON and Lua. A plain substring scan over those text files is enough to
    // warn "this is still used" before a rename/delete.
    if (ctx.map_root.empty()) return 0;
    int hits = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fs::path(ctx.map_root), ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto ext = it->path().extension().string();
        if (ext != ".json" && ext != ".lua") continue;
        std::ifstream f(it->path(), std::ios::binary);
        if (!f) continue;
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (body.find(rel) != std::string::npos) ++hits;
    }
    return hits;
}

void AssetPanel::begin_rename(const AssetPanelContext& ctx, const std::string& rel) {
    m_pending     = Pending::Rename;
    m_open_modal  = true;
    m_pending_rel = rel;
    m_manage_msg.clear();
    m_ref_count   = count_references(ctx, rel);
    auto slash = rel.find_last_of('/');
    std::string base = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
    m_name_buf.fill('\0');
    std::memcpy(m_name_buf.data(), base.c_str(), std::min(base.size(), m_name_buf.size() - 1));
}

void AssetPanel::begin_delete(const AssetPanelContext& ctx, const std::string& rel) {
    m_pending     = Pending::Delete;
    m_open_modal  = true;
    m_pending_rel = rel;
    m_manage_msg.clear();
    m_ref_count   = count_references(ctx, rel);
}

void AssetPanel::begin_import(const std::string& folder_rel) {
    m_pending     = Pending::Import;
    m_pending_rel = folder_rel;   // deferred: the native picker runs after the tree
}

void AssetPanel::draw_manage_popups(const AssetPanelContext& ctx) {
    // Import runs the native file dialog outside the tree/popup stack.
    if (m_pending == Pending::Import) { do_import(ctx); m_pending = Pending::None; return; }

    if (m_open_modal) {
        ImGui::OpenPopup(m_pending == Pending::Rename ? "Rename asset" : "Delete asset");
        m_open_modal = false;
    }

    if (ImGui::BeginPopupModal("Rename asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", m_pending_rel.c_str());
        if (m_ref_count > 0)
            ImGui::TextColored(severity_color(Severity::Warning),
                               "Referenced by %d map file(s) — renaming will break those paths.", m_ref_count);
        ImGui::InputText("New name", m_name_buf.data(), m_name_buf.size());
        if (!m_manage_msg.empty())
            ImGui::TextColored(severity_color(Severity::Warning), "%s", m_manage_msg.c_str());
        if (ImGui::Button("Rename")) do_rename(ctx);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { m_pending = Pending::None; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete %s?", m_pending_rel.c_str());
        if (m_ref_count > 0)
            ImGui::TextColored(severity_color(Severity::Warning),
                               "Referenced by %d map file(s) — those paths will dangle.", m_ref_count);
        if (!m_manage_msg.empty())
            ImGui::TextColored(severity_color(Severity::Warning), "%s", m_manage_msg.c_str());
        if (ImGui::Button("Delete")) do_delete(ctx);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { m_pending = Pending::None; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void AssetPanel::do_rename(const AssetPanelContext& ctx) {
    std::string new_name = m_name_buf.data();
    if (new_name.empty() || new_name.find('/') != std::string::npos ||
        new_name.find('\\') != std::string::npos) {
        m_manage_msg = "Enter a file name (no path separators).";
        return;
    }
    if (kind_of(new_name) == AssetKind::Other) {
        m_manage_msg = "Keep an asset extension (.glb/.gltf/.ktx2/.ogg).";
        return;
    }
    auto slash = m_pending_rel.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? std::string{} : m_pending_rel.substr(0, slash + 1);
    std::string new_rel = dir + new_name;

    std::error_code ec;
    fs::path src = fs::path(ctx.map_root) / m_pending_rel;
    fs::path dst = fs::path(ctx.map_root) / new_rel;
    if (fs::exists(dst, ec)) { m_manage_msg = "A file with that name already exists."; return; }
    fs::rename(src, dst, ec);
    if (ec) { m_manage_msg = "Rename failed: " + ec.message(); return; }

    if (m_selected_rel == m_pending_rel) clear_selection_preview(ctx), m_selected_rel.clear();
    build_tree(ctx);
    m_pending = Pending::None;
    ImGui::CloseCurrentPopup();
}

void AssetPanel::do_delete(const AssetPanelContext& ctx) {
    std::error_code ec;
    fs::remove(fs::path(ctx.map_root) / m_pending_rel, ec);
    if (ec) { m_manage_msg = "Delete failed: " + ec.message(); return; }

    if (m_selected_rel == m_pending_rel) clear_selection_preview(ctx), m_selected_rel.clear();
    build_tree(ctx);
    m_pending = Pending::None;
    ImGui::CloseCurrentPopup();
}

void AssetPanel::do_import(const AssetPanelContext& ctx) {
    if (!ctx.pick_import_file) return;
    std::string src = ctx.pick_import_file();
    if (src.empty()) return;   // cancelled

    fs::path srcp(src);
    std::string base = srcp.filename().string();
    if (kind_of(base) == AssetKind::Other) {
        log::warn(TAG, "Import ignored: '{}' is not an asset (.glb/.gltf/.ktx2/.ogg)", base);
        return;
    }
    fs::path dst = fs::path(ctx.map_root) / m_pending_rel / base;
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(srcp, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) { log::error(TAG, "Import failed: {}", ec.message()); return; }
    log::info(TAG, "Imported '{}' into '{}'", base, m_pending_rel);
    build_tree(ctx);
}

// ── Teardown ─────────────────────────────────────────────────────────────────

void AssetPanel::free_texture(const AssetPanelContext& ctx) {
    if (!m_tex_id && m_tex.width == 0) return;
    if (ctx.rhi) ctx.rhi->wait_idle();   // GPU may still reference the descriptor this frame
    if (m_tex_id) {
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(m_tex_id));
        m_tex_id = nullptr;
    }
    if (ctx.rhi && m_tex.width > 0) render::destroy_texture(*ctx.rhi, m_tex);
    m_tex = {};
    m_tex_w = m_tex_h = 0;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void AssetPanel::on_map_loaded(const AssetPanelContext& ctx) {
    clear_transient(ctx);
    build_tree(ctx);
}

void AssetPanel::clear_transient(const AssetPanelContext& ctx) {
    clear_selection_preview(ctx);
    m_selected_rel.clear();
    m_selected_kind = AssetKind::Other;
    m_pending = Pending::None;
    m_open_modal = false;
}

void AssetPanel::release_all(const AssetPanelContext& ctx) {
    clear_transient(ctx);
    m_root = TreeNode{};
}

} // namespace uldum::editor
