#pragma once

namespace uldum::editor {

class Editor {
public:
    bool init();
    void shutdown();
    void update();
    void render();

    bool is_active() const { return m_active; }
    void set_active(bool active);

    // Future API (terrain editor v1):
    // void sculpt_raise(float x, float y, float radius, float strength);
    // void sculpt_lower(float x, float y, float radius, float strength);
    // void sculpt_smooth(float x, float y, float radius);
    // void sculpt_flatten(float x, float y, float radius, float height);
    // void paint_texture(float x, float y, float radius, TextureId texture);
    // void set_pathing(int tile_x, int tile_y, PathingFlags flags);
    // void place_object(ObjectTypeId type, float x, float y, float rotation);
    // void save_map(std::string_view path);

private:
    bool m_active = false;
};

} // namespace uldum::editor
