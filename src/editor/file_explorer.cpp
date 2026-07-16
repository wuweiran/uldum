#include "editor/file_explorer.h"

#include "asset/asset.h"
#include "asset/model.h"
#include "asset/texture.h"
#include "audio/audio.h"
#include "render/gpu_texture.h"
#include "render/renderer.h"
#include "rhi/rhi.h"
#include "core/log.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <nlohmann/json.hpp>

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
constexpr const char* TAG = "FileExplorer";

FileKind kind_of(std::string_view rel) {
    auto ends = [&](std::string_view s) { return rel.size() >= s.size() && rel.substr(rel.size() - s.size()) == s; };
    if (ends(".glb") || ends(".gltf")) return FileKind::Model;
    if (ends(".ktx2"))                 return FileKind::Texture;
    if (ends(".ogg"))                  return FileKind::Sound;
    if (ends(".lua"))                  return FileKind::Script;
    if (ends(".json"))                 return FileKind::Json;
    if (ends(".bin"))                  return FileKind::Bin;
    return FileKind::Other;
}

// Managed assets can be imported / renamed / deleted. Scripts and other files
// (JSON, terrain) are visible but never mutated from here.
bool is_managed_kind(FileKind k) {
    return k == FileKind::Model || k == FileKind::Texture || k == FileKind::Sound;
}

// If `rel` lives under scenes/<name>/, return <name>; else empty. A .lua under
// a scene validates against that scene's runtime set; a shared script doesn't.
std::string scene_of(std::string_view rel) {
    constexpr std::string_view kPfx = "scenes/";
    if (rel.substr(0, kPfx.size()) != kPfx) return {};
    rel.remove_prefix(kPfx.size());
    auto slash = rel.find('/');
    return slash == std::string_view::npos ? std::string{} : std::string(rel.substr(0, slash));
}

constexpr ImVec4 kWarnColor{0.95f, 0.80f, 0.35f, 1.0f};   // loader warnings
constexpr ImVec4 kErrorColor{0.95f, 0.45f, 0.40f, 1.0f};  // load failure
} // namespace

// ── Tree build ───────────────────────────────────────────────────────────────

