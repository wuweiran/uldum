#include "script/script_check.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace uldum::script {

std::optional<ScriptSyntaxError> check_syntax(std::string_view source,
                                              std::string_view chunk_name) {
    lua_State* L = luaL_newstate();
    if (!L) {
        return ScriptSyntaxError{std::string(chunk_name), 0,
                                 "failed to create Lua state"};
    }

    // luaL_loadbuffer PARSES into a chunk on the stack but does NOT run it —
    // exactly what we want. Non-zero return = syntax error, with the message
    // pushed as a string. The chunk name is prefixed with '=' so Lua uses it
    // verbatim (no "[string ...]" truncation) in the error text.
    std::string chunk_id = "=" + std::string(chunk_name);
    int rc = luaL_loadbuffer(L, source.data(), source.size(), chunk_id.c_str());

    std::optional<ScriptSyntaxError> result;
    if (rc != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        std::string full = msg ? msg : "unknown syntax error";

        // Lua formats the error as "<chunk>:<line>: <message>". Peel off the
        // chunk prefix and the line number so callers get structured fields.
        ScriptSyntaxError err;
        err.chunk = std::string(chunk_name);
        err.line  = 0;
        err.message = full;

        // Find ":<digits>:" after the chunk name to extract the line.
        std::string prefix = std::string(chunk_name) + ":";
        if (full.rfind(prefix, 0) == 0) {
            size_t p = prefix.size();
            int line = 0;
            size_t d = p;
            while (d < full.size() && full[d] >= '0' && full[d] <= '9') {
                line = line * 10 + (full[d] - '0');
                ++d;
            }
            if (d > p && d < full.size() && full[d] == ':') {
                err.line = line;
                // Message after ":<line>: "
                size_t m = d + 1;
                while (m < full.size() && full[m] == ' ') ++m;
                err.message = full.substr(m);
            }
        }
        result = std::move(err);
    }

    lua_close(L);
    return result;
}

std::vector<ScriptSyntaxError> check_all(const std::vector<NamedSource>& scripts) {
    std::vector<ScriptSyntaxError> errors;
    for (const auto& s : scripts) {
        if (auto e = check_syntax(s.source, s.name)) {
            errors.push_back(std::move(*e));
        }
    }
    return errors;
}

// ── Tier 2: undefined-global detection ──────────────────────────────────────
//
// A hand-written Lua 5.4 lexer + recursive-descent scope resolver. It does NOT
// evaluate anything and does NOT touch Lua internals — it walks the grammar
// tracking lexical scope, records every *read* of a name that isn't a local,
// and every name *written* as a global at file scope. After the walk, a read
// is "undefined" if its name is neither a known global (engine/stdlib) nor a
// file-scope global write (Lua globals are visible file-wide, so forward refs
// are fine). Runs only after Tier-1 syntax passes; on any internal parse
// desync it bails to empty (never emits false positives on valid code).

namespace {

// ---- Lexer ----------------------------------------------------------------

enum class Tok { Name, Keyword, Number, String, Op, Eof };

struct Token {
    Tok         kind = Tok::Eof;
    std::string text;   // Name/Keyword/Op spelling (empty for Number/String)
    int         line = 1;
    int         col  = 1;
};

bool is_keyword(std::string_view s) {
    static const std::unordered_set<std::string> kw = {
        "and","break","do","else","elseif","end","false","for","function",
        "goto","if","in","local","nil","not","or","repeat","return","then",
        "true","until","while"};
    return kw.count(std::string(s)) != 0;
}

struct Lexer {
    std::string_view src;
    size_t pos = 0;
    int line = 1, col = 1;
    bool ok = true;   // cleared on a lexing anomaly

    char cur()  const { return pos < src.size() ? src[pos] : '\0'; }
    char peek(size_t o) const { return pos + o < src.size() ? src[pos + o] : '\0'; }

    void adv() {
        if (pos >= src.size()) return;
        if (src[pos] == '\n') { ++line; col = 1; }
        else ++col;
        ++pos;
    }

    // Consume a long bracket body `[==[ ... ]==]` given the opening level
    // (number of '=' between the brackets). Assumes the opening `[==[` is
    // already consumed. Counts newlines so line numbers stay accurate.
    void skip_long(int level) {
        while (pos < src.size()) {
            if (cur() == ']') {
                size_t save = pos;
                adv();
                int n = 0;
                while (cur() == '=') { adv(); ++n; }
                if (n == level && cur() == ']') { adv(); return; }
                pos = save; // not a close; consume the ']' literally
                adv();
            } else {
                adv();
            }
        }
    }

    // If at a `[` that begins a long bracket, return its level (>=0) and
    // consume the opener; else return -1 and consume nothing.
    int try_open_long() {
        if (cur() != '[') return -1;
        size_t save = pos; int scol = col, sline = line;
        adv();
        int level = 0;
        while (cur() == '=') { adv(); ++level; }
        if (cur() == '[') { adv(); return level; }
        pos = save; col = scol; line = sline; // rewind
        return -1;
    }

    void skip_ws_and_comments() {
        for (;;) {
            char c = cur();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { adv(); continue; }
            if (c == '-' && peek(1) == '-') {
                adv(); adv();
                int level = try_open_long();
                if (level >= 0) { skip_long(level); continue; }
                while (pos < src.size() && cur() != '\n') adv();
                continue;
            }
            break;
        }
    }

