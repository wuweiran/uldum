#include "audio/audio.h"
#include "core/log.h"

namespace uldum::audio {

static constexpr const char* TAG = "Audio";
static bool s_first_update = true;

bool AudioEngine::init() {
    log::info(TAG, "AudioEngine initialized (stub) — miniaudio device pending");
    return true;
}

void AudioEngine::shutdown() {
    log::info(TAG, "AudioEngine shut down (stub)");
}

void AudioEngine::update() {
    if (s_first_update) {
        log::trace(TAG, "update (stub) — will process 3D audio, streaming here");
        s_first_update = false;
    }
}

} // namespace uldum::audio