void FileExplorer::build_tree(const FileExplorerContext& ctx) {
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
void FileExplorer::insert_path(TreeNode& root, const std::string& rel) {
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
            node.kind   = is_file ? kind_of(seg) : FileKind::Other;
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

void FileExplorer::draw(const FileExplorerContext& ctx, bool* p_open) {
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

void FileExplorer::draw_tree(const FileExplorerContext& ctx) {
    for (auto& child : m_root.children) draw_node(ctx, child);
}

void FileExplorer::draw_node(const FileExplorerContext& ctx, TreeNode& node) {
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
    bool manageable = ctx.editable && is_managed_kind(node.kind);
    if (!is_managed_kind(node.kind))
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));  // non-assets dimmed
    ImGui::TreeNodeEx(node.name.c_str(), flags);
    if (!is_managed_kind(node.kind)) ImGui::PopStyleColor();

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

void FileExplorer::draw_inspector(const FileExplorerContext& ctx) {
    if (m_selected_rel.empty()) {
        ImGui::TextDisabled("Select a file to inspect.");
        return;
    }
    ImGui::TextWrapped("%s", m_selected_rel.c_str());
    ImGui::Separator();

    switch (m_selected_kind) {
        case FileKind::Model: {
            if (!m_model_error.empty()) {
                ImGui::TextColored(kErrorColor, "%s", m_model_error.c_str());
                break;
            }
            ImGui::TextUnformatted(m_model_summary.c_str());
            if (!m_clip_names.empty()) {
                ImGui::TextDisabled("Clips:");
                for (const auto& c : m_clip_names) { ImGui::SameLine(); ImGui::TextUnformatted(c.c_str()); }
            }
            // 3D orbit preview (offscreen render target). An InvisibleButton over
            // the image captures the mouse so dragging orbits the model instead of
            // moving the window; wheel zooms while hovered.
            if (ctx.model_view_tex_id && ctx.renderer && ctx.renderer->viewer_has_model()) {
                f32 side = std::min(ImGui::GetContentRegionAvail().x, 360.0f);
                ImVec2 origin = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##model_view", ImVec2(side, side));
                ImGui::GetWindowDrawList()->AddImage(
                    reinterpret_cast<ImTextureID>(ctx.model_view_tex_id),
                    origin, ImVec2(origin.x + side, origin.y + side));
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    ImVec2 d = ImGui::GetIO().MouseDelta;
                    ctx.renderer->viewer_orbit(d.x, d.y);
                }
                if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
                    ctx.renderer->viewer_zoom(ImGui::GetIO().MouseWheel);
                ImGui::TextDisabled("drag = orbit,  wheel = zoom");

                // Animation clip switcher (WC3-style: prev / name / next). The
                // name sits in a fixed-width centered slot so the right arrow
                // doesn't shift as clip names change length.
                const auto& clips = ctx.renderer->viewer_clips();
                if (!clips.empty()) {
                    i32 cur = ctx.renderer->viewer_clip();
                    i32 n   = static_cast<i32>(clips.size());
                    if (ImGui::ArrowButton("##clip_prev", ImGuiDir_Left))
                        ctx.renderer->viewer_set_clip(cur <= 0 ? n - 1 : cur - 1);
                    ImGui::SameLine();
                    const char* label = (cur < 0) ? "(bind pose)" : clips[cur].c_str();
                    constexpr f32 kSlot = 140.0f;
                    f32 tw = ImGui::CalcTextSize(label).x;
                    f32 x0 = ImGui::GetCursorPosX();
                    ImGui::SetCursorPosX(x0 + std::max(0.0f, (kSlot - tw) * 0.5f));
                    ImGui::TextUnformatted(label);
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::SetCursorPosX(x0 + kSlot);
                    if (ImGui::ArrowButton("##clip_next", ImGuiDir_Right))
                        ctx.renderer->viewer_set_clip(cur >= n - 1 ? 0 : cur + 1);
                }
            }
            if (!m_model_warnings.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Loader warnings:");
                for (const auto& w : m_model_warnings)
                    ImGui::TextColored(kWarnColor, "%s", w.c_str());
            }
            break;
        }
        case FileKind::Texture: {
            if (m_tex_id) {
                f32 avail = ImGui::GetContentRegionAvail().x;
                f32 w = static_cast<f32>(m_tex_w), h = static_cast<f32>(m_tex_h);
                if (w > avail && w > 0) { h *= avail / w; w = avail; }
                ImGui::Image(reinterpret_cast<ImTextureID>(m_tex_id), ImVec2(w, h));
                ImGui::Text("%u x %u", m_tex_w, m_tex_h);
            } else {
                ImGui::TextColored(kErrorColor, "Failed to load texture.");
            }
            break;
        }
        case FileKind::Sound: {
            if (ImGui::Button("Play") && ctx.audio) ctx.audio->play_sfx_2d(m_selected_rel);
            if (m_sound_secs > 0.0f) {
                ImGui::SameLine();
                if (m_sound_secs < 60.0f) ImGui::Text("%.1fs", m_sound_secs);
                else ImGui::Text("%d:%02d", static_cast<int>(m_sound_secs) / 60,
                                  static_cast<int>(m_sound_secs) % 60);
            }
            break;
        }
        case FileKind::Script: {
            // A scene script validates the whole scene (syntax + undefined engine
            // calls). Any other .lua gets a file-granularity syntax check.
            if (!m_script_scene.empty()) {
                if (ImGui::Button("Validate") && ctx.validate_scene)
                    m_script_results = ctx.validate_scene(m_script_scene);
                ImGui::SameLine();
                ImGui::TextDisabled("scene '%s'", m_script_scene.c_str());
            } else {
                if (ImGui::Button("Check syntax") && ctx.check_syntax)
                    m_script_results = ctx.check_syntax(m_selected_rel);
            }
            for (const auto& line : m_script_results) {
                if (line.starts_with("["))   // "[syntax]"/"[global]" = a finding
                    ImGui::TextColored(kErrorColor, "%s", line.c_str());
                else
                    ImGui::TextUnformatted(line.c_str());   // clean / note line
            }
            break;
        }
        case FileKind::Json: {
            ImGui::TextDisabled("%s", m_json_label.c_str());
            for (const auto& line : m_json_info) ImGui::TextUnformatted(line.c_str());
            ImGui::Separator();
            if (m_json_error.empty()) ImGui::TextUnformatted("Valid JSON");
            else                      ImGui::TextColored(kErrorColor, "%s", m_json_error.c_str());
            break;
        }
        case FileKind::Bin: {
            ImGui::TextDisabled("%s", m_bin_label.c_str());
            break;
        }
        default:
            ImGui::TextDisabled("Data file — not previewable.");
            break;
    }
}

// ── Selection ────────────────────────────────────────────────────────────────

void FileExplorer::select(const FileExplorerContext& ctx, const std::string& rel, FileKind kind) {
    if (rel == m_selected_rel) return;
    clear_selection_preview(ctx);

    m_selected_rel  = rel;
    m_selected_kind = kind;

    switch (kind) {
        case FileKind::Model:   select_model(ctx, rel);   break;
        case FileKind::Texture: select_texture(ctx, rel); break;
        case FileKind::Script:  m_script_scene = scene_of(rel); break;
        case FileKind::Json:    inspect_json(ctx, rel);   break;
        case FileKind::Sound:
            if (ctx.audio) m_sound_secs = ctx.audio->sound_length_seconds(rel);
            break;
        case FileKind::Bin: {
            // Recognized by name; not parsed (opaque binary).
            std::string_view base = rel;
            base.remove_prefix(base.find_last_of('/') + 1);
            if      (base == "terrain.bin")    m_bin_label = "Terrain data (heightmap, cliffs, tiles, pathing)";
            else if (base == "placements.bin") m_bin_label = "Placements (preplaced objects, regions, cameras)";
            else                               m_bin_label = "Binary data file";
            break;
        }
        default: break;   // Other kinds show path only
    }
}

void FileExplorer::clear_selection_preview(const FileExplorerContext& ctx) {
    free_texture(ctx);
    m_model_summary.clear();
    m_clip_names.clear();
    m_model_warnings.clear();
    m_model_error.clear();
    m_script_scene.clear();
    m_script_results.clear();
    m_json_label.clear();
    m_json_info.clear();
    m_json_error.clear();
    m_bin_label.clear();
    m_sound_secs = 0.0f;
    if (ctx.renderer) ctx.renderer->viewer_set_model("");   // stop rendering the offscreen model
}

void FileExplorer::select_model(const FileExplorerContext& ctx, const std::string& rel) {
    if (!ctx.assets) return;

    auto bytes = ctx.assets->read_file_bytes(rel);
    if (bytes.empty()) {
        m_model_error = "File not found in the mounted map.";
        return;
    }
    auto md = asset::load_model_from_memory(bytes.data(), static_cast<u32>(bytes.size()), rel,
                                            &m_model_warnings);
    if (!md) {
        m_model_error = std::format("Load failed: {}", md.error());
        return;
    }

    u32 verts = 0;
    for (const auto& m : md->meshes)         verts += static_cast<u32>(m.vertices.size());
    for (const auto& m : md->skinned_meshes) verts += static_cast<u32>(m.vertices.size());
    m_model_summary = std::format("{} meshes | {} verts | {} bones | {} anims | {} materials",
                                  md->meshes.size() + md->skinned_meshes.size(), verts,
                                  md->skeleton.bones.size(), md->animations.size(), md->materials.size());
    for (const auto& a : md->animations) m_clip_names.push_back(a.name);

    // Hand the model to the renderer's offscreen viewer (loads via its own cache,
    // renders isolated). Only meaningful for a source-folder map the renderer's
    // asset mounts can resolve; harmless otherwise (viewer no-ops on load fail).
    if (ctx.renderer) ctx.renderer->viewer_set_model(rel);
}

void FileExplorer::select_texture(const FileExplorerContext& ctx, const std::string& rel) {
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

// Recognize a map JSON by its path and pull a few shallow fields — a friendly
// "what is this" + a line of contents, plus whether it's well-formed. Pure
// nlohmann; touches no loader. Every field read is type-guarded (untrusted map
// data), so a wrong-shaped file degrades to the generic key/element count.
void FileExplorer::inspect_json(const FileExplorerContext& ctx, const std::string& rel) {
    if (!ctx.assets) return;
    auto bytes = ctx.assets->read_file_bytes(rel);
    std::string_view sv(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    auto is = [&](std::string_view p) { return rel == p; };
    auto under = [&](std::string_view dir, std::string_view file) {
        return rel.size() > dir.size() && rel.starts_with(dir) &&
               rel.substr(rel.find_last_of('/') + 1) == file;
    };
    // (label, is-a-registry-of-entries) — registries preview as a keyed count.
    std::string label; bool registry = false;
    if      (is("manifest.json"))                    label = "Map manifest";
    else if (is("tileset.json"))                     label = "Tileset";
    else if (is("hud.json"))                         label = "HUD layout";
    else if (under("types/", "units.json"))          { label = "Unit definitions";         registry = true; }
    else if (under("types/", "abilities.json"))      { label = "Ability definitions";      registry = true; }
    else if (under("types/", "items.json"))          { label = "Item definitions";         registry = true; }
    else if (under("types/", "destructables.json"))  { label = "Destructable definitions"; registry = true; }
    else if (under("types/", "doodads.json"))        { label = "Doodad definitions";       registry = true; }
    else if (under("types/", "effects.json"))        { label = "Effect definitions";       registry = true; }
    else if (rel.find("scenes/") == 0 && rel.ends_with("scene.json")) label = "Scene config";
    else if (rel.find("strings/") == 0)              { label = "Localized strings";        registry = true; }
    else                                             label = "JSON file";
    m_json_label = label;

    auto doc = nlohmann::json::parse(sv, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        m_json_error = "Invalid JSON (parse failed).";
        return;
    }

    auto str_field = [&](const char* k) -> std::string {
        auto it = doc.find(k);
        return (it != doc.end() && it->is_string()) ? it->get<std::string>() : std::string{};
    };

    if (registry && doc.is_object()) {
        m_json_info.push_back(std::format("{} entries", doc.size()));
    } else if (label == "Map manifest") {
        if (auto s = str_field("name");   !s.empty()) m_json_info.push_back("name: " + s);
        if (auto s = str_field("author"); !s.empty()) m_json_info.push_back("author: " + s);
        if (auto s = str_field("game_mode"); !s.empty()) m_json_info.push_back("mode: " + s);
        if (auto s = str_field("suggested_players"); !s.empty()) m_json_info.push_back("players: " + s);
    } else if (label == "Tileset") {
        auto it = doc.find("layers");
        if (it != doc.end() && it->is_array()) m_json_info.push_back(std::format("{} layers", it->size()));
    } else if (label == "HUD layout") {
        if (auto s = str_field("preset"); !s.empty()) m_json_info.push_back("preset: " + s);
        auto it = doc.find("composites");
        if (it != doc.end() && it->is_object()) m_json_info.push_back(std::format("{} composites", it->size()));
    } else {
        // Unknown shape — still say something useful.
        if (doc.is_object())      m_json_info.push_back(std::format("{} top-level keys", doc.size()));
        else if (doc.is_array())  m_json_info.push_back(std::format("{} elements", doc.size()));
    }
}

// ── File management (asset kinds only, source-folder maps only) ───────────────

int FileExplorer::count_references(const FileExplorerContext& ctx, const std::string& rel) const {
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

void FileExplorer::begin_rename(const FileExplorerContext& ctx, const std::string& rel) {
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

void FileExplorer::begin_delete(const FileExplorerContext& ctx, const std::string& rel) {
    m_pending     = Pending::Delete;
    m_open_modal  = true;
    m_pending_rel = rel;
    m_manage_msg.clear();
    m_ref_count   = count_references(ctx, rel);
}

void FileExplorer::begin_import(const std::string& folder_rel) {
    m_pending     = Pending::Import;
    m_pending_rel = folder_rel;   // deferred: the native picker runs after the tree
}

void FileExplorer::draw_manage_popups(const FileExplorerContext& ctx) {
    // Import runs the native file dialog outside the tree/popup stack.
    if (m_pending == Pending::Import) { do_import(ctx); m_pending = Pending::None; return; }

    if (m_open_modal) {
        ImGui::OpenPopup(m_pending == Pending::Rename ? "Rename asset" : "Delete asset");
        m_open_modal = false;
    }

    if (ImGui::BeginPopupModal("Rename asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", m_pending_rel.c_str());
        if (m_ref_count > 0)
            ImGui::TextColored(kWarnColor,
                               "Referenced by %d map file(s) — renaming will break those paths.", m_ref_count);
        ImGui::InputText("New name", m_name_buf.data(), m_name_buf.size());
        if (!m_manage_msg.empty())
            ImGui::TextColored(kWarnColor, "%s", m_manage_msg.c_str());
        if (ImGui::Button("Rename")) do_rename(ctx);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { m_pending = Pending::None; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete %s?", m_pending_rel.c_str());
        if (m_ref_count > 0)
            ImGui::TextColored(kWarnColor,
                               "Referenced by %d map file(s) — those paths will dangle.", m_ref_count);
        if (!m_manage_msg.empty())
            ImGui::TextColored(kWarnColor, "%s", m_manage_msg.c_str());
        if (ImGui::Button("Delete")) do_delete(ctx);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { m_pending = Pending::None; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void FileExplorer::do_rename(const FileExplorerContext& ctx) {
    std::string new_name = m_name_buf.data();
    if (new_name.empty() || new_name.find('/') != std::string::npos ||
        new_name.find('\\') != std::string::npos) {
        m_manage_msg = "Enter a file name (no path separators).";
        return;
    }
    if (kind_of(new_name) == FileKind::Other) {
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

void FileExplorer::do_delete(const FileExplorerContext& ctx) {
    std::error_code ec;
    fs::remove(fs::path(ctx.map_root) / m_pending_rel, ec);
    if (ec) { m_manage_msg = "Delete failed: " + ec.message(); return; }

    if (m_selected_rel == m_pending_rel) clear_selection_preview(ctx), m_selected_rel.clear();
    build_tree(ctx);
    m_pending = Pending::None;
    ImGui::CloseCurrentPopup();
}

void FileExplorer::do_import(const FileExplorerContext& ctx) {
    if (!ctx.pick_import_file) return;
    std::string src = ctx.pick_import_file();
    if (src.empty()) return;   // cancelled

    fs::path srcp(src);
    std::string base = srcp.filename().string();
    if (kind_of(base) == FileKind::Other) {
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

void FileExplorer::free_texture(const FileExplorerContext& ctx) {
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

void FileExplorer::on_map_loaded(const FileExplorerContext& ctx) {
    clear_transient(ctx);
    build_tree(ctx);
}

void FileExplorer::clear_transient(const FileExplorerContext& ctx) {
    clear_selection_preview(ctx);
    m_selected_rel.clear();
    m_selected_kind = FileKind::Other;
    m_pending = Pending::None;
    m_open_modal = false;
}

void FileExplorer::release_all(const FileExplorerContext& ctx) {
    clear_transient(ctx);
    m_root = TreeNode{};
}

} // namespace uldum::editor
