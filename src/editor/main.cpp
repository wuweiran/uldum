#include "editor/editor.h"
#include "core/log.h"

#include <string>

int main(int argc, char* argv[]) {
    std::string map_path;
    if (argc > 1) {
        map_path = argv[1];
    }

    uldum::editor::Editor editor;

    if (!editor.init(map_path)) {
        uldum::log::error("Main", "Editor initialization failed");
        return 1;
    }

    editor.run();
    editor.shutdown();

    return 0;
}
