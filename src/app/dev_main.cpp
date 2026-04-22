#include "app/app.h"
#include "core/log.h"

#include <cstring>
#include <string>

// WC3-style CLI: `uldum_dev --map <path>` auto-starts an offline game of
// that map. Any other launch drops the user in the dev-console menu to
// pick a map and/or configure multiplayer.
static uldum::LaunchArgs parse_args(int argc, char* argv[]) {
    uldum::LaunchArgs args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            args.map_path   = argv[++i];
            args.net_mode   = uldum::network::Mode::Offline;
            args.auto_start = true;
        }
    }
    return args;
}

int main(int argc, char* argv[]) {
    auto args = parse_args(argc, argv);
    uldum::App app;

    if (!app.init(args)) {
        uldum::log::error("Main", "Engine initialization failed");
        return 1;
    }

    app.run();
    app.shutdown();

    return 0;
}