    Token next() {
        skip_ws_and_comments();
        Token t; t.line = line; t.col = col;
        char c = cur();
        if (c == '\0') { t.kind = Tok::Eof; return t; }

        // Name / keyword
        if (std::isalpha((unsigned char)c) || c == '_') {
            std::string s;
            while (std::isalnum((unsigned char)cur()) || cur() == '_') { s += cur(); adv(); }
            t.text = s;
            t.kind = is_keyword(s) ? Tok::Keyword : Tok::Name;
            return t;
        }
        // Number
        if (std::isdigit((unsigned char)c) || (c == '.' && std::isdigit((unsigned char)peek(1)))) {
            char prev = 0;
            while (pos < src.size()) {
                char d = cur();
                if (std::isalnum((unsigned char)d) || d == '.') { prev = d; adv(); }
                else if ((d == '+' || d == '-') &&
                         (prev == 'e' || prev == 'E' || prev == 'p' || prev == 'P')) { prev = d; adv(); }
                else break;
            }
            t.kind = Tok::Number;
            return t;
        }
        // Short string
        if (c == '"' || c == '\'') {
            char q = c; adv();
            while (pos < src.size() && cur() != q) {
                if (cur() == '\\') { adv(); if (pos < src.size()) adv(); }
                else adv();
            }
            if (cur() == q) adv(); else ok = false;
            t.kind = Tok::String;
            return t;
        }
        // Long string
        if (c == '[') {
            int level = try_open_long();
            if (level >= 0) { skip_long(level); t.kind = Tok::String; return t; }
        }
        // Operators / punctuation (longest match first)
        static const char* multi[] = {"...","..","::","<<",">>","//","==","~=","<=",">="};
        for (const char* m : multi) {
            size_t len = std::strlen(m);
            if (src.compare(pos, len, m) == 0) {
                t.text = m; t.kind = Tok::Op;
                for (size_t i = 0; i < len; ++i) adv();
                return t;
            }
        }
        t.text = std::string(1, c); t.kind = Tok::Op; adv();
        return t;
    }
};

// ---- Parser + scope resolver ----------------------------------------------

struct Resolver {
    std::vector<Token> toks;
    size_t i = 0;
    std::vector<std::unordered_set<std::string>> scopes;  // lexical local scopes
    std::vector<UndefinedGlobal> reads;   // global read-sites (name not local)
    std::unordered_set<std::string> global_writes;        // names assigned at file scope
    std::string chunk;
    bool failed = false;

    const Token& tk() const { return toks[i]; }
    const Token& tk2() const { return toks[i + 1 < toks.size() ? i + 1 : toks.size() - 1]; }
    void adv() { if (i + 1 < toks.size()) ++i; }
    bool at_eof() const { return tk().kind == Tok::Eof; }

    bool is_op(const char* s) const { return tk().kind == Tok::Op && tk().text == s; }
    bool is_kw(const char* s) const { return tk().kind == Tok::Keyword && tk().text == s; }

    // Expect an operator/keyword; on mismatch mark failed (bails the whole run).
    void expect_op(const char* s) { if (is_op(s)) adv(); else failed = true; }

    void push() { scopes.emplace_back(); }
    void pop()  { if (!scopes.empty()) scopes.pop_back(); }
    void declare(const std::string& n) { if (!scopes.empty()) scopes.back().insert(n); }
    bool is_local(const std::string& n) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            if (it->count(n)) return true;
        return false;
    }
    void record_read(const Token& t) {
        if (!is_local(t.text)) reads.push_back({chunk, t.line, t.col, t.text});
    }
    void record_write(const std::string& n) {
        if (!is_local(n)) global_writes.insert(n);
    }

    // A bare Name not yet committed to read-or-write (assignment target vs use).
    struct Pending { bool valid = false; Token tok; };

    // primaryexp: '(' exp ')' | Name
    Pending primary() {
        Pending p;
        if (is_op("(")) { adv(); expr(); expect_op(")"); return p; }
        if (tk().kind == Tok::Name) { p.valid = true; p.tok = tk(); adv(); return p; }
        failed = true; return p;
    }

    void args() {
        if (is_op("(")) {
            adv();
            if (!is_op(")")) { exprlist(); }
            expect_op(")");
        } else if (tk().kind == Tok::String) {
            adv();                       // f "str"
        } else if (is_op("{")) {
            table();                     // f {tbl}
        } else {
            failed = true;
        }
    }

    // suffixedexp: primary { '.'Name | '['exp']' | ':'Name args | args }
    // Returns a pending bare Name only when NO suffix followed (so the caller,
    // an assignment lvalue, can decide read-vs-write). Any suffix commits the
    // base as a read immediately.
    Pending suffixed() {
        Pending p = primary();
        for (;;) {
            if (is_op(".")) {
                if (p.valid) { record_read(p.tok); p.valid = false; }
                adv();
                if (tk().kind == Tok::Name) adv(); else { failed = true; return p; }
            } else if (is_op("[")) {
                if (p.valid) { record_read(p.tok); p.valid = false; }
                adv(); expr(); expect_op("]");
            } else if (is_op(":")) {
                if (p.valid) { record_read(p.tok); p.valid = false; }
                adv();
                if (tk().kind == Tok::Name) adv(); else { failed = true; return p; }
                args();
            } else if (is_op("(") || is_op("{") || tk().kind == Tok::String) {
                if (p.valid) { record_read(p.tok); p.valid = false; }
                args();
            } else {
                break;
            }
            if (failed) return p;
        }
        return p;
    }

    void table() {
        expect_op("{");
        while (!is_op("}") && !at_eof() && !failed) {
            if (is_op("[")) {                       // [exp] = exp
                adv(); expr(); expect_op("]"); expect_op("="); expr();
            } else if (tk().kind == Tok::Name && tk2().kind == Tok::Op && tk2().text == "=") {
                adv(); adv(); expr();               // Name = exp  (Name is a key, not a read)
            } else {
                expr();                             // positional exp
            }
            if (is_op(",") || is_op(";")) adv(); else break;
        }
        expect_op("}");
    }

    // funcbody: '(' [parlist] ')' block 'end'   (self declared by caller if method)
    void funcbody(bool is_method) {
        push();
        if (is_method) declare("self");
        expect_op("(");
        if (!is_op(")")) {
            for (;;) {
                if (is_op("...")) { adv(); break; }
                if (tk().kind == Tok::Name) { declare(tk().text); adv(); }
                else { failed = true; break; }
                if (is_op(",")) { adv(); continue; }
                break;
            }
        }
        expect_op(")");
        block();
        if (is_kw("end")) adv(); else failed = true;
        pop();
    }

    // simpleexp operand
    void simpleexp() {
        if (tk().kind == Tok::Number || tk().kind == Tok::String ||
            is_kw("nil") || is_kw("true") || is_kw("false") || is_op("...")) {
            adv(); return;
        }
        if (is_op("{")) { table(); return; }
        if (is_kw("function")) { adv(); funcbody(false); return; }
        if (is_op("(") || tk().kind == Tok::Name) {
            Pending p = suffixed();
            if (p.valid) record_read(p.tok);   // bare name in expr context = read
            return;
        }
        failed = true;
    }

