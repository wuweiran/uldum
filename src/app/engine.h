#pragma once

#include "platform/platform.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "render/renderer.h"
#include "audio/audio.h"
#include "script/script.h"
#include "simulation/simulation.h"
#include "network/network.h"
#include "input/command_system.h"
#include "input/selection.h"
#include "input/picking.h"
#include "map/map.h"

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
    input::CommandSystem     m_commands;
    input::SelectionState    m_selection;
    input::Picker            m_picker;
    map::MapManager          m_map;
};

} // namespace uldum
