#include "app/app.h"
#include "core/log.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <cstring>
#include <string>

// Load game.json product configuration
static uldum::LaunchArgs load_game_config(int argc, char* argv[]) {
    uldum::LaunchArgs args;

    // Read game.json
    std::ifstream file("game.json");
    if (file.is_open()) {
        try {
            nlohmann::json j;
            file >> j;
            args.map_path = j.value("default_map", args.map_path);
            args.port     = static_cast<uldum::u16>(j.value("default_port", static_cast<int>(args.port)));
        } catch (...) {
            uldum::log::warn("Game", "Failed to parse game.json — using defaults");
        }
    }

    // CLI overrides
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0) {
            args.net_mode = uldum::network::Mode::Host;
        } else if (std::strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            args.net_mode = uldum::network::Mode::Client;
            args.connect_address = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            args.port = static_cast<uldum::u16>(std::stoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            args.map_path = argv[++i];
        }
    }

    return args;
}

#if defined(ULDUM_PLATFORM_WINDOWS) && !defined(ULDUM_DEBUG)
#include <Windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    // Parse command line for WinMain
    auto args = load_game_config(__argc, __argv);
#else
int main(int argc, char* argv[]) {
    auto args = load_game_config(argc, argv);
#endif

    uldum::App app;

    if (!app.init(args)) {
        uldum::log::error("Game", "Engine initialization failed");
        return 1;
    }

    app.run();
    app.shutdown();

    return 0;
}