    bool is_unop() const {
        return is_kw("not") || is_op("-") || is_op("#") || is_op("~");
    }
    bool is_binop() const {
        if (is_kw("and") || is_kw("or")) return true;
        if (tk().kind != Tok::Op) return false;
        static const std::unordered_set<std::string> b = {
            "+","-","*","/","//","%","^","..","<","<=",">",">=","==","~=",
            "&","|","~","<<",">>"};
        return b.count(tk().text) != 0;
    }

    void expr() {
        while (is_unop() && !failed) adv();
        simpleexp();
        while (is_binop() && !failed) {
            adv();
            while (is_unop() && !failed) adv();
            simpleexp();
        }
    }

    void exprlist() { expr(); while (is_op(",") && !failed) { adv(); expr(); } }

    // ---- statements ----
    void block() {
        while (!failed && !at_eof() &&
               !is_kw("end") && !is_kw("else") && !is_kw("elseif") && !is_kw("until")) {
            statement();
        }
    }

    void funcname_and_body() {
        // funcname: Name {'.'Name} [':'Name]  then funcbody
        Token base = tk();
        if (tk().kind != Tok::Name) { failed = true; return; }
        adv();
        bool has_suffix = false, is_method = false;
        while (is_op(".")) { adv(); if (tk().kind == Tok::Name) adv(); else { failed = true; return; } has_suffix = true; }
        if (is_op(":")) { adv(); if (tk().kind == Tok::Name) adv(); else { failed = true; return; } has_suffix = true; is_method = true; }
        if (has_suffix) record_read(base);          // `function M.foo` reads M
        else            record_write(base.text);    // `function Foo` defines global Foo
        funcbody(is_method);
    }

    void local_stmt() {
        adv(); // 'local'
        if (is_kw("function")) {
            adv();
            if (tk().kind != Tok::Name) { failed = true; return; }
            std::string name = tk().text; adv();
            declare(name);                 // visible inside its own body (recursion)
            funcbody(false);
            return;
        }
        // local namelist [attribs] ['=' explist]
        std::vector<std::string> names;
        for (;;) {
            if (tk().kind != Tok::Name) { failed = true; return; }
            names.push_back(tk().text); adv();
            if (is_op("<")) { adv(); if (tk().kind == Tok::Name) adv(); expect_op(">"); } // <const>/<close>
            if (is_op(",")) { adv(); continue; }
            break;
        }
        if (is_op("=")) { adv(); exprlist(); }   // RHS evaluated BEFORE names bind
        for (auto& n : names) declare(n);
    }

    void for_stmt() {
        adv(); // 'for'
        if (tk().kind != Tok::Name) { failed = true; return; }
        std::string first = tk().text; adv();
        if (is_op("=")) {                        // numeric: for i = a,b[,c] do
            adv(); expr(); expect_op(","); expr();
            if (is_op(",")) { adv(); expr(); }
            if (is_kw("do")) adv(); else { failed = true; return; }
            push(); declare(first); block(); pop();
        } else {                                 // generic: for a,b in explist do
            std::vector<std::string> names = {first};
            while (is_op(",")) { adv(); if (tk().kind == Tok::Name) { names.push_back(tk().text); adv(); } else { failed = true; return; } }
            if (is_kw("in")) adv(); else { failed = true; return; }
            exprlist();                          // iterators evaluated in outer scope
            if (is_kw("do")) adv(); else { failed = true; return; }
            push(); for (auto& n : names) declare(n); block(); pop();
        }
        if (is_kw("end")) adv(); else failed = true;
    }

    void if_stmt() {
        adv(); // 'if'
        expr();                                  // condition in outer scope
        if (is_kw("then")) adv(); else { failed = true; return; }
        push(); block(); pop();
        while (is_kw("elseif")) {
            adv(); expr();
            if (is_kw("then")) adv(); else { failed = true; return; }
            push(); block(); pop();
        }
        if (is_kw("else")) { adv(); push(); block(); pop(); }
        if (is_kw("end")) adv(); else failed = true;
    }

    void exprstat() {
        Pending p = suffixed();
        if (is_op("=") || is_op(",")) {
            // assignment: varlist '=' explist. First lvalue is `p`; a bare
            // pending Name is a write target, a suffixed one already read.
            std::vector<std::string> bare_targets;
            if (p.valid) bare_targets.push_back(p.tok.text);
            while (is_op(",")) {
                adv();
                Pending q = suffixed();
                if (q.valid) bare_targets.push_back(q.tok.text);
                if (failed) return;
            }
            expect_op("=");
            exprlist();
            for (auto& n : bare_targets) record_write(n);
        }
        // else: a function-call statement — base already recorded as read.
    }

    void statement() {
        if (is_op(";")) { adv(); return; }
        if (is_op("::")) { adv(); if (tk().kind == Tok::Name) adv(); expect_op("::"); return; }  // label
        if (is_kw("break")) { adv(); return; }
        if (is_kw("goto"))  { adv(); if (tk().kind == Tok::Name) adv(); return; }                 // label ref, not a var
        if (is_kw("do"))    { adv(); push(); block(); pop(); if (is_kw("end")) adv(); else failed = true; return; }
        if (is_kw("while")) { adv(); expr(); if (is_kw("do")) adv(); else { failed = true; return; } push(); block(); pop(); if (is_kw("end")) adv(); else failed = true; return; }
        if (is_kw("repeat")){ adv(); push(); block(); if (is_kw("until")) adv(); else { failed = true; pop(); return; } expr(); pop(); return; } // until sees block locals
        if (is_kw("if"))    { if_stmt(); return; }
        if (is_kw("for"))   { for_stmt(); return; }
        if (is_kw("function")) { adv(); funcname_and_body(); return; }
        if (is_kw("local")) { local_stmt(); return; }
        if (is_kw("return")){ adv(); if (!at_eof() && !is_kw("end") && !is_kw("else") && !is_kw("elseif") && !is_kw("until") && !is_op(";")) exprlist(); if (is_op(";")) adv(); return; }
        exprstat();
    }

