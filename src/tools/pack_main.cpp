// uldum_pack — Pack and unpack uldum asset archives (.uldpak / .uldmap).
// Usage:
//   uldum_pack pack <input_dir> <output> [--encrypt --key <secret>]
//   uldum_pack unpack <input> <output_dir> [--key <secret>]
//   uldum_pack list <input>

#include "asset/upk.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using namespace uldum;
using namespace uldum::asset;

// ── Pack command ────────────────────────────────────────────────────────────

static int cmd_pack(const std::string& input_dir, const std::string& output_path,
                    bool encrypt, const std::string& secret) {
    fs::path base(input_dir);
    if (!fs::is_directory(base)) {
        std::cerr << "ERROR: '" << input_dir << "' is not a directory\n";
        return 1;
    }

    struct FileInfo {
        std::string rel_path;
        u64         name_hash;
        fs::path    abs_path;
    };
    std::vector<FileInfo> files;

    for (auto& entry : fs::recursive_directory_iterator(base)) {
        if (!entry.is_regular_file()) continue;
        auto rel = fs::relative(entry.path(), base).string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        auto norm = upk_normalize_path(rel);
        files.push_back({norm, upk_hash(norm), entry.path()});
    }

    std::sort(files.begin(), files.end(), [](auto& a, auto& b) {
        return a.name_hash < b.name_hash;
    });

    std::cout << "Packing " << files.size() << " files from '" << input_dir << "'...\n";

    u8 key[UPK_KEY_LEN] = {};
    if (encrypt) upk_derive_key(secret, key);

    UPKHeader header{};
    std::memcpy(header.magic, UPK_MAGIC, 4);
    header.version    = UPK_VERSION;
    header.file_count = static_cast<u32>(files.size());
    header.flags      = encrypt ? UPK_FLAG_ENCRYPTED : 0;

    u32 table_size = static_cast<u32>(files.size() * sizeof(UPKEntry));

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "ERROR: Cannot create '" << output_path << "'\n";
        return 1;
    }

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    std::vector<UPKEntry> entries(files.size());
    auto table_pos = out.tellp();
    out.write(reinterpret_cast<const char*>(entries.data()), table_size);

    for (size_t i = 0; i < files.size(); ++i) {
        std::ifstream in(files[i].abs_path, std::ios::binary | std::ios::ate);
        auto size = in.tellg();
        in.seekg(0);
        std::vector<u8> data(static_cast<size_t>(size));
        in.read(reinterpret_cast<char*>(data.data()), size);

        u32 raw_size = static_cast<u32>(data.size());
        if (encrypt) upk_xor(data.data(), raw_size, key);

        u32 offset = static_cast<u32>(out.tellp());
        out.write(reinterpret_cast<const char*>(data.data()), data.size());

        entries[i] = {files[i].name_hash, offset, raw_size, static_cast<u32>(data.size()), 0};
    }

    out.seekp(table_pos);
    out.write(reinterpret_cast<const char*>(entries.data()), table_size);
    out.seekp(0, std::ios::end);
    auto total = out.tellp();
    out.close();

    std::cout << "Created '" << output_path << "' (" << files.size() << " files, "
              << total << " bytes)\n";
    return 0;
}

// ── Unpack command ──────────────────────────────────────────────────────────

static int cmd_unpack(const std::string& input_path, const std::string& output_dir,
                      const std::string& secret) {
    UPKReader reader;
    if (!reader.open(input_path, secret)) {
        std::cerr << "ERROR: Failed to open '" << input_path << "'\n";
        return 1;
    }

    std::cerr << "NOTE: File names are hashed — extracting with hash-based names.\n";

    fs::create_directories(output_dir);

    // We need to re-read the entries directly for offset/size info
    std::ifstream in(input_path, std::ios::binary);
    UPKHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    std::vector<UPKEntry> entries(header.file_count);
    in.read(reinterpret_cast<char*>(entries.data()), header.file_count * sizeof(UPKEntry));

    for (auto& e : entries) {
        auto data = reader.read_by_hash(e.name_hash);
        if (data.empty()) continue;

        char name[32];
        std::snprintf(name, sizeof(name), "%016llx",
                      static_cast<unsigned long long>(e.name_hash));

        auto path = fs::path(output_dir) / name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        std::cout << "  " << name << " (" << data.size() << " bytes)\n";
    }

    std::cout << "Extracted " << header.file_count << " files to '" << output_dir << "'\n";
    return 0;
}

// ── List command ────────────────────────────────────────────────────────────

static int cmd_list(const std::string& input_path) {
    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        std::cerr << "ERROR: Cannot open '" << input_path << "'\n";
        return 1;
    }

    UPKHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (std::memcmp(header.magic, UPK_MAGIC, 4) != 0) {
        std::cerr << "ERROR: Not a UPK file\n";
        return 1;
    }

    std::vector<UPKEntry> entries(header.file_count);
    in.read(reinterpret_cast<char*>(entries.data()), header.file_count * sizeof(UPKEntry));

    std::cout << "UPK v" << header.version << " — " << header.file_count << " files";
    if (header.flags & UPK_FLAG_ENCRYPTED) std::cout << " [encrypted]";
    if (header.flags & UPK_FLAG_COMPRESSED) std::cout << " [compressed]";
    std::cout << "\n\n";

    std::cout << "  Hash              Raw Size   Stored Size\n";
    std::cout << "  ----------------  ---------  -----------\n";

    u64 total_raw = 0, total_stored = 0;
    for (auto& e : entries) {
        char line[80];
        std::snprintf(line, sizeof(line), "  %016llx  %9u  %11u",
                      static_cast<unsigned long long>(e.name_hash), e.raw_size, e.stored_size);
        std::cout << line << "\n";
        total_raw += e.raw_size;
        total_stored += e.stored_size;
    }

    std::cout << "\n  Total: " << total_raw << " bytes raw, " << total_stored << " bytes stored\n";
    return 0;
}

// ── Main ────────────────────────────────────────────────────────────────────

static void print_usage() {
    std::cout <<
        "uldum_pack — Uldum Package Tool\n"
        "\n"
        "Usage:\n"
        "  uldum_pack pack <input_dir> <output.upk> [--encrypt --key <secret>]\n"
        "  uldum_pack unpack <input.upk> <output_dir> [--key <secret>]\n"
        "  uldum_pack list <input.upk>\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) { print_usage(); return 1; }

    std::string cmd = argv[1];
    bool encrypt = false;
    std::string key_secret;

    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--encrypt") == 0) encrypt = true;
        else if (std::strcmp(argv[i], "--key") == 0 && i + 1 < argc) key_secret = argv[++i];
    }

    if (cmd == "pack" && argc >= 4) {
        return cmd_pack(argv[2], argv[3], encrypt, key_secret);
    } else if (cmd == "unpack" && argc >= 4) {
        return cmd_unpack(argv[2], argv[3], key_secret);
    } else if (cmd == "list" && argc >= 3) {
        return cmd_list(argv[2]);
    } else {
        print_usage();
        return 1;
    }
}
