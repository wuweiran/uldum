#pragma once

// Lua script validation — dev/editor only, NEVER linked into the shipping
// game (see src/script/CMakeLists.txt: uldum_scriptcheck is added only to
// uldum_dev and uldum_editor). Two tiers:
//
//   Tier 1 — check_syntax: compile with luaL_loadbuffer (parse, don't run) to
//            catch syntax errors. Depends on the Lua C API only.
//   Tier 2 — check_globals: a hand-rolled Lua 5.4 lexer + scope-tracking parser
//            (NO Lua internals, NO external tool) that flags reads of globals
//            not in a known set. The known set is extracted from engine source
//            (script.cpp bindings + constants.lua) so it can't drift. Catches
//            the "typo'd API name" regression class (PanCamera → CameraSet...).

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace uldum::script {

// One syntax error: the chunk name passed in (typically a file path), the
// 1-based line the Lua parser reported, and its message.
struct ScriptSyntaxError {
    std::string chunk;   // name given to check_syntax (e.g. the file path)
    int         line = 0;
    std::string message;
};

// Parse `source` without executing it. Returns nullopt if it compiles clean,
// or the parse error. `chunk_name` is used both as the Lua chunk name (so the
// error's line refers to it) and echoed back in the result.
std::optional<ScriptSyntaxError> check_syntax(std::string_view source,
                                              std::string_view chunk_name);

// Convenience: check a batch of (chunk_name, source) pairs, collecting every
// failure. Empty result = all clean.
struct NamedSource {
    std::string name;
    std::string source;
};
std::vector<ScriptSyntaxError> check_all(const std::vector<NamedSource>& scripts);

// ── Tier 2: undefined-global detection ──────────────────────────────────────

// One flagged global read: where a name that isn't local, isn't a known
// engine/stdlib global, and isn't defined anywhere in the file was used.
struct UndefinedGlobal {
    std::string chunk;
    int         line = 0;
    int         column = 0;
    std::string name;
};

// The set of names treated as "defined" globals: engine bindings, script
// constants, and the Lua stdlib baseline. Built by extract_known_globals.
using GlobalSet = std::unordered_set<std::string>;

// Extract the known-globals set straight from engine SOURCE so it can never
// drift from the real bindings:
//   - script_cpp_src : contents of src/script/script.cpp (scans lua["Name"]).
//   - constants_lua_src: contents of engine/scripts/constants.lua (top-level
//     NAME = ... assignments).
// The Lua 5.4 stdlib baseline (pairs, ipairs, string, table, math, ...) is
// always included. Any argument may be empty; the stdlib baseline still applies.
GlobalSet extract_known_globals(std::string_view script_cpp_src,
                                std::string_view constants_lua_src);

// Scan `source` for reads of globals not in `known`. A global assigned
// anywhere at file scope (e.g. `spawn_timer = ...`, `function Foo()`) is
// treated as defined for the whole file (Lua global semantics), so forward
// references don't false-positive. Returns empty if the source doesn't parse
// (syntax errors are Tier 1's job — this avoids garbage output on broken input).
std::vector<UndefinedGlobal> check_globals(std::string_view source,
                                           std::string_view chunk_name,
                                           const GlobalSet& known);

// Project-scoped variant: the engine loads all of a map's scripts into ONE
// shared global environment, so a global function defined in combat.lua is
// callable from main.lua. This does two passes over the whole set — pass 1
// collects every file's file-scope global writes, pass 2 flags reads against
// (known ∪ all-writes). Use this for a map's script folder; single-file
// check_globals would false-positive on legitimate cross-file globals.
std::vector<UndefinedGlobal> check_globals_project(const std::vector<NamedSource>& scripts,
                                                   const GlobalSet& known);

} // namespace uldum::script