    void run() {
        push();          // chunk (main) scope — like an implicit function body
        declare("...");  // vararg placeholder (harmless)
        block();
        pop();
        if (!at_eof()) failed = true;   // trailing garbage → desync, bail
    }
};

} // namespace

GlobalSet extract_known_globals(std::string_view script_cpp_src,
                                std::string_view constants_lua_src) {
    GlobalSet g;

    // Lua 5.4 stdlib baseline the sandbox leaves reachable (base library +
    // library tables). script.cpp additionally rebinds several of these, but
    // listing them here keeps the checker correct even if a binding moves.
    static const char* stdlib[] = {
        "assert","collectgarbage","dofile","error","getmetatable","ipairs",
        "load","loadfile","next","pairs","pcall","print","rawequal","rawget",
        "rawlen","rawset","select","setmetatable","tonumber","tostring","type",
        "xpcall","require","_G","_VERSION",
        "string","table","math","os","io","coroutine","utf8","package","debug"};
    for (const char* s : stdlib) g.insert(s);

    // Engine bindings: every lua["Name"] registration in script.cpp. This is
    // the source of truth — extracting it here means the known set can't drift
    // from the actual C++ API.
    {
        std::string_view s = script_cpp_src;
        const std::string key = "lua[\"";
        size_t p = 0;
        while ((p = s.find(key, p)) != std::string_view::npos) {
            p += key.size();
            size_t e = s.find('"', p);
            if (e == std::string_view::npos) break;
            g.insert(std::string(s.substr(p, e - p)));
            p = e + 1;
        }
    }

    // Usertype globals: `lua.new_usertype<...>("Name"` registers a global
    // (Player, Unit, Item, Destructable, ...) — these are callable/constructor
    // globals, not lua["..."] entries, so scan for them too.
    {
        std::string_view s = script_cpp_src;
        const std::string key = "new_usertype<";
        size_t p = 0;
        while ((p = s.find(key, p)) != std::string_view::npos) {
            // find the ("Name" after the template args
            size_t q = s.find('"', p);
            if (q == std::string_view::npos) break;
            size_t e = s.find('"', q + 1);
            if (e == std::string_view::npos) break;
            g.insert(std::string(s.substr(q + 1, e - (q + 1))));
            p = e + 1;
        }
    }

    // Script constants: top-level `NAME = ...` in constants.lua (EVENT_*,
    // TRIGGER_PRIORITY_*, UNIT_STATUS_*, ...). Match an identifier at line
    // start followed by '='.
    {
        std::string_view s = constants_lua_src;
        size_t p = 0;
        while (p < s.size()) {
            size_t eol = s.find('\n', p);
            std::string_view line = s.substr(p, eol == std::string_view::npos ? std::string_view::npos : eol - p);
            // trim leading spaces
            size_t a = 0; while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
            size_t b = a;
            while (b < line.size() && (std::isalnum((unsigned char)line[b]) || line[b] == '_')) ++b;
            if (b > a) {
                size_t c = b; while (c < line.size() && (line[c] == ' ' || line[c] == '\t')) ++c;
                if (c < line.size() && line[c] == '=' && (c + 1 >= line.size() || line[c + 1] != '=')) {
                    g.insert(std::string(line.substr(a, b - a)));
                }
            }
            if (eol == std::string_view::npos) break;
            p = eol + 1;
        }
    }
    return g;
}

namespace {
// Parse one source: tokenize + resolve. Fills `reads` (global read-sites) and
// `writes` (names assigned at file scope). Returns false on lex/parse desync
// (caller then skips producing diagnostics for that source — Tier 1 owns
// syntax errors, so a desync here means "don't risk false positives").
bool parse_one(std::string_view source, std::string_view chunk_name,
               std::vector<UndefinedGlobal>& reads,
               std::unordered_set<std::string>& writes) {
    Lexer lex; lex.src = source;
    std::vector<Token> toks;
    for (;;) {
        Token t = lex.next();
        toks.push_back(t);
        if (t.kind == Tok::Eof) break;
        if (!lex.ok) return false;
        if (toks.size() > 2'000'000) return false;
    }
    Resolver r;
    r.toks = std::move(toks);
    r.chunk = std::string(chunk_name);
    r.run();
    if (r.failed) return false;
    reads = std::move(r.reads);
    writes = std::move(r.global_writes);
    return true;
}
} // namespace

std::vector<UndefinedGlobal> check_globals(std::string_view source,
                                           std::string_view chunk_name,
                                           const GlobalSet& known) {
    std::vector<UndefinedGlobal> reads;
    std::unordered_set<std::string> writes;
    if (!parse_one(source, chunk_name, reads, writes)) return {};

    std::vector<UndefinedGlobal> out;
    for (auto& rd : reads) {
        if (known.count(rd.name)) continue;
        if (writes.count(rd.name)) continue;
        out.push_back(rd);
    }
    return out;
}

std::vector<UndefinedGlobal> check_globals_project(const std::vector<NamedSource>& scripts,
                                                   const GlobalSet& known) {
    struct Parsed { std::vector<UndefinedGlobal> reads; bool ok; };
    std::vector<Parsed> parsed;
    parsed.reserve(scripts.size());
    std::unordered_set<std::string> all_writes;
    for (const auto& s : scripts) {
        std::vector<UndefinedGlobal> reads;
        std::unordered_set<std::string> writes;
        bool ok = parse_one(s.source, s.name, reads, writes);
        if (ok) all_writes.insert(writes.begin(), writes.end());
        parsed.push_back({std::move(reads), ok});
    }

    std::vector<UndefinedGlobal> out;
    for (auto& p : parsed) {
        if (!p.ok) continue;
        for (auto& rd : p.reads) {
            if (known.count(rd.name)) continue;
            if (all_writes.count(rd.name)) continue;
            out.push_back(rd);
        }
    }
    return out;
}

namespace {

struct LuaType {
    std::unordered_set<std::string> names;

    static LuaType one(std::string name) {
        LuaType type;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        type.names.insert(std::move(name));
        return type;
    }

    static LuaType any() { return one("any"); }

