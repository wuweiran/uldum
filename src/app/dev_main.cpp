#include "app/engine.h"
#include "core/log.h"

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
