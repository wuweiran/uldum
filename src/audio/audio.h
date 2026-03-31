#pragma once

#include <string_view>

namespace uldum::audio {

class AudioEngine {
public:
    bool init();
    void shutdown();
    void update();

    // Future API:
    // SoundHandle play_sfx(std::string_view asset, const Vec3& position);
    // void play_music(std::string_view asset, float fade_in_sec);
    // void stop_music(float fade_out_sec);
    // void set_listener(const Vec3& position, const Vec3& forward, const Vec3& up);
    // void set_volume(Channel channel, float volume);
};

} // namespace uldum::audio