    bool contains(std::string_view name) const {
        return names.contains(std::string(name));
    }
};

LuaType combine(LuaType a, const LuaType& b) {
    a.names.insert(b.names.begin(), b.names.end());
    return a;
}

std::string type_name(const LuaType& type) {
    if (type.names.empty() || type.contains("any")) return "any";
    std::vector<std::string> names(type.names.begin(), type.names.end());
    std::sort(names.begin(), names.end());
    std::string result;
    for (const auto& name : names) {
        if (!result.empty()) result += "|";
        result += name;
    }
    return result;
}

bool is_table_type(std::string_view name) {
    return name == "table" || name.ends_with("[]");
}

bool compatible(const LuaType& actual, const LuaType& expected) {
    if (actual.names.empty() || expected.names.empty() ||
        actual.contains("any") || expected.contains("any")) return true;

    for (const auto& a : actual.names) {
        for (const auto& e : expected.names) {
            if (a == e) return true;
            if (is_table_type(a) && is_table_type(e)) return true;
        }
    }
    return false;
}

struct ApiParam {
    std::string name;
    LuaType     type = LuaType::any();
    bool        optional = false;
};

struct ApiFunction {
    std::string name;
    std::vector<ApiParam> params;
    std::vector<LuaType>  returns;
    bool variadic = false;
};

using ApiMap = std::unordered_map<std::string, ApiFunction>;

void skip_spaces(std::string_view text, size_t& pos) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
}

std::string parse_identifier(std::string_view text, size_t& pos) {
    skip_spaces(text, pos);
    size_t begin = pos;
    while (pos < text.size() &&
           (std::isalnum(static_cast<unsigned char>(text[pos])) ||
            text[pos] == '_' || text[pos] == '.')) ++pos;
    if (pos + 1 < text.size() && text[pos] == '[' && text[pos + 1] == ']') pos += 2;
    if (pos < text.size() && text[pos] == '?') ++pos;
    return std::string(text.substr(begin, pos - begin));
}

