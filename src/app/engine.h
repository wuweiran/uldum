#pragma once

#include "platform/platform.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "render/renderer.h"
#include "audio/audio.h"
#include "network/game_server.h"
#include "network/network.h"
#include "input/command_system.h"
#include "input/selection.h"
#include "input/picking.h"
#include "input/input_preset.h"
#include "input/input_bindings.h"
#include "map/map.h"

#include <memory>
#include <string>

namespace uldum {

struct LaunchArgs {
    network::Mode net_mode = network::Mode::Offline;
    std::string connect_address;
    u16 port = 7777;
};

class Engine {
public:
    Engine() = default;
    ~Engine() = default;

    bool init(const LaunchArgs& args = {});
    void run();
    void shutdown();

private:
    // The World to render — server's world in offline/host, client world in client mode.
    simulation::World& active_world();

    LaunchArgs               m_args;

    // Modules (ordered by init dependency)
    std::unique_ptr<platform::Platform> m_platform;
    rhi::VulkanRhi           m_rhi;
    asset::AssetManager      m_asset;
    render::Renderer         m_renderer;
    audio::AudioEngine       m_audio;
    network::GameServer      m_server;
    network::NetworkManager  m_network;
    input::CommandSystem     m_commands;
    input::SelectionState    m_selection;
    input::Picker            m_picker;
    input::InputBindings     m_bindings;
    std::unique_ptr<input::InputPreset> m_input_preset;
    map::MapManager          m_map;
};

} // namespace uldum
