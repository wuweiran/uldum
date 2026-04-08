#pragma once

#include "core/types.h"

#include <glm/vec3.hpp>

#include <string>
#include <string_view>
#include <unordered_map>

// miniaudio types (opaque pointers — defined in audio.cpp)
struct ma_engine;
struct ma_sound;

namespace uldum::audio {

enum class Channel : u8 { Master, SFX, Music, Ambient, Voice, Count };

// Opaque handle for active sounds (ambient loops, etc.)
struct SoundHandle { u32 id = 0; bool is_valid() const { return id != 0; } };

class AudioEngine {
public:
    bool init();
    void shutdown();
    void update();

    // Listener (call each frame with camera position)
    void set_listener(glm::vec3 position, glm::vec3 forward, glm::vec3 up);

    // SFX — short one-shot sounds
    void play_sfx(std::string_view path, glm::vec3 position);
    void play_sfx_2d(std::string_view path);

    // Music — streaming, one track at a time, crossfade
    void play_music(std::string_view path, f32 fade_in = 1.0f);
    void stop_music(f32 fade_out = 1.0f);

    // Ambient — looping, 3D positioned
    SoundHandle play_ambient(std::string_view path, glm::vec3 position);
    void stop_ambient(SoundHandle handle, f32 fade_out = 0.5f);

    // Volume control (0.0 to 1.0)
    void set_volume(Channel channel, f32 volume);

    // Set the map root for resolving sound paths
    void set_map_root(std::string_view root) { m_map_root = root; }

private:
    std::string resolve_path(std::string_view path) const;

    ma_engine* m_engine = nullptr;
    void* m_groups = nullptr;  // ma_sound_group array, cast in .cpp

    // Active music track
    ma_sound* m_music = nullptr;
    ma_sound* m_music_prev = nullptr;  // fading out during crossfade
    f32 m_music_fade_in = 0;
    f32 m_music_fade_out = 0;
    f32 m_music_fade_timer = 0;
    f32 m_music_prev_fade_timer = 0;

    // Active ambient sounds
    struct AmbientSound {
        ma_sound* sound = nullptr;
        f32 fade_out_timer = 0;
        f32 fade_out_duration = 0;
    };
    std::unordered_map<u32, AmbientSound> m_ambients;
    u32 m_next_handle = 1;

    // Fire-and-forget SFX (cleaned up when done)
    struct ActiveSFX {
        ma_sound* sound = nullptr;
    };
    std::vector<ActiveSFX> m_active_sfx;

    std::string m_map_root;
    bool m_initialized = false;
};

} // namespace uldum::audio