LuaType parse_annotation_type(std::string_view text, size_t& pos) {
    LuaType result;
    for (;;) {
        std::string atom = parse_identifier(text, pos);
        if (atom.empty()) break;
        bool nullable = atom.ends_with('?');
        if (nullable) atom.pop_back();
        std::transform(atom.begin(), atom.end(), atom.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        result.names.insert(std::move(atom));
        if (nullable) result.names.insert("nil");
        skip_spaces(text, pos);
        if (pos >= text.size() || text[pos] != '|') break;
        ++pos;
    }
    return result.names.empty() ? LuaType::any() : result;
}

ApiMap parse_api(std::string_view source,
                 std::unordered_set<std::string>* duplicates = nullptr) {
    ApiMap api;
    std::vector<ApiParam> pending_params;
    std::vector<LuaType> pending_returns;

    size_t line_begin = 0;
    while (line_begin <= source.size()) {
        size_t line_end = source.find('\n', line_begin);
        if (line_end == std::string_view::npos) line_end = source.size();
        std::string_view line = source.substr(line_begin, line_end - line_begin);
        size_t first = line.find_first_not_of(" \t\r");
        if (first != std::string_view::npos) line.remove_prefix(first);

        constexpr std::string_view PARAM = "---@param";
        constexpr std::string_view RETURN = "---@return";
        constexpr std::string_view FUNCTION = "function ";

        if (line.starts_with(PARAM)) {
            size_t pos = PARAM.size();
            std::string name = parse_identifier(line, pos);
            bool optional = name.ends_with('?');
            if (optional) name.pop_back();
            LuaType type = name == "..." ? LuaType::any()
                                          : parse_annotation_type(line, pos);
            optional = optional || type.contains("nil");
            pending_params.push_back({std::move(name), std::move(type), optional});
        } else if (line.starts_with(RETURN)) {
            size_t pos = RETURN.size();
            for (;;) {
                LuaType type = parse_annotation_type(line, pos);
                pending_returns.push_back(std::move(type));
                skip_spaces(line, pos);
                if (pos >= line.size() || line[pos] != ',') break;
                ++pos;
            }
        } else if (line.starts_with(FUNCTION)) {
            size_t name_begin = FUNCTION.size();
            size_t open = line.find('(', name_begin);
            size_t close = open == std::string_view::npos
                         ? std::string_view::npos : line.find(')', open + 1);
            if (open != std::string_view::npos && close != std::string_view::npos) {
                std::string name(line.substr(name_begin, open - name_begin));
                while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
                    name.pop_back();

                ApiFunction fn;
                fn.name = name;
                std::string_view args = line.substr(open + 1, close - open - 1);
                size_t arg_pos = 0;
                size_t annotated = 0;
                while (arg_pos < args.size()) {
                    size_t comma = args.find(',', arg_pos);
                    if (comma == std::string_view::npos) comma = args.size();
                    std::string arg(args.substr(arg_pos, comma - arg_pos));
                    size_t a = arg.find_first_not_of(" \t");
                    size_t b = arg.find_last_not_of(" \t");
                    arg = a == std::string::npos ? std::string{}
                                                 : arg.substr(a, b - a + 1);
                    if (!arg.empty()) {
                        if (arg == "...") {
                            fn.variadic = true;
                        } else {
                            auto it = std::find_if(pending_params.begin(), pending_params.end(),
                                [&](const ApiParam& p) { return p.name == arg; });
                            if (it != pending_params.end()) fn.params.push_back(*it);
                            else if (annotated < pending_params.size() &&
                                     pending_params[annotated].name != "...")
                                fn.params.push_back(pending_params[annotated]);
                            else fn.params.push_back({arg, LuaType::any(), false});
                            ++annotated;
                        }
                    }
                    arg_pos = comma + 1;
                }
                fn.returns = pending_returns;
                if (api.contains(name) && duplicates) duplicates->insert(name);
                api[name] = std::move(fn);
            }
            pending_params.clear();
            pending_returns.clear();
        }

        if (line_end == source.size()) break;
        line_begin = line_end + 1;
    }
    return api;
}

struct ExprType {
    std::vector<LuaType> values{LuaType::any()};
    Token location;
    std::string bare_name;

    const LuaType& first() const { return values.front(); }
};

struct TypeAnalyzer {
    std::vector<Token> toks;
    size_t i = 0;
    std::vector<std::unordered_map<std::string, LuaType>> scopes;
    std::unordered_map<std::string, LuaType> globals;
    const ApiMap& api;
    std::string chunk;
    std::vector<ScriptTypeError> errors;
    bool failed = false;

    const Token& tk() const { return toks[i]; }
    const Token& tk2() const { return toks[i + 1 < toks.size() ? i + 1 : toks.size() - 1]; }
    void adv() { if (i + 1 < toks.size()) ++i; }
    bool at_eof() const { return tk().kind == Tok::Eof; }
    bool is_op(const char* s) const { return tk().kind == Tok::Op && tk().text == s; }
    bool is_kw(const char* s) const { return tk().kind == Tok::Keyword && tk().text == s; }
    void expect_op(const char* s) { if (is_op(s)) adv(); else failed = true; }
    void push() { scopes.emplace_back(); }
    void pop() { if (!scopes.empty()) scopes.pop_back(); }

    void declare(const std::string& name, LuaType type = LuaType::any()) {
        if (!scopes.empty()) scopes.back()[name] = std::move(type);
    }

    bool has_local(const std::string& name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            if (it->contains(name)) return true;
        return false;
    }

    LuaType lookup(const std::string& name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        auto global = globals.find(name);
        if (global != globals.end()) return global->second;
        if (api.contains(name)) return LuaType::one("function");
        return LuaType::any();
    }

    void assign(const std::string& name, LuaType type) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) {
                if (found->second.contains("any") || found->second.contains("nil")) {
                    found->second = std::move(type);
                } else {
                    found->second = combine(std::move(found->second), type);
                }
                return;
            }
        }
        globals[name] = std::move(type);
    }

    void type_error(const Token& at, std::string message) {
        errors.push_back({chunk, at.line, at.col, std::move(message)});
    }

    void validate_call(const Token& name, const std::vector<ExprType>& args) {
        if (has_local(name.text)) return;
        auto found = api.find(name.text);
        if (found == api.end()) return;
        const auto& fn = found->second;

        size_t required = 0;
        for (const auto& p : fn.params) if (!p.optional) ++required;
        if (args.size() < required) {
            type_error(name, name.text + " expects at least " +
                       std::to_string(required) + " argument" +
                       (required == 1 ? "" : "s") + ", got " +
                       std::to_string(args.size()));
        } else if (!fn.variadic && args.size() > fn.params.size()) {
            type_error(name, name.text + " expects at most " +
                       std::to_string(fn.params.size()) + " argument" +
                       (fn.params.size() == 1 ? "" : "s") + ", got " +
                       std::to_string(args.size()));
        }

        size_t count = std::min(args.size(), fn.params.size());
        for (size_t n = 0; n < count; ++n) {
            if (compatible(args[n].first(), fn.params[n].type)) continue;
            type_error(args[n].location,
                       name.text + " argument " + std::to_string(n + 1) +
                       " ('" + fn.params[n].name + "') expects " +
                       type_name(fn.params[n].type) + ", got " +
                       type_name(args[n].first()));
        }
    }

    std::vector<ExprType> args() {
        std::vector<ExprType> result;
        if (is_op("(")) {
            adv();
            if (!is_op(")")) result = exprlist_nodes();
            expect_op(")");
        } else if (tk().kind == Tok::String) {
            ExprType value; value.values = {LuaType::one("string")}; value.location = tk();
            result.push_back(std::move(value)); adv();
        } else if (is_op("{")) {
            result.push_back(table());
        } else failed = true;
        return result;
    }

    ExprType table() {
        ExprType result; result.values = {LuaType::one("table")}; result.location = tk();
        expect_op("{");
        while (!is_op("}") && !at_eof() && !failed) {
            if (is_op("[")) {
                adv(); expr(); expect_op("]"); expect_op("="); expr();
            } else if (tk().kind == Tok::Name && tk2().kind == Tok::Op && tk2().text == "=") {
                adv(); adv(); expr();
            } else expr();
            if (is_op(",") || is_op(";")) adv(); else break;
        }
        expect_op("}");
        return result;
    }

    ExprType primary() {
        if (is_op("(")) {
            Token at = tk(); adv(); ExprType value = expr(); expect_op(")");
            value.location = at; value.bare_name.clear(); return value;
        }
        if (tk().kind == Tok::Name) {
            Token name = tk(); adv();
            ExprType value; value.values = {lookup(name.text)};
            value.location = name; value.bare_name = name.text; return value;
        }
        failed = true;
        return {};
    }

    ExprType suffixed() {
        ExprType value = primary();
        for (;;) {
            if (is_op(".")) {
                adv();
                if (tk().kind == Tok::Name) adv(); else { failed = true; return value; }
                value.values = {LuaType::any()}; value.bare_name.clear();
            } else if (is_op("[")) {
                adv(); expr(); expect_op("]");
                value.values = {LuaType::any()}; value.bare_name.clear();
            } else if (is_op(":")) {
                adv();
                if (tk().kind == Tok::Name) adv(); else { failed = true; return value; }
                args(); value.values = {LuaType::any()}; value.bare_name.clear();
            } else if (is_op("(") || is_op("{") || tk().kind == Tok::String) {
                std::vector<ExprType> call_args = args();
                if (!value.bare_name.empty()) {
                    Token name = value.location;
                    validate_call(name, call_args);
                    auto found = api.find(value.bare_name);
                    value.values = found != api.end() && !found->second.returns.empty()
                                 ? found->second.returns
                                 : std::vector<LuaType>{LuaType::any()};
                } else value.values = {LuaType::any()};
                value.bare_name.clear();
            } else break;
            if (failed) return value;
        }
        return value;
    }

    void funcbody(bool method) {
        push();
        if (method) declare("self");
        expect_op("(");
        if (!is_op(")")) {
            for (;;) {
                if (is_op("...")) { adv(); break; }
                if (tk().kind == Tok::Name) { declare(tk().text); adv(); }
                else { failed = true; break; }
                if (is_op(",")) { adv(); continue; }
                break;
            }
        }
        expect_op(")"); block();
        if (is_kw("end")) adv(); else failed = true;
        pop();
    }

    ExprType simple() {
        Token at = tk();
        if (tk().kind == Tok::Number) {
            adv(); ExprType v; v.values = {LuaType::one("number")}; v.location = at; return v;
        }
        if (tk().kind == Tok::String) {
            adv(); ExprType v; v.values = {LuaType::one("string")}; v.location = at; return v;
        }
        if (is_kw("nil")) {
            adv(); ExprType v; v.values = {LuaType::one("nil")}; v.location = at; return v;
        }
        if (is_kw("true") || is_kw("false")) {
            adv(); ExprType v; v.values = {LuaType::one("boolean")}; v.location = at; return v;
        }
        if (is_op("...")) {
            adv(); ExprType v; v.values = {LuaType::any()}; v.location = at; return v;
        }
        if (is_op("{")) return table();
        if (is_kw("function")) {
            adv(); funcbody(false);
            ExprType v; v.values = {LuaType::one("function")}; v.location = at; return v;
        }
        if (is_op("(") || tk().kind == Tok::Name) return suffixed();
        failed = true;
        return {};
    }

    bool is_unop() const { return is_kw("not") || is_op("-") || is_op("#") || is_op("~"); }
    bool is_binop() const {
        if (is_kw("and") || is_kw("or")) return true;
        if (tk().kind != Tok::Op) return false;
        static const std::unordered_set<std::string> ops = {
            "+","-","*","/","//","%","^","..","<","<=",">",">=","==","~=",
            "&","|","~","<<",">>"};
        return ops.contains(tk().text);
    }

    int binop_precedence() const {
        if (is_kw("or")) return 1;
        if (is_kw("and")) return 2;
        if (is_op("<") || is_op("<=") || is_op(">") || is_op(">=") ||
            is_op("==") || is_op("~=")) return 3;
        if (is_op("|")) return 4;
        if (is_op("~")) return 5;
        if (is_op("&")) return 6;
        if (is_op("<<") || is_op(">>")) return 7;
        if (is_op("..")) return 8;
        if (is_op("+") || is_op("-")) return 9;
        if (is_op("*") || is_op("/") || is_op("//") || is_op("%")) return 10;
        if (is_op("^")) return 12;
        return 0;
    }

    ExprType expr(int min_precedence = 1) {
        std::vector<Token> unary;
        while (is_unop() && !failed) { unary.push_back(tk()); adv(); }
        ExprType left = simple();
        for (auto it = unary.rbegin(); it != unary.rend(); ++it) {
            if (it->text == "not") left.values = {LuaType::one("boolean")};
            else if (it->text == "#") left.values = {LuaType::one("number")};
            else {
                LuaType number = LuaType::one("number");
                if (!compatible(left.first(), number))
                    type_error(*it, "operator '" + it->text + "' expects number, got " +
                                    type_name(left.first()));
                left.values = {std::move(number)};
            }
            left.bare_name.clear();
        }
        while (is_binop() && !failed) {
            int precedence = binop_precedence();
            if (precedence < min_precedence) break;
            Token op = tk(); adv();
            bool right_associative = op.text == "^" || op.text == "..";
            ExprType right = expr(precedence + (right_associative ? 0 : 1));
            if (op.text == "and" || op.text == "or")
                left.values = {combine(left.first(), right.first())};
            else if (op.text == "==" || op.text == "~=" || op.text == "<" ||
                     op.text == "<=" || op.text == ">" || op.text == ">=")
                left.values = {LuaType::one("boolean")};
            else if (op.text == "..")
                left.values = {LuaType::one("string")};
            else {
                LuaType number = LuaType::one("number");
                if (!compatible(left.first(), number))
                    type_error(op, "operator '" + op.text + "' expects number, got " +
                                   type_name(left.first()));
                if (!compatible(right.first(), number))
                    type_error(op, "operator '" + op.text + "' expects number, got " +
                                   type_name(right.first()));
                left.values = {std::move(number)};
            }
            left.bare_name.clear();
        }
        return left;
    }

    std::vector<ExprType> exprlist_nodes() {
        std::vector<ExprType> result;
        for (;;) {
            ExprType value = expr();
            if (is_op(",")) {
                if (value.values.size() > 1) value.values.resize(1);
                result.push_back(std::move(value));
                adv();
                continue;
            }
            Token location = value.location;
            for (auto& type : value.values) {
                ExprType one; one.values = {type}; one.location = location;
                result.push_back(std::move(one));
            }
            break;
        }
        return result;
    }

    void block() {
        while (!failed && !at_eof() && !is_kw("end") && !is_kw("else") &&
               !is_kw("elseif") && !is_kw("until")) statement();
    }

    void function_statement() {
        Token base = tk();
        if (tk().kind != Tok::Name) { failed = true; return; }
        adv();
        bool suffixed_name = false, method = false;
        while (is_op(".")) {
            adv(); if (tk().kind == Tok::Name) adv(); else { failed = true; return; }
            suffixed_name = true;
        }
        if (is_op(":")) {
            adv(); if (tk().kind == Tok::Name) adv(); else { failed = true; return; }
            suffixed_name = true; method = true;
        }
        if (!suffixed_name) globals[base.text] = LuaType::one("function");
        funcbody(method);
    }

    void local_statement() {
        adv();
        if (is_kw("function")) {
            adv();
            if (tk().kind != Tok::Name) { failed = true; return; }
            std::string name = tk().text; adv();
            declare(name, LuaType::one("function")); funcbody(false); return;
        }
        std::vector<std::string> names;
        for (;;) {
            if (tk().kind != Tok::Name) { failed = true; return; }
            names.push_back(tk().text); adv();
            if (is_op("<")) { adv(); if (tk().kind == Tok::Name) adv(); expect_op(">"); }
            if (is_op(",")) { adv(); continue; }
            break;
        }
        std::vector<ExprType> values;
        if (is_op("=")) { adv(); values = exprlist_nodes(); }
        for (size_t n = 0; n < names.size(); ++n) {
            LuaType type = n < values.size() ? values[n].first() : LuaType::any();
            if (type.contains("nil")) type = LuaType::any();
            declare(names[n], std::move(type));
        }
    }

    void for_statement() {
        adv();
        if (tk().kind != Tok::Name) { failed = true; return; }
        std::string first = tk().text; adv();
        if (is_op("=")) {
            adv(); expr(); expect_op(","); expr(); if (is_op(",")) { adv(); expr(); }
            if (is_kw("do")) adv(); else { failed = true; return; }
            push(); declare(first, LuaType::one("number")); block(); pop();
        } else {
            std::vector<std::string> names{first};
            while (is_op(",")) {
                adv(); if (tk().kind == Tok::Name) { names.push_back(tk().text); adv(); }
                else { failed = true; return; }
            }
            if (is_kw("in")) adv(); else { failed = true; return; }
            exprlist_nodes();
            if (is_kw("do")) adv(); else { failed = true; return; }
            push(); for (const auto& name : names) declare(name); block(); pop();
        }
        if (is_kw("end")) adv(); else failed = true;
    }

    void if_statement() {
        adv(); expr();
        if (is_kw("then")) adv(); else { failed = true; return; }
        push(); block(); pop();
        while (is_kw("elseif")) {
            adv(); expr();
            if (is_kw("then")) adv(); else { failed = true; return; }
            push(); block(); pop();
        }
        if (is_kw("else")) { adv(); push(); block(); pop(); }
        if (is_kw("end")) adv(); else failed = true;
    }

    void expression_statement() {
        ExprType first = suffixed();
        if (is_op("=") || is_op(",")) {
            std::vector<std::string> targets;
            if (!first.bare_name.empty()) targets.push_back(first.bare_name);
            while (is_op(",")) {
                adv(); ExprType target = suffixed();
                if (!target.bare_name.empty()) targets.push_back(target.bare_name);
            }
            expect_op("=");
            auto values = exprlist_nodes();
            for (size_t n = 0; n < targets.size(); ++n)
                assign(targets[n], n < values.size() ? values[n].first() : LuaType::one("nil"));
        }
    }

    void statement() {
        if (is_op(";")) { adv(); return; }
        if (is_op("::")) { adv(); if (tk().kind == Tok::Name) adv(); expect_op("::"); return; }
        if (is_kw("break")) { adv(); return; }
        if (is_kw("goto")) { adv(); if (tk().kind == Tok::Name) adv(); return; }
        if (is_kw("do")) { adv(); push(); block(); pop(); if (is_kw("end")) adv(); else failed = true; return; }
        if (is_kw("while")) { adv(); expr(); if (is_kw("do")) adv(); else { failed = true; return; } push(); block(); pop(); if (is_kw("end")) adv(); else failed = true; return; }
        if (is_kw("repeat")) { adv(); push(); block(); if (is_kw("until")) adv(); else { failed = true; pop(); return; } expr(); pop(); return; }
        if (is_kw("if")) { if_statement(); return; }
        if (is_kw("for")) { for_statement(); return; }
        if (is_kw("function")) { adv(); function_statement(); return; }
        if (is_kw("local")) { local_statement(); return; }
        if (is_kw("return")) {
            adv();
            if (!at_eof() && !is_kw("end") && !is_kw("else") && !is_kw("elseif") &&
                !is_kw("until") && !is_op(";")) exprlist_nodes();
            if (is_op(";")) adv();
            return;
        }
        expression_statement();
    }

    void run() {
        push(); declare("..."); block(); pop();
        if (!at_eof()) failed = true;
    }
};

