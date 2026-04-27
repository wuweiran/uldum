# Uldum Package Format

Binary archive format for bundling engine and map assets. Conceptually a folder in a single file: pack a directory and unpack it back out to get the original tree. Supports optional XOR encryption on data blobs.

## Extensions

- `.uldpak` — generic asset package (used for engine: `engine.uldpak`)
- `.uldmap` — map package (used for maps: `maps/<name>.uldmap`)

Both use the same underlying format — the extension distinguishes role, not structure. A `.uldmap` file is a map; an `.uldpak` file is anything else.

## Design Goals

- **Folder-in-a-file**: pack and unpack round-trip a directory losslessly (same paths, same structure)
- **Fast random access**: O(log n) file lookup (binary search on hashed paths, cached in memory at open time)
- **Compression**: LZ4 per-blob (optional, future)
- **Encryption**: XOR with repeated key on data blobs (data only; paths are plaintext)
- **Simple**: single file, minimal external dependencies

## File Layout

```
┌──────────────────────────────────┐
│ Header (16 bytes)                │
├──────────────────────────────────┤
│ Entry table (variable)           │
│   [entry 0] [entry 1] ... [N-1]  │
├──────────────────────────────────┤
│ Data blobs (contiguous)          │
│   [blob 0] [blob 1] ... [N-1]    │
└──────────────────────────────────┘
```

### Header (16 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | `"UPK\0"` (0x55, 0x50, 0x4B, 0x00) |
| 4 | 4 | version | Format version (currently 1) |
| 8 | 4 | file_count | Number of entries in the entry table |
| 12 | 4 | flags | Bit 0: compressed (per-blob LZ4, future), Bit 1: encrypted (XOR) |

### Entry Table (variable-length)

Each entry, in order:

| Field | Size | Description |
|-------|------|-------------|
| path_len    | u16            | Byte length of the path string |
| path        | char[path_len] | Relative path (normalized: forward slashes, lowercase, UTF-8) |
| offset      | u32            | Byte offset from start of file to this entry's data blob |
| raw_size    | u32            | Original uncompressed size |
| stored_size | u32            | Size as stored (after compression/encryption) |

Paths are plaintext. The runtime computes an FNV-1a 64-bit hash of the path at open time and caches a sorted (hash → entry) table for O(log n) lookups. The on-disk format carries the path string so tools (`uldum_pack unpack`, editor) can reconstruct the original folder.

### Data Blobs

Each blob's stored bytes are processed in order on write (reverse on read):
1. **Compress** (if flags bit 0): LZ4 block compression (future)
2. **Encrypt** (if flags bit 1): XOR with repeated 32-byte key

Read path: seek to `offset`, read `stored_size` bytes, decrypt (if encrypted), decompress (if compressed).

## Path Normalization

