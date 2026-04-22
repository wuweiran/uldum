#pragma once

#include <RmlUi/Core/FileInterface.h>

#include "core/types.h"

#include <unordered_map>
#include <vector>

namespace uldum::ui {

// Routes RmlUi's file loading through our AssetManager so RML/RCSS/font
// files come from the same places as the rest of the engine's assets —
// filesystem in dev, engine.uldpak or APK assets at runtime.
//
// RmlUi's FileHandle is an opaque cookie; we keep a table of open handles
// holding the file's bytes + current read offset.
class FileInterface final : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String& path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;
    size_t Length(Rml::FileHandle file) override;

private:
    struct OpenFile {
        std::vector<u8> bytes;
        size_t          offset = 0;
    };
    std::unordered_map<Rml::FileHandle, OpenFile> m_files;
    Rml::FileHandle m_next_handle = 1;
};

} // namespace uldum::ui