bool tokenize(std::string_view source, std::vector<Token>& tokens) {
    Lexer lexer; lexer.src = source;
    for (;;) {
        Token token = lexer.next();
        tokens.push_back(token);
        if (token.kind == Tok::Eof) return lexer.ok;
        if (!lexer.ok || tokens.size() > 2'000'000) return false;
    }
}

std::unordered_set<std::string> extract_cpp_api_names(std::string_view source) {
    std::unordered_set<std::string> names;
    constexpr std::string_view KEY = "lua[\"";
    size_t pos = 0;
    while ((pos = source.find(KEY, pos)) != std::string_view::npos) {
        pos += KEY.size();
        size_t end = source.find('"', pos);
        if (end == std::string_view::npos) break;
        std::string name(source.substr(pos, end - pos));
        if (!name.empty() && std::isupper(static_cast<unsigned char>(name[0])) &&
            !name.starts_with("TRIGGER_PRIORITY_")) names.insert(std::move(name));
        pos = end + 1;
    }
    names.insert("Player");
    return names;
}

} // namespace

std::vector<ScriptTypeError> check_types_project(
        const std::vector<NamedSource>& scripts,
        std::string_view api_declarations) {
    ApiMap api = parse_api(api_declarations);
    std::unordered_map<std::string, LuaType> globals;

    for (const auto& script : scripts) {
        std::vector<UndefinedGlobal> reads;
        std::unordered_set<std::string> writes;
        if (!parse_one(script.source, script.name, reads, writes)) continue;
        for (const auto& name : writes) globals.try_emplace(name, LuaType::any());
    }

    std::vector<ScriptTypeError> result;
    for (const auto& script : scripts) {
        std::vector<Token> tokens;
        if (!tokenize(script.source, tokens)) continue;
        TypeAnalyzer analyzer{std::move(tokens), 0, {}, globals, api, script.name};
        analyzer.run();
        if (analyzer.failed) {
            result.push_back({script.name, analyzer.tk().line, analyzer.tk().col,
                              "type checker could not analyze this expression"});
            continue;
        }
        result.insert(result.end(), analyzer.errors.begin(), analyzer.errors.end());
        for (const auto& [name, type] : analyzer.globals) {
            if (!type.contains("any")) globals[name] = type;
        }
    }
    return result;
}

std::vector<ApiConsistencyError> check_api_consistency(
        std::string_view script_cpp_src,
        std::string_view api_declarations) {
    std::unordered_set<std::string> duplicates;
    ApiMap api = parse_api(api_declarations, &duplicates);
    auto cpp_names = extract_cpp_api_names(script_cpp_src);

    std::vector<ApiConsistencyError> result;
    for (const auto& name : duplicates)
        result.push_back({name, "duplicate declaration in api.lua"});
    for (const auto& name : cpp_names) {
        if (!api.contains(name))
            result.push_back({name, "bound by ScriptEngine but missing from api.lua"});
    }
    for (const auto& [name, fn] : api) {
        if (!cpp_names.contains(name))
            result.push_back({name, "declared in api.lua but not bound by ScriptEngine"});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.name < b.name;
    });
    return result;
}

} // namespace uldum::script
