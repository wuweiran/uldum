#pragma once

#include "platform/platform.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "render/renderer.h"
#include "audio/audio.h"
#include "script/script.h"
#include "simulation/simulation.h"
#include "network/network.h"
#include "map/map.h"
#include "editor/editor.h"

#include <memory>

namespace uldum {

class Engine {
public:
    Engine() = default;
    ~Engine() = default;

    bool init();
    void run();
    void shutdown();

private:
    // Modules (ordered by init dependency)
    std::unique_ptr<platform::Platform> m_platform;
    rhi::VulkanRhi           m_rhi;
    asset::AssetManager      m_asset;
    render::Renderer         m_renderer;
    audio::AudioEngine       m_audio;
    script::ScriptEngine     m_script;
    simulation::Simulation   m_simulation;
    network::NetworkManager  m_network;
    map::MapManager          m_map;
    editor::Editor           m_editor;
};

} // namespace uldum
