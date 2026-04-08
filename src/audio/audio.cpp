#include "audio/audio.h"
#include "core/log.h"

#include <miniaudio.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace uldum::audio {

static constexpr const char* TAG = "Audio";
static constexpr u32 GROUP_COUNT = static_cast<u32>(Channel::Count);

#define GROUPS() (static_cast<ma_sound_group*>(m_groups))

bool AudioEngine::init() {
    m_engine = new ma_engine;

    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;

    if (ma_engine_init(&config, m_engine) != MA_SUCCESS) {
        log::error(TAG, "Failed to initialize miniaudio engine");
        delete m_engine;
        m_engine = nullptr;
        return false;
    }

    // Create sound groups for each channel
    auto* groups = new ma_sound_group[GROUP_COUNT];
    m_groups = groups;
    const char* group_names[] = {"Master", "SFX", "Music", "Ambient", "Voice"};
    for (u32 i = 0; i < GROUP_COUNT; ++i) {
        // All groups are children of the master group (index 0) except master itself
        ma_sound_group* parent = (i == 0) ? nullptr : &groups[0];
        if (ma_sound_group_init(m_engine, 0, parent, &groups[i]) != MA_SUCCESS) {
            log::error(TAG, "Failed to create sound group '{}'", group_names[i]);
        }
    }

    m_initialized = true;
    log::info(TAG, "AudioEngine initialized — miniaudio {}",
              ma_version_string());
    return true;
}

void AudioEngine::shutdown() {
    if (!m_initialized) return;

    // Clean up active SFX
    for (auto& sfx : m_active_sfx) {
        if (sfx.sound) {
            ma_sound_uninit(sfx.sound);
            delete sfx.sound;
        }
    }
    m_active_sfx.clear();

    // Clean up ambient sounds
    for (auto& [id, amb] : m_ambients) {
        if (amb.sound) {
            ma_sound_uninit(amb.sound);
            delete amb.sound;
        }
    }
    m_ambients.clear();

    // Clean up music
    if (m_music) {
        ma_sound_uninit(m_music);
        delete m_music;
        m_music = nullptr;
    }
    if (m_music_prev) {
        ma_sound_uninit(m_music_prev);
        delete m_music_prev;
        m_music_prev = nullptr;
    }

    // Clean up groups
    if (m_groups) {
        auto* groups = GROUPS();
        for (u32 i = 0; i < GROUP_COUNT; ++i) {
            ma_sound_group_uninit(&groups[i]);
        }
        delete[] groups;
        m_groups = nullptr;
    }

    if (m_engine) {
        ma_engine_uninit(m_engine);
        delete m_engine;
        m_engine = nullptr;
    }

    m_initialized = false;
    log::info(TAG, "AudioEngine shut down");
}

void AudioEngine::update() {
    if (!m_initialized) return;

    // Update music fade-in
    if (m_music && m_music_fade_in > 0) {
        m_music_fade_timer += 1.0f / 60.0f;  // approximate frame dt
        f32 t = std::min(m_music_fade_timer / m_music_fade_in, 1.0f);
        ma_sound_set_volume(m_music, t);
        if (t >= 1.0f) m_music_fade_in = 0;
    }

    // Update music fade-out (previous track during crossfade)
    if (m_music_prev && m_music_prev_fade_timer > 0) {
        m_music_prev_fade_timer -= 1.0f / 60.0f;
        if (m_music_prev_fade_timer <= 0) {
            ma_sound_uninit(m_music_prev);
            delete m_music_prev;
            m_music_prev = nullptr;
        } else {
            f32 t = m_music_prev_fade_timer / m_music_fade_out;
            ma_sound_set_volume(m_music_prev, std::max(0.0f, t));
        }
    }

    // Update ambient fade-outs
    std::vector<u32> finished;
    for (auto& [id, amb] : m_ambients) {
        if (amb.fade_out_duration > 0) {
            amb.fade_out_timer -= 1.0f / 60.0f;
            if (amb.fade_out_timer <= 0) {
                ma_sound_uninit(amb.sound);
                delete amb.sound;
                amb.sound = nullptr;
                finished.push_back(id);
            } else {
                f32 t = amb.fade_out_timer / amb.fade_out_duration;
                ma_sound_set_volume(amb.sound, std::max(0.0f, t));
            }
        }
    }
    for (u32 id : finished) m_ambients.erase(id);

    // Clean up finished SFX
    m_active_sfx.erase(
        std::remove_if(m_active_sfx.begin(), m_active_sfx.end(), [](ActiveSFX& sfx) {
            if (sfx.sound && ma_sound_at_end(sfx.sound)) {
                ma_sound_uninit(sfx.sound);
                delete sfx.sound;
                sfx.sound = nullptr;
                return true;
            }
            return false;
        }),
        m_active_sfx.end());
}

