#include "script/script_check.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <cctype>
#include <cstring>

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
    // Pass 1: parse every file, accumulate reads and the UNION of all file-scope
    // global writes (the shared global env the engine builds by loading them all).
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

    // Pass 2: a read is undefined only if it's neither a known global nor
    // written anywhere across the whole project.
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


} // namespace uldum::script
