#include "app/engine.h"
#include "core/log.h"

#include <cstring>
#include <string>

static uldum::LaunchArgs parse_args(int argc, char* argv[]) {
    uldum::LaunchArgs args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0) {
            args.net_mode = uldum::network::Mode::Host;
        } else if (std::strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            args.net_mode = uldum::network::Mode::Client;
            args.connect_address = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            args.port = static_cast<uldum::u16>(std::stoi(argv[++i]));
        }
    }
    return args;
}

int main(int argc, char* argv[]) {
    auto args = parse_args(argc, argv);
    uldum::Engine engine;

    if (!engine.init(args)) {
        uldum::log::error("Main", "Engine initialization failed");
        return 1;
    }

    engine.run();
    engine.shutdown();

    return 0;
}
