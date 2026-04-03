#include "app/engine.h"
#include "core/log.h"

// uldum_game entry point — shipped game, loads game.json configuration.
// Stub: Phase 13 (Packaging) will implement game.json loading and custom window title.

#if defined(ULDUM_PLATFORM_WINDOWS) && !defined(ULDUM_DEBUG)
#include <Windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main()
#endif
{
    uldum::Engine engine;

    if (!engine.init()) {
        uldum::log::error("Main", "Engine initialization failed");
        return 1;
    }

    engine.run();
    engine.shutdown();

    return 0;
}
