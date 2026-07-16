#pragma once

// File explorer panel for uldum_editor.
//
// A dockable "Assets" window (opened from the View menu; not an edit mode —
// modes are for world editing, this is about the map *package*). It is a file
// explorer over the open map: it shows every file in the map's own folder
// structure, but behaves as an asset manager — only asset files (models,
// textures, sounds) can be imported, renamed, or deleted. Scripts, data, and
// terrain files are visible but never mutated, so a maker can't accidentally
// delete a Lua file from here.
//
// Selecting a file previews it (model in the viewport, texture as an image,
// sound on demand) and shows a read-only info panel: the asset's factual
// makeup plus any warnings the engine's loader would raise. The info panel
// only reports — it never prescribes fixes.

#include "core/types.h"
#include "render/gpu_texture.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace uldum::asset      { class AssetManager; }
namespace uldum::audio      { class AudioEngine; }
namespace uldum::rhi        { class Rhi; }
namespace uldum::render     { class Renderer; }

namespace uldum::editor {

// Browsable file kinds, keyed off extension. Model/Texture/Sound are the
// manageable assets; Script (.lua), Json, and Bin are inspectable but not
// managed; everything else is `Other` — shown in the tree, view-only.
enum class FileKind : u8 { Model, Texture, Sound, Script, Json, Bin, Other };

// Everything the panel needs each frame, owned by Editor and passed in so the
// panel never reaches into Editor privates. `pick_import_file` opens the
// Editor's native file dialog and returns the chosen source path (empty if
// cancelled). `check_syntax` / `validate_scene` run Lua validation and return
// display lines (Editor owns the scriptcheck logic + engine-source paths).
struct FileExplorerContext {
    asset::AssetManager* assets    = nullptr;
    audio::AudioEngine*  audio     = nullptr;
    rhi::Rhi*            rhi       = nullptr;
    render::Renderer*    renderer  = nullptr;   // drives the offscreen model viewer
    void*                model_view_tex_id = nullptr;  // ImTextureID for the viewer color target
    std::string          map_root;          // virtual + fs root of the open map
    bool                 map_loaded = false;
    bool                 editable   = false; // source-folder map → files mutable
    std::function<std::string()> pick_import_file;
    std::function<std::vector<std::string>(const std::string& rel)>   check_syntax;
    std::function<std::vector<std::string>(const std::string& scene)> validate_scene;
};

class FileExplorer {
public:
    // Draw the "Assets" window. `p_open` backs the window's close box so the
    // View-menu toggle and the window X stay in sync.
    void draw(const FileExplorerContext& ctx, bool* p_open);

    // Rebuild the file tree for a newly-opened map. Clears any prior preview.
    void on_map_loaded(const FileExplorerContext& ctx);

    // Drop the live preview entity + selection (scene switch / reset). Keeps the
    // built tree if the map is unchanged; Editor calls on_map_loaded to rebuild.
    void clear_transient(const FileExplorerContext& ctx);

    // Full teardown: preview entity + GPU texture + ImGui texture id released.
    void release_all(const FileExplorerContext& ctx);

private:
    // Nested file tree mirroring the map's on-disk layout. `rel` is the
    // map-relative path (used for preview, read_file_bytes, and file ops);
    // it matches how placed entities store their model path.
    struct TreeNode {
        std::string name;                 // path segment (folder or file basename)
        std::string rel;                  // full map-relative path ("" = root)
        bool        is_dir = true;
        FileKind   kind   = FileKind::Other;   // files only
        std::vector<TreeNode> children;
    };

    void build_tree(const FileExplorerContext& ctx);
    static void insert_path(TreeNode& root, const std::string& rel);
    void draw_tree(const FileExplorerContext& ctx);
    void draw_node(const FileExplorerContext& ctx, TreeNode& node);
    void draw_inspector(const FileExplorerContext& ctx);

    void select(const FileExplorerContext& ctx, const std::string& rel, FileKind kind);
    void select_model(const FileExplorerContext& ctx, const std::string& rel);
    void select_texture(const FileExplorerContext& ctx, const std::string& rel);
    void inspect_json(const FileExplorerContext& ctx, const std::string& rel);
    void clear_selection_preview(const FileExplorerContext& ctx);

    // File management (source-folder maps only; asset kinds only).
    void begin_rename(const FileExplorerContext& ctx, const std::string& rel);
    void begin_delete(const FileExplorerContext& ctx, const std::string& rel);
    void begin_import(const std::string& folder_rel);
    void draw_manage_popups(const FileExplorerContext& ctx);
    void do_rename(const FileExplorerContext& ctx);
    void do_delete(const FileExplorerContext& ctx);
    void do_import(const FileExplorerContext& ctx);
    int  count_references(const FileExplorerContext& ctx, const std::string& rel) const;

    void free_texture(const FileExplorerContext& ctx);

    TreeNode m_root;

    // Current selection.
    std::string m_selected_rel;
    FileKind   m_selected_kind = FileKind::Other;

    // Model info (read-only text): factual makeup + the loader's own warnings
    // (from ModelData.warnings). The 3D model preview is deferred to the
    // dedicated viewer phase. `m_model_error` is set if the load hard-fails.
    std::string      m_model_summary;
    std::vector<std::string> m_clip_names;
    std::vector<std::string> m_model_warnings;
    std::string      m_model_error;

    // Script inspection: the scene a selected .lua belongs to ("" = not under a
    // scene), and the last validation output (empty until a button is pressed).
    std::string      m_script_scene;
    std::vector<std::string> m_script_results;

    // JSON inspection (computed on select): recognized kind, a few shallow info
    // lines, and well-formedness. `m_json_error` empty = parsed clean.
    std::string      m_json_label;
    std::vector<std::string> m_json_info;
    std::string      m_json_error;

    // Recognized binary (.bin) label, and selected sound duration in seconds
    // (0 = unknown). Both computed on select.
    std::string      m_bin_label;
    f32              m_sound_secs = 0.0f;

    // Texture preview: uploaded GPU image + its ImGui binding.
    render::GpuTexture m_tex{};
    void*              m_tex_id = nullptr;   // ImTextureID (VkDescriptorSet)
    u32                m_tex_w = 0, m_tex_h = 0;

    // Pending file operation, driven through modal popups.
    enum class Pending : u8 { None, Rename, Delete, Import };
    Pending     m_pending = Pending::None;
    bool        m_open_modal = false;
    std::string m_pending_rel;              // file (rename/delete) or folder (import)
    std::array<char, 128> m_name_buf{};
    std::string m_manage_msg;               // error shown in the modal
    int         m_ref_count = 0;            // references to the pending target
};

} // namespace uldum::editor
