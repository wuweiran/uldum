# Uldum Package Format

Binary archive format for bundling engine and map assets. Supports compression and encryption to prevent casual extraction.

## Extensions

- `.uldpak` — generic asset package (used for engine: `engine.uldpak`)
- `.uldmap` — map package (used for maps: `maps/<name>.uldmap`)

Both use the same underlying format — the extension distinguishes role, not structure. A `.uldmap` file is a map; an `.uldpak` file is anything else.

## Design Goals

- **Fast random access**: O(log n) file lookup via sorted hash table
- **No plaintext filenames**: paths stored as 64-bit hashes
- **Compression**: LZ4 for fast decompression (optional per-file)
- **Encryption**: XOR with repeated key (v1), upgradeable to AES (future)
- **Simple**: single-file archive, no dependencies beyond LZ4

## File Layout

```
┌──────────────────────────────────┐
│ Header (16 bytes)                │
├──────────────────────────────────┤
│ File Table (24 bytes × N)        │
├──────────────────────────────────┤
│ Data Blobs (variable)            │
│   [blob 0] [blob 1] ... [blob N]│
└──────────────────────────────────┘
```

### Header (16 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | `"UPK\0"` (0x55, 0x50, 0x4B, 0x00) |
| 4 | 4 | version | Format version (currently 1) |
| 8 | 4 | file_count | Number of entries in file table |
| 12 | 4 | flags | Bit 0: compressed (LZ4), Bit 1: encrypted (XOR v1) |

### File Table Entry (24 bytes)

Entries are sorted by `name_hash` for binary search lookup.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 8 | name_hash | FNV-1a 64-bit hash of relative file path (forward slashes, lowercase) |
| 8 | 4 | offset | Byte offset from start of file to this entry's data blob |
| 12 | 4 | raw_size | Original uncompressed size in bytes |
| 16 | 4 | stored_size | Size as stored (after compression, before encryption) |
| 20 | 4 | reserved | Reserved for future use (must be 0) |

### Data Blobs

Each blob is the file's content, processed in order:
1. **Compress** (if flags bit 0): LZ4 block compression
2. **Encrypt** (if flags bit 1): XOR with repeated 32-byte key

To read a file: seek to `offset`, read `stored_size` bytes, decrypt (if encrypted), decompress (if compressed).

## Name Hashing

File paths are normalized before hashing:
- Forward slashes only (`/` not `\`)
- Lowercase
- No leading `./` or `/`
- Example: `shared/assets/textures/terrain/grass.png`

Hash function: FNV-1a 64-bit
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