// ── Listener ──────────────────────────────────────────────────────────────

void AudioEngine::set_listener(glm::vec3 position, glm::vec3 forward, glm::vec3 up) {
    if (!m_initialized) return;
    // miniaudio uses right-handed Y-up internally, but we pass raw game coords
    // and let it work in our Z-up space (listener direction is what matters)
    ma_engine_listener_set_position(m_engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(m_engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(m_engine, 0, up.x, up.y, up.z);
}

// ── SFX ───────────────────────────────────────────────────────────────────

void AudioEngine::play_sfx(std::string_view path, glm::vec3 position) {
    if (!m_initialized) return;
    std::string resolved = resolve_path(path);
    if (resolved.empty()) return;

    auto* sound = new ma_sound;
    u32 flags = MA_SOUND_FLAG_DECODE;  // decode fully for short SFX
    if (ma_sound_init_from_file(m_engine, resolved.c_str(), flags,
                                &GROUPS()[static_cast<u32>(Channel::SFX)],
                                nullptr, sound) != MA_SUCCESS) {
        log::warn(TAG, "Failed to play SFX '{}'", path);
        delete sound;
        return;
    }

    ma_sound_set_spatialization_enabled(sound, MA_TRUE);
    ma_sound_set_position(sound, position.x, position.y, position.z);
    ma_sound_set_min_distance(sound, 500.0f);
    ma_sound_set_max_distance(sound, 5000.0f);
    ma_sound_set_volume(sound, 0.4f);
    ma_sound_start(sound);
    m_active_sfx.push_back({sound});
}

void AudioEngine::play_sfx_2d(std::string_view path) {
    if (!m_initialized) return;
    std::string resolved = resolve_path(path);
    if (resolved.empty()) return;

    auto* sound = new ma_sound;
    u32 flags = MA_SOUND_FLAG_DECODE;
    if (ma_sound_init_from_file(m_engine, resolved.c_str(), flags,
                                &GROUPS()[static_cast<u32>(Channel::SFX)],
                                nullptr, sound) != MA_SUCCESS) {
        log::warn(TAG, "Failed to play 2D SFX '{}'", path);
        delete sound;
        return;
    }

    ma_sound_set_spatialization_enabled(sound, MA_FALSE);
    ma_sound_start(sound);
    m_active_sfx.push_back({sound});
}

// ── Music ─────────────────────────────────────────────────────────────────

void AudioEngine::play_music(std::string_view path, f32 fade_in) {
    if (!m_initialized) return;
    std::string resolved = resolve_path(path);
    if (resolved.empty()) return;

    // Crossfade: move current to prev
    if (m_music_prev) {
        ma_sound_uninit(m_music_prev);
        delete m_music_prev;
    }
    m_music_prev = m_music;
    if (m_music_prev) {
        m_music_fade_out = fade_in;
        m_music_prev_fade_timer = fade_in;
    }

    // Start new track
    m_music = new ma_sound;
    u32 flags = MA_SOUND_FLAG_STREAM;  // stream music, don't decode fully
    if (ma_sound_init_from_file(m_engine, resolved.c_str(), flags,
                                &GROUPS()[static_cast<u32>(Channel::Music)],
                                nullptr, m_music) != MA_SUCCESS) {
        log::warn(TAG, "Failed to play music '{}'", path);
        delete m_music;
        m_music = nullptr;
        return;
    }

    ma_sound_set_spatialization_enabled(m_music, MA_FALSE);
    ma_sound_set_looping(m_music, MA_TRUE);
    ma_sound_set_volume(m_music, (fade_in > 0) ? 0.0f : 1.0f);
    ma_sound_start(m_music);

    m_music_fade_in = fade_in;
    m_music_fade_timer = 0;

    log::info(TAG, "Playing music '{}' (fade {}s)", path, fade_in);
}

void AudioEngine::stop_music(f32 fade_out) {
    if (!m_initialized || !m_music) return;

    if (fade_out <= 0) {
        ma_sound_uninit(m_music);
        delete m_music;
        m_music = nullptr;
    } else {
        // Move to prev for fade out
        if (m_music_prev) {
            ma_sound_uninit(m_music_prev);
            delete m_music_prev;
        }
        m_music_prev = m_music;
        m_music = nullptr;
        m_music_fade_out = fade_out;
        m_music_prev_fade_timer = fade_out;
    }
}

// ── Ambient ───────────────────────────────────────────────────────────────

SoundHandle AudioEngine::play_ambient(std::string_view path, glm::vec3 position) {
    if (!m_initialized) return {};
    std::string resolved = resolve_path(path);
    if (resolved.empty()) return {};

    auto* sound = new ma_sound;
    u32 flags = MA_SOUND_FLAG_DECODE;
    if (ma_sound_init_from_file(m_engine, resolved.c_str(), flags,
                                &GROUPS()[static_cast<u32>(Channel::Ambient)],
                                nullptr, sound) != MA_SUCCESS) {
        log::warn(TAG, "Failed to play ambient '{}'", path);
        delete sound;
        return {};
    }

    ma_sound_set_spatialization_enabled(sound, MA_TRUE);
    ma_sound_set_position(sound, position.x, position.y, position.z);
    ma_sound_set_min_distance(sound, 500.0f);
    ma_sound_set_max_distance(sound, 5000.0f);
    ma_sound_set_looping(sound, MA_TRUE);
    ma_sound_start(sound);

    u32 id = m_next_handle++;
    m_ambients[id] = {sound, 0, 0};
    return {id};
}

void AudioEngine::stop_ambient(SoundHandle handle, f32 fade_out) {
    if (!m_initialized) return;
    auto it = m_ambients.find(handle.id);
    if (it == m_ambients.end()) return;

    if (fade_out <= 0) {
        ma_sound_uninit(it->second.sound);
        delete it->second.sound;
        m_ambients.erase(it);
    } else {
        it->second.fade_out_duration = fade_out;
        it->second.fade_out_timer = fade_out;
    }
}

// ── Volume ────────────────────────────────────────────────────────────────

void AudioEngine::set_volume(Channel channel, f32 volume) {
    if (!m_initialized) return;
    u32 idx = static_cast<u32>(channel);
    if (idx < GROUP_COUNT) {
        ma_sound_group_set_volume(&GROUPS()[idx], std::clamp(volume, 0.0f, 1.0f));
    }
}

// ── Path resolution ───────────────────────────────────────────────────────

std::string AudioEngine::resolve_path(std::string_view path) const {
    namespace fs = std::filesystem;

    // Try map assets first
    if (!m_map_root.empty()) {
        std::string map_path = m_map_root + "/shared/assets/" + std::string(path);
        if (fs::exists(map_path)) return map_path;
    }

    // Try engine root
    std::string engine_path = "engine/" + std::string(path);
    if (fs::exists(engine_path)) return engine_path;

    // Try as absolute/relative
    if (fs::exists(path)) return std::string(path);

    log::warn(TAG, "Sound not found: '{}'", path);
    return {};
}

} // namespace uldum::audio
