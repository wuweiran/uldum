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
        std::string rel_path;   // normalized
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

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "ERROR: Cannot create '" << output_path << "'\n";
        return 1;
    }

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Entry-table layout per entry (variable): u16 path_len, path bytes,
    // u32 offset, u32 raw_size, u32 stored_size. Reserve it by writing
    // zeros of the correct total size, then rewrite once blob offsets are
    // known.
    auto table_pos = out.tellp();
    std::size_t table_size = 0;
    for (auto& f : files) {
        table_size += sizeof(u16) + f.rel_path.size() + 3 * sizeof(u32);
    }
    std::vector<char> zero(table_size, 0);
    out.write(zero.data(), table_size);

    // Write blobs, capture offsets.
    struct WrittenEntry { u32 offset, raw_size, stored_size; };
    std::vector<WrittenEntry> written(files.size());

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

        written[i] = {offset, raw_size, static_cast<u32>(data.size())};
    }

    // Rewind and write the entry table now that offsets are known.
    out.seekp(table_pos);
    for (size_t i = 0; i < files.size(); ++i) {
        u16 path_len = static_cast<u16>(files[i].rel_path.size());
        out.write(reinterpret_cast<const char*>(&path_len), sizeof(path_len));
        out.write(files[i].rel_path.data(), path_len);
        out.write(reinterpret_cast<const char*>(&written[i].offset),      sizeof(u32));
        out.write(reinterpret_cast<const char*>(&written[i].raw_size),    sizeof(u32));
        out.write(reinterpret_cast<const char*>(&written[i].stored_size), sizeof(u32));
    }

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

    fs::create_directories(output_dir);

    // We need the (path, offset, sizes) trio to extract. Re-parse the entry
    // table directly — UPKReader exposes hash lookup but we want paths here.
    std::ifstream in(input_path, std::ios::binary);
    UPKHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));

    u32 extracted = 0;
    for (u32 i = 0; i < header.file_count; ++i) {
        u16 path_len = 0;
        in.read(reinterpret_cast<char*>(&path_len), sizeof(path_len));
        std::string path(path_len, '\0');
        if (path_len) in.read(path.data(), path_len);
        u32 offset = 0, raw_size = 0, stored_size = 0;
        in.read(reinterpret_cast<char*>(&offset),      sizeof(u32));
        in.read(reinterpret_cast<char*>(&raw_size),    sizeof(u32));
        in.read(reinterpret_cast<char*>(&stored_size), sizeof(u32));

        auto data = reader.read(path);
        if (data.empty()) {
            std::cerr << "WARN: empty data for '" << path << "'\n";
            continue;
        }

        fs::path out_path = fs::path(output_dir) / path;
        fs::create_directories(out_path.parent_path());
        std::ofstream out(out_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        ++extracted;
    }

    std::cout << "Extracted " << extracted << " files to '" << output_dir << "'\n";
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
    if (header.version != UPK_VERSION) {
        std::cerr << "ERROR: UPK version " << header.version
                  << " not supported (expected " << UPK_VERSION << ")\n";
        return 1;
    }

    std::cout << "UPK v" << header.version << " — " << header.file_count << " files";
    if (header.flags & UPK_FLAG_ENCRYPTED)  std::cout << " [encrypted]";
    if (header.flags & UPK_FLAG_COMPRESSED) std::cout << " [compressed]";
    std::cout << "\n\n";

    std::cout << "  Raw Size   Stored Size  Path\n";
    std::cout << "  ---------  -----------  ----\n";

    u64 total_raw = 0, total_stored = 0;
    for (u32 i = 0; i < header.file_count; ++i) {
        u16 path_len = 0;
        in.read(reinterpret_cast<char*>(&path_len), sizeof(path_len));
        std::string path(path_len, '\0');
        if (path_len) in.read(path.data(), path_len);
        u32 offset = 0, raw_size = 0, stored_size = 0;
        in.read(reinterpret_cast<char*>(&offset),      sizeof(u32));
        in.read(reinterpret_cast<char*>(&raw_size),    sizeof(u32));
        in.read(reinterpret_cast<char*>(&stored_size), sizeof(u32));

        char line[128];
        std::snprintf(line, sizeof(line), "  %9u  %11u  %s",
                      raw_size, stored_size, path.c_str());
        std::cout << line << "\n";
        total_raw += raw_size;
        total_stored += stored_size;
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
        "  uldum_pack pack   <input_dir> <output>    [--encrypt --key <secret>]\n"
        "  uldum_pack unpack <input>     <output_dir> [--key <secret>]\n"
        "  uldum_pack list   <input>\n";
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