Paths are normalized before lookup (and before the hash is computed):
- Forward slashes only (`/` not `\`)
- Lowercase
- No leading `./` or `/`
- Example: `textures/terrain/grass.ktx2`

Hash function (FNV-1a 64-bit), used internally for fast lookup:
```
hash = 14695981039346656037 (FNV offset basis)
for each byte:
    hash ^= byte
    hash *= 1099511628211 (FNV prime)
```

## Encryption (v1)

XOR with a 32-byte key repeated over the data:
```
encrypted[i] = plain[i] ^ key[i % 32]
```

The key is derived from a secret string baked into the executable. Not cryptographically secure — prevents casual extraction, not determined reverse-engineering.

Future versions may use AES-256-CBC (indicated by flags or version field).

## Tool Usage

`uldum_pack` handles both packing and unpacking:

```
# Pack a directory into a package
uldum_pack pack <input_dir> <output> [--compress] [--encrypt --key <secret>]

# Unpack a package to a directory
uldum_pack unpack <input> <output_dir> [--key <secret>]

# List contents (hashes + sizes, not names)
uldum_pack list <input>
```

Output extension is not enforced by the tool — pass `.uldpak` or `.uldmap` as appropriate for the contents.

### Examples

```bash
# Pack engine assets (compressed, no encryption)
uldum_pack pack engine engine.uldpak --compress

# Pack a map (compressed + encrypted)
uldum_pack pack maps/test_map.uldmap build/bin/maps/test_map.uldmap --compress --encrypt --key "my_secret"

# Unpack for inspection
uldum_pack unpack test_map.uldmap extracted/ --key "my_secret"

# List files
uldum_pack list engine.uldpak
```

## Runtime Loading

The `AssetManager` maintains an ordered list of **mounts**, each bound to a virtual path prefix. A mount is one of two kinds:

- **Package mount** — reads from a `.uldpak` / `.uldmap` archive file
- **Directory mount** — reads from a filesystem directory (via `ifstream`)

Both satisfy the same `read_file_bytes(virtual_path) → bytes` interface. All asset loading (textures, models, configs, shaders, scripts, terrain, effects) routes through this. There is no silent filesystem fallback: if a file isn't in any mount, the load surfaces as an error.

### Auto-mounts

- `engine.uldpak` is auto-mounted as a package at prefix `engine/` in `AssetManager::init` — engine assets are always packed, for every target.
- A map is auto-mounted at prefix `<map_root>/` in `MapManager::load_map`. The backing storage depends on the target:

| Target | Accepts `.uldmap` file | Accepts `.uldmap` directory |
|---|---|---|
| `uldum_dev`    | ✅ | ❌ |
| `uldum_game`   | ✅ | ❌ |
| `uldum_server` | ✅ | ❌ |
| `uldum_editor` | ✅ | ✅ |

Only the editor can point at a loose source directory. Everything else consumes the packaged `.uldmap` file that the build produced.

### Why the two mount kinds

Shipped builds want packages — single file, compressed, optionally encrypted. The editor wants to edit loose files in the source tree and see changes immediately, without a repack round-trip. The mount abstraction lets both coexist behind one `read_file_bytes` API; the policy (who's allowed to mount what) lives in the caller (`MapManager::load_map`'s `allow_directory` parameter).

Source trees (`engine/`, `maps/<name>.uldmap/`) remain directories; the build pipeline packs them into the `.uldpak` / `.uldmap` files that runtime targets consume. The editor bypasses the packaged artifact and opens the source directory directly so saves land where they belong.

## Archive Strategy

| Archive | Contents | Compression | Encryption |
|---------|----------|-------------|------------|
| `engine.uldpak` | Engine shaders, textures, configs, scripts | Yes | No (public assets) |
| `<map>.uldmap` | Map terrain, scripts, assets, types | Yes | Yes (protect gameplay) |

## Texture Format

**KTX2 + Basis Universal** is the only texture format the engine accepts. This applies to every target (`uldum_dev`, `uldum_game`, `uldum_server`, `uldum_editor`) and every mount (`engine.uldpak`, `.uldmap` file, loose directory). A PNG inside a mount is a load error — there is no format fallback, and no bake step in the engine pipeline.

Library: [Basis Universal](https://github.com/BinomialLLC/basis_universal) provides both the runtime transcoder (linked into `uldum_asset` via `basisu_transcoder.cpp` compiled as a small static lib) and the authoring CLI (`basisu.exe` built as part of the engine build, lands in `build/bin/`). One KTX2 payload transcodes to BC7 on desktop and ASTC on Android at GPU upload time.

**Why no in-engine bake:** the engine stays out of texture production. Map makers use their own tools (Photoshop, GIMP, Substance, etc.) plus `basisu` — the Basis Universal CLI — to produce KTX2 files directly. That mirrors how they already produce glTF models, OGG audio, and Lua scripts: finished authored files in the map folder. `uldum_pack pack` archives them as-is.

**Author workflow (PNG → KTX2):**

```
basisu -ktx2 -uastc -uastc_level 2 -mipmap          -output_file tex.ktx2 tex.png   # albedo / sRGB (default)
basisu -ktx2 -uastc -uastc_level 2 -mipmap -linear  -output_file tex.ktx2 tex.png   # normals / data / linear
```

A helper script `scripts/png_to_ktx2.ps1` wraps these flags — see [editor.md](editor.md).
