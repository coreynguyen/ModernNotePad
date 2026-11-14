#include "mxs_internal.h"
#include <cstring>
#include <unordered_map>

namespace mxs {

// ================================================================= lexer ====
static const std::unordered_map<std::string, Tok> g_keywords = {
    {"and",Tok::KwAnd},{"or",Tok::KwOr},{"not",Tok::KwNot},
    {"if",Tok::KwIf},{"then",Tok::KwThen},{"else",Tok::KwElse},
    {"do",Tok::KwDo},{"while",Tok::KwWhile},{"for",Tok::KwFor},{"in",Tok::KwIn},
    {"to",Tok::KwTo},   // [mxs25 F] 'by' is contextual (Parser::for_expr), usable as a variable name
    {"of",Tok::KwOf},{"where",Tok::KwWhere},
    {"collect",Tok::KwCollect},{"case",Tok::KwCase},
    {"fn",Tok::KwFn},{"function",Tok::KwFn},
    {"local",Tok::KwLocal},{"global",Tok::KwGlobal},
    {"return",Tok::KwReturn},{"exit",Tok::KwExit},{"continue",Tok::KwContinue},
    {"struct",Tok::KwStruct},{"try",Tok::KwTry},{"catch",Tok::KwCatch},
    {"throw",Tok::KwThrow},{"as",Tok::KwAs},{"with",Tok::KwWith},
    {"rollout",Tok::KwRollout},{"utility",Tok::KwUtility},{"parameters",Tok::KwParameters},
    {"rcmenu",Tok::KwRcMenu},{"at",Tok::KwAt},{"about",Tok::KwAbout},
    {"attributes",Tok::KwAttributes},
    {"default",Tok::KwDefault},
    {"true",Tok::KwTrue},{"false",Tok::KwFalse},{"on",Tok::KwOn},{"off",Tok::KwOff},
    {"undefined",Tok::KwUndefined},{"ok",Tok::KwOk},{"unsupplied",Tok::KwUnsupplied},
};

Token Lexer::next() {
    Token tk; tk.line = line;
    bool nl = false, sp = false;
    for (;;) {
        while (p < end) {
            char c = *p;
            if (c == '\n') { ++line; nl = true; ++p; }
            else if (c == '\r' || c == ' ' || c == '\t') { sp = true; ++p; }
            // Line continuation. Real scripts put whitespace and even a
            // trailing comment after the backslash:
            //     append rgm.chunk.info (chunk_info <backslash>
            //         index:(1) <backslash>   -- array index
            // so skip blanks, then an optional '--' comment, then the newline.
            else if (c == '\\') {
                const char* q = p + 1;
                while (q < end && (*q == ' ' || *q == '\t')) ++q;
                if (q + 1 < end && q[0] == '-' && q[1] == '-')
                    while (q < end && *q != '\n') ++q;          // trailing comment
                // Consume PAST the newline: leaving p on it would set
                // nl_before on the next token, which stops juxtaposition
                // argument gathering -- exactly what a continuation exists to
                // prevent ("f a b \\\n   kw:v").
                if (q < end && (*q == '\r' || *q == '\n')) {
                    if (*q == '\r' && q + 1 < end && q[1] == '\n') ++q;
                    p = q + 1; ++line;
                }
                else { ++p; }   // stray backslash: skip it rather than dying
            }
            else break;
        }
        if (p + 1 < end && p[0] == '-' && p[1] == '-') {           // -- comment
            while (p < end && *p != '\n') ++p;
            continue;
        }
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {           // block comment
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') { ++line; nl = true; } ++p; }
            if (p + 1 >= end) err("unterminated block comment");
            p += 2; sp = true;
            continue;
        }
        break;
    }
    tk.nl_before = nl; tk.sp_before = sp || nl; tk.line = line;
    tk.off = tk.end_off = (size_t)(p - base);
    if (p >= end) { tk.t = Tok::End; tk.sp_after = true; return tk; }

    char c = *p;
    auto one = [&](Tok t) { ++p; tk.t = t; };

    if (isdigit((unsigned char)c) || (c == '.' && p + 1 < end && isdigit((unsigned char)p[1]))) {
        // number: int, hex, float, exponent; optional trailing L/l (int64) ignored
        const char* s = p;
        bool isfloat = false;
        if (c == '0' && p + 1 < end && (p[1] == 'x' || p[1] == 'X')) {
            p += 2; while (p < end && isxdigit((unsigned char)*p)) ++p;
            tk.t = Tok::Number; tk.v = Value::I(strtoll(s, nullptr, 16));
        } else {
            while (p < end && isdigit((unsigned char)*p)) ++p;
            if (p < end && *p == '.' && !(p + 1 < end && p[1] == '.')) {   // not '..'
                isfloat = true; ++p; while (p < end && isdigit((unsigned char)*p)) ++p;
            }
            if (p < end && (*p == 'e' || *p == 'E')) {
                const char* save = p; ++p;
                if (p < end && (*p == '+' || *p == '-')) ++p;
                if (p < end && isdigit((unsigned char)*p)) { isfloat = true; while (p < end && isdigit((unsigned char)*p)) ++p; }
                else p = save;
            }
            tk.t = Tok::Number;
            if (isfloat) tk.v = Value::F(strtod(s, nullptr));
            else         tk.v = Value::I(strtoll(s, nullptr, 10));
            // Time literals: 10f (frames), 2.5s (seconds), 160t (ticks), 1m (minutes).
            // SPEC-GAP: there is no Time value type; a time literal reads as a Float in
            // frames, at 30 fps / 4800 ticks per second (MAX defaults).
            // Composite forms (1m30s, 2s15f, 3f40t) accumulate. A suffix must not
            // run into an identifier ('2feet' is not a time).
            {
                auto suffixAt = [&](const char* q) {
                    return q < end && (*q == 'f' || *q == 's' || *q == 't' || *q == 'm') &&
                           !(q + 1 < end && (isalpha((unsigned char)q[1]) || q[1] == '_'));
                };
                if (suffixAt(p)) {
                    double total = 0, part = tk.v.num();
                    for (;;) {
                        const char u = *p++;
                        total += u == 's' ? part * 30.0 : u == 't' ? part / 160.0 : u == 'm' ? part * 1800.0 : part;
                        // another <number><suffix> glued on?
                        const char* q = p;
                        while (q < end && (isdigit((unsigned char)*q) || *q == '.')) ++q;
                        if (q == p || !suffixAt(q)) break;
                        part = strtod(p, nullptr);
                        p = q;
                    }
                    if (p < end && isdigit((unsigned char)*p)) err("bad time literal");
                    tk.v = Value::T(total);
                }
            }
            if (p < end && (*p == 'L' || *p == 'l') && !isfloat) ++p;      // Integer64 literal suffix
            if (p < end && (*p == 'd' || *p == 'D') && isfloat) ++p;       // double suffix (accept)
        }
    }
    else if (c == '"' || (c == '@' && p + 1 < end && p[1] == '"')) {
        bool verbatim = (c == '@');
        p += verbatim ? 2 : 1;
        std::string out;
        while (p < end && *p != '"') {
            if (!verbatim && *p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case 'n': out += '\n'; break;  case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;  case '"': out += '"';  break;
                    case '\\': out += '\\'; break; case '%': out += '%';  break;
                    case '*': out += '*';  break;  case '?': out += '?';  break;
                    case 'a': out += '\a'; break;  case 'b': out += '\b'; break;
                    case 'x': { ++p; int v2 = 0, n = 0; while (p < end && isxdigit((unsigned char)*p) && n < 2) { v2 = v2*16 + (isdigit((unsigned char)*p) ? *p-'0' : (tolower(*p)-'a'+10)); ++p; ++n; } out += char(v2); continue; }
                    default: out += *p; break;
                }
                ++p;
            } else { if (*p == '\n') ++line; out += *p++; }
        }
        if (p >= end) err("unterminated string");
        ++p;
        tk.t = Tok::String; tk.v = Value::S(std::move(out));
    }
    else if (c == '#') {
        ++p;
        // The reference manual prints arrays as '# (1, 2)'; accept the space.
        { const char* q = p; while (q < end && (*q == ' ' || *q == '\t')) ++q;
          if (q > p && q < end && *q == '(') p = q; }
        if (p < end && *p == '(') { ++p; tk.t = Tok::ArrayOpen; }
        else if (p < end && *p == '{') { ++p; tk.t = Tok::BitArrayOpen; }
        else if (p < end && *p == '\'') {                                  // #'quoted name'
            ++p; std::string out;
            while (p < end && *p != '\'') out += *p++;
            if (p >= end) err("unterminated quoted name");
            ++p; tk.t = Tok::NameLit; tk.v = Value::N(out);
        }
        else if (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
            std::string out;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_')) out += *p++;
            tk.t = Tok::NameLit; tk.v = Value::N(out);
        }
        else err("bad # literal");
    }
    else if (isalpha((unsigned char)c) || c == '_') {
        std::string out;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_')) out += *p++;
        auto it = g_keywords.find(lower(out));
        if (it != g_keywords.end()) { tk.t = it->second; tk.text = std::move(out); }
        else { tk.t = Tok::Ident; tk.text = std::move(out); }
    }
    else if (c == '$') {
        // Pathname literal (2025 "Pathname Literals"): $name, $'quoted name', wildcards * ?,
        // '\\' escapes the next character, '/' child level, '...' any-depth level, and quoted
        // segments in any combination ($'Bip001 L Hand'/*). The raw spelling, quotes and
        // escapes included, goes to Engine::scene_path_hook, which does the matching.
        // A bare '$' (current selection) leaves text empty; '.' (not '...') ends the path.
        ++p;
        std::string raw;
        while (p < end) {
            char d = *p;
            if (isalnum((unsigned char)d) || d == '_' || d == '*' || d == '?' || d == '/') { raw += d; ++p; }
            else if (d == '\\' && p + 1 < end && p[1] != '\n' && p[1] != '\r') { raw += d; raw += p[1]; p += 2; }
            else if (d == '\'') {
                raw += *p++;
                while (p < end && *p != '\'' && *p != '\n') {
                    if (*p == '\\' && p + 1 < end) raw += *p++;
                    raw += *p++;
                }
                if (p >= end || *p != '\'') err("unterminated quoted pathname");
                raw += *p++;
            }
            else if (d == '.' && p + 2 < end && p[1] == '.' && p[2] == '.') { raw += "..."; p += 3; }
            else break;
        }
        tk.t = Tok::Dollar; tk.text = std::move(raw);
    }
    else switch (c) {
        case '(': one(Tok::LParen); break;   case ')': one(Tok::RParen); break;
        case '[': one(Tok::LBracket); break; case ']': one(Tok::RBracket); break;
        case '}': one(Tok::RBrace); break;
        case ',': one(Tok::Comma); break;    case ';': one(Tok::Semi); break;
        case '^': one(Tok::Caret); break;    case '&': one(Tok::Amp); break;
        case '?': one(Tok::Query); break;    case '$': one(Tok::Dollar); break;
        case ':': one(Tok::Colon); break;
        case '.':
            if (p + 1 < end && p[1] == '.') { p += 2; tk.t = Tok::DotDot; }
            else one(Tok::Dot);
            break;
        case '+': if (p + 1 < end && p[1] == '=') { p += 2; tk.t = Tok::PlusEq; } else one(Tok::Plus); break;
        case '-': if (p + 1 < end && p[1] == '=') { p += 2; tk.t = Tok::MinusEq; } else one(Tok::Minus); break;
        case '*': if (p + 1 < end && p[1] == '=') { p += 2; tk.t = Tok::StarEq; } else one(Tok::Star); break;
        case '/': if (p + 1 < end && p[1] == '=') { p += 2; tk.t = Tok::SlashEq; } else one(Tok::Slash); break;
        case '=':
            if (p + 1 < end && p[1] == '=') { p += 2; tk.t = Tok::Eq; }
            else one(Tok::Assign);
            break;
        case '!':
            if (p + 1 < end && p[1] == '=') { p += 2; tk.t = Tok::Ne; }
            else err("unexpected '!'");
            break;
        case '<':
            if (p + 1 < end && p[1] == '=') { p += 2; tk.t = Tok::Le; }
            else one(Tok::Lt);
            break;
        case '>':
            if (p + 1 < end && p[1] == '=') { p += 2; tk.t = Tok::Ge; }
            else one(Tok::Gt);
            break;
        default: err(std::string("unexpected character '") + c + "'");
    }
    tk.sp_after = (p >= end) || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n';
    tk.end_off = (size_t)(p - base);
    return tk;
}

// ================================================================ parser ====
Parser::Parser(const std::string& src_in, std::string name) : src_name(std::move(name)) {
    // Skip a UTF-8 BOM. Scripts saved from Notepad or Visual Studio routinely
    // carry one, and it would otherwise lex as an unexpected character on the
    // very first line -- which is exactly how vpt_imp.ms / vmp_imp.ms failed.
    const bool bom = src_in.size() >= 3 &&
        (unsigned char)src_in[0] == 0xEF && (unsigned char)src_in[1] == 0xBB &&
        (unsigned char)src_in[2] == 0xBF;
    source = bom ? src_in.substr(3) : src_in;
    Lexer lx(source, src_name);
    for (;;) { Token t = lx.next(); bool endt = (t.t == Tok::End); toks.push_back(std::move(t)); if (endt) break; }
}

Node::~Node() {   // [auditA]
    if (kids.empty()) return;
    std::vector<std::unique_ptr<Node>> work;
    for (auto& k : kids) if (k) work.push_back(std::move(k));
    while (!work.empty()) {
        std::unique_ptr<Node> n = std::move(work.back());
        work.pop_back();
        for (auto& k : n->kids) if (k) work.push_back(std::move(k));
        n->kids.clear();
    }   // each n is destroyed here with no children left
}

NodeP Parser::mk(NK k, int line) { auto n = std::make_unique<Node>(); n->k = k; n->line = line; return n; }

void Parser::expect(Tok t, const char* what) {
    if (!accept(t)) err(std::string("expected ") + what);
}

void Parser::skip_separators() { while (accept(Tok::Semi)) {} }

NodeP Parser::parse_program() {

    auto blk = mk(NK::Block, 1);
    skip_separators();
    while (!at(Tok::End)) {
        blk->kids.push_back(expr());
        skip_separators();
    }
    return blk;
}

// which tokens can begin an operand (for juxtaposition-call argument gathering)
bool Parser::starts_operand(const Token& tk) const {
    switch (tk.t) {
        case Tok::Ident:
            return !(stop_by && lower(tk.text) == "by");   // [mxs25 F] 'for ... to <e> by': ends the 'to' expression
        case Tok::Number: case Tok::String: case Tok::NameLit:
        case Tok::LParen: case Tok::ArrayOpen: case Tok::BitArrayOpen:
        // '[' can begin a point-literal argument (print [1,2,3]). Indexing is
        // handled in postfix_expr and requires no space, so there is no clash.
        case Tok::LBracket:
        case Tok::Query:
        // '$' can begin an argument: 'physiqueOps.getVertexBone $ v'.
        case Tok::Dollar:
        case Tok::KwTrue: case Tok::KwFalse: case Tok::KwOn: case Tok::KwOff:
        case Tok::KwUndefined: case Tok::KwOk: case Tok::KwUnsupplied:
        case Tok::Amp:
            return true;
        default: return false;
    }
}

// '-' beginning a new negated argument: space before, none after (Parser.h: space_before/space_after)
bool Parser::arg_minus() const {
    return at(Tok::Minus) && cur().sp_before && !cur().sp_after && starts_operand(peek());
}

// ------------------------------------------------------------ expression ----
// ---- rollout / UI ---------------------------------------------------------
// Grammar (only the subset real importers use):
//     rollout <name> "title" [width:n height:n] ( <items> )
//     <item> ::= <controlType> <name> ["caption"] <param:value>...
//              | on <ctlName> <event> [do] <expr>
//              | local/global decls, fn defs, ordinary statements
//
// Layout follows the documented model: controls flow top-to-bottom in
// declaration order; 'across:N' puts the next N on one row; 'align:' places
// them within the row; 'offset:[dx,dy]' nudges from the auto position; and
// 'pos:[x,y]' overrides with an absolute position. Realising these as native
// Win32 controls happens in the host (see docs/MAXSCRIPT_ROLLOUTS.md); the
// parser only needs to capture the declarations faithfully.
bool Parser::is_ui_control(const std::string& lw) const {
    static const char* k[] = {
        "button","checkbox","checkbutton","radiobuttons","spinner","slider",
        "edittext","combobox","dropdownlist","listbox","multilistbox","label",
        "hyperlink","colorpicker","mapbutton","materialbutton","pickbutton",
        "progressbar","timer","bitmap","angle","groupbox","imgtag","curvecontrol",
        "activexcontrol","dotnetcontrol","subrollout","link","editText",
        // rcmenu item types
        "menuitem","submenu","separator",
        nullptr };
    for (const char* const* s = k; *s; ++s) if (lw == *s) return true;
    return false;
}

NodeP Parser::rollout_expr() {
    const int ln = cur().line;
    // A 'parameters' block declares plugin parameters, not controls:
    //     parameters main rollout:params (
    //         unk14 type:#integer default:1 ui:spUnk14
    //     )
    // Every line is '<name> <kw:value>...', so entries are captured as items
    // rather than executed as statements (which produced
    // "Call to undefined function: unk14").
    const bool isParams = at(Tok::KwParameters);
    const bool isAttrs  = at(Tok::KwAttributes);
    const size_t srcStart = cur().off;                 // [mxs25 F] attributes: keep source (custAttributes.getDefSource)
    ++pos;                                             // 'rollout' / 'utility'
    auto n = mk(NK::Rollout, ln);
    n->flag = isParams;
    if (cur().text.empty()) err("expected rollout name");
    n->name = lower(cur().text); ++pos;
    if (at(Tok::String)) { n->str = *cur().v.s; ++pos; }   // title
    // rollout-level params: width: height: etc.
    while (!at(Tok::LParen) && !at(Tok::End) && !cur().nl_before) {
        if (!cur().text.empty() && peek().t == Tok::Colon) {
            n->kw_names.push_back(lower(cur().text)); pos += 2;
            n->kids.push_back(operand());
        } else ++pos;
    }
    expect(Tok::LParen, "'(' opening rollout body");

    // NOTE the group_depth term: a group's closing ')' must not be mistaken
    // for the rollout's own, or everything after it parses as top level.
    while ((!at(Tok::RParen) || n->group_depth > 0) && !at(Tok::End)) {
        if (accept(Tok::Semi)) continue;                     // 'rollout r (a; b)'
        // on <ctl> <event> [do] <expr>
        if (at(Tok::KwOn)) {
            const int hl = cur().line; ++pos;
            auto h = mk(NK::OnHandler, hl);
            if (cur().text.empty()) err("expected control name after 'on'");
            h->name = lower(cur().text); ++pos;
            if (cur().text.empty()) err("expected event name in 'on' handler");
            h->str = lower(cur().text); ++pos;
            // some events take arguments: on ctl entered txt do ...
            while (!at(Tok::KwDo) && !at(Tok::End) && !cur().nl_before) {
                h->fn_params.push_back(lower(cur().text)); ++pos;
            }
            accept(Tok::KwDo);
            h->kids.push_back(expr());
            n->kids.push_back(std::move(h));
            n->on_count++;
            continue;
        }
        // group "Caption" ( <items> ) -- a visual grouping box that NESTS
        // controls. Without this the whole group parsed as an ordinary
        // statement and its controls became bogus function calls.
        if (!cur().text.empty() && lower(cur().text) == "group" &&
            (peek().t == Tok::String || peek().t == Tok::LParen)) {
            const int gl = cur().line; ++pos;
            auto grp = mk(NK::UiItem, gl);
            grp->str = "groupbox";
            grp->name = "group" + std::to_string(n->item_count + 1);
            if (at(Tok::String)) { grp->lit = cur().v; ++pos; }
            n->kids.push_back(std::move(grp));
            n->item_count++;
            // Flatten the group's contents into the rollout: they are ordinary
            // controls that simply render inside the box.
            expect(Tok::LParen, "'(' opening group body");
            n->group_depth++;
            continue;
        }
        if (at(Tok::RParen) && n->group_depth > 0) {
            // Emit a marker so the host can size the group box AROUND the
            // controls it contains. Flattening without this made every group
            // render as a small empty box above its contents.
            auto ge = mk(NK::UiItem, cur().line);
            ge->str = "groupend";
            ge->name = "";
            n->kids.push_back(std::move(ge));
            n->item_count++;
            --n->group_depth; ++pos; continue;
        }

        // In a parameters block ANY identifier-led line is a declaration:
        //     unk14  type:#integer default:1 ui:spUnk14
        // Note the keyword args start at the SECOND token, not the first, so
        // a one-token lookahead for ':' misses them.
        if (isParams && !cur().text.empty() && cur().t != Tok::KwLocal &&
            cur().t != Tok::KwGlobal && cur().t != Tok::KwFn &&
            cur().t != Tok::KwOn && !at(Tok::RParen)) {
            const int pl = cur().line;
            auto par = mk(NK::UiItem, pl);
            par->str = "parameter";
            par->name = lower(cur().text); ++pos;
            while (!at(Tok::End) && !cur().nl_before) {
                if (!cur().text.empty() && peek().t == Tok::Colon) {
                    par->kw_names.push_back(lower(cur().text)); pos += 2;
                    par->kids.push_back(operand());
                } else break;
            }
            n->kids.push_back(std::move(par));
            n->item_count++;
            continue;
        }

        // <controlType> <name> ["caption"] params...
        if (!cur().text.empty() && is_ui_control(lower(cur().text)) &&
            !peek().text.empty() && peek().t != Tok::Colon) {
            const int il = cur().line;
            auto it = mk(NK::UiItem, il);
            it->str = lower(cur().text); ++pos;         // control type
            it->name = lower(cur().text); ++pos;        // variable name
            if (at(Tok::String)) { it->lit = cur().v; ++pos; }   // caption
            while (!at(Tok::End) && !cur().nl_before) {
                if (!cur().text.empty() && peek().t == Tok::Colon) {
                    it->kw_names.push_back(lower(cur().text)); pos += 2;
                    it->kids.push_back(operand());
                } else break;
            }
            n->kids.push_back(std::move(it));
            n->item_count++;
            continue;
        }
        // anything else: ordinary statement inside the rollout body
        n->kids.push_back(expr());
        n->stmt_count++;
    }
    if (isAttrs && at(Tok::RParen)) n->src = source.substr(srcStart, cur().end_off - srcStart);
    expect(Tok::RParen, "')' closing rollout body");
    return n;
}

NodeP Parser::expr() {
    if (stack_exhausted()) err("expression nested too deeply");   // [auditA]
    // rcmenu is structurally identical to a rollout: <name> ( items,
    // 'on <item> picked do ...' ). Without this its body leaked into the
    // enclosing rollout, ending it early -- which is what made RGM_IMP fail
    // 1000 lines later with a misleading "expected 'while'".
    // 'attributes <Name> ( parameters ... rollout ... )' -- a custom attribute
    // definition. Same block shape as a rollout, and its body holds parameters
    // and rollout blocks that must be parsed, not called.
    if (at(Tok::KwRollout) || at(Tok::KwUtility) || at(Tok::KwParameters) ||
        at(Tok::KwRcMenu) || at(Tok::KwAttributes))
        return rollout_expr();

    // Contexts: with/at/in/about/animate/undo/redraw/quiet/coordsys and
    // 'set <context> <qualifier>'. See context_expr().
    if (at_context_start()) return context_expr();

    // macroScript / plugin / tool definitions
    {
        std::string kind;
        if (at_scripted_start(kind)) return scripted_expr(kind);
    }
    // 'mapped fn' / 'mapped function'
    if (at(Tok::Ident) && lower(cur().text) == "mapped" && peek().t == Tok::KwFn) {
        ++pos;
        NodeP f = fn_expr();
        f->flag = true;
        return f;
    }
    if (at_when_start()) return when_expr();   // [mxs25 F]
    // 'persistent global x = ...'
    if (at(Tok::Ident) && lower(cur().text) == "persistent" && peek().t == Tok::KwGlobal) {
        ++pos;
        NodeP g = global_expr();
        g->flag = true;
        return g;
    }

    switch (cur().t) {
        case Tok::KwIf:       return if_expr();
        case Tok::KwFor:      return for_expr();
        case Tok::KwWhile:    return while_expr();
        case Tok::KwDo:       return do_expr();
        case Tok::KwCase:     return case_expr();
        case Tok::KwFn:       return fn_expr();
        case Tok::KwStruct:   return struct_expr();
        case Tok::KwLocal:    return local_expr();
        case Tok::KwGlobal:   return global_expr();
        case Tok::KwTry:      return try_expr();
        case Tok::KwReturn: { int ln = cur().line; ++pos;
            auto n = mk(NK::Return, ln);
            if (!at(Tok::End) && !at(Tok::Semi) && !at(Tok::RParen) && !cur().nl_before)
                n->kids.push_back(expr());
            return n; }
        case Tok::KwExit: { int ln = cur().line; ++pos;
            auto n = mk(NK::Exit, ln);
            if (accept(Tok::KwWith)) { n->flag = true; n->kids.push_back(expr()); }
            return n; }
        case Tok::KwContinue: { int ln = cur().line; ++pos; return mk(NK::Continue, ln); }
        case Tok::KwThrow: { int ln = cur().line; ++pos;
            auto n = mk(NK::Throw, ln);
            if (!at(Tok::End) && !at(Tok::Semi) && !at(Tok::RParen) && !cur().nl_before)
                n->kids.push_back(expr());
            return n; }
        default: break;
    }
    // assignment?  dest = expr  |  dest op= expr
    NodeP lhs = or_expr();
    Tok t = cur().t;
    if (t == Tok::Assign || t == Tok::PlusEq || t == Tok::MinusEq || t == Tok::StarEq || t == Tok::SlashEq) {
        // ScenePath is a valid target root: '$.skin.enabled = false' assigns
        // through the current-selection path. It still raises a clear runtime
        // error until GX2 scene bindings exist, but it must PARSE.
        if (lhs->k != NK::Ident && lhs->k != NK::Prop &&
            lhs->k != NK::Index && lhs->k != NK::ScenePath)
            err("invalid assignment target");
        int ln = cur().line; ++pos;
        auto n = mk(t == Tok::Assign ? NK::Assign : NK::CompoundAssign, ln);
        // Right-associative, so 'a = b = c' parses as 'a = (b = c)'.
        if (t != Tok::Assign)
            n->name = (t == Tok::PlusEq ? "+" : t == Tok::MinusEq ? "-" : t == Tok::StarEq ? "*" : "/");
        n->kids.push_back(std::move(lhs));
        n->kids.push_back(expr());
        return n;
    }
    return lhs;
}
// Everything in MaxScript is an expression, so a declaration initialiser and
// the RHS of an assignment must accept control-flow forms too:
//     local verts = for i = 1 to n collect (readVert f)
//     local mode  = if hdr.flags == 1 then #skinned else #rigid
// or_expr() alone would stop at 'for'/'if' and report a syntax error.
NodeP Parser::expr_no_assign() {
    switch (cur().t) {
        case Tok::KwIf:     return if_expr();
        case Tok::KwFor:    return for_expr();
        case Tok::KwWhile:  return while_expr();
        case Tok::KwDo:     return do_expr();
        case Tok::KwCase:   return case_expr();
        case Tok::KwFn:     return fn_expr();
        case Tok::KwTry:    return try_expr();
        case Tok::KwStruct: return struct_expr();
        default: break;
    }
    return or_expr();
}

NodeP Parser::or_expr() {
    NodeP l = and_expr();
    while (at(Tok::KwOr)) { int ln = cur().line; ++pos;
        auto n = mk(NK::Or, ln); n->kids.push_back(std::move(l)); n->kids.push_back(and_expr()); l = std::move(n); }
    return l;
}
NodeP Parser::and_expr() {
    NodeP l = not_expr();
    while (at(Tok::KwAnd)) { int ln = cur().line; ++pos;
        auto n = mk(NK::And, ln); n->kids.push_back(std::move(l)); n->kids.push_back(not_expr()); l = std::move(n); }
    return l;
}
NodeP Parser::not_expr() {
    if (stack_exhausted()) err("expression nested too deeply");   // [auditA]
    if (at(Tok::KwNot)) { int ln = cur().line; ++pos;
        auto n = mk(NK::Unary, ln); n->name = "not"; n->kids.push_back(not_expr()); return n; }
    return cmp_expr();
}
NodeP Parser::cmp_expr() {
    NodeP l = add_expr();
    for (;;) {
        const char* op = nullptr;
        switch (cur().t) {
            case Tok::Eq: op = "=="; break; case Tok::Ne: op = "!="; break;
            case Tok::Lt: op = "<";  break; case Tok::Gt: op = ">";  break;
            case Tok::Le: op = "<="; break; case Tok::Ge: op = ">="; break;
            default: break;
        }
        if (!op) break;
        int ln = cur().line; ++pos;
        auto n = mk(NK::Binary, ln); n->name = op;
        n->kids.push_back(std::move(l)); n->kids.push_back(add_expr()); l = std::move(n);
    }
    return l;
}
NodeP Parser::add_expr() {
    NodeP l = mul_expr();
    for (;;) {
        if (at(Tok::Plus)) { int ln = cur().line; ++pos;
            auto n = mk(NK::Binary, ln); n->name = "+";
            n->kids.push_back(std::move(l)); n->kids.push_back(mul_expr()); l = std::move(n); }
        else if (at(Tok::Minus) && !arg_minus()) { int ln = cur().line; ++pos;
            auto n = mk(NK::Binary, ln); n->name = "-";
            n->kids.push_back(std::move(l)); n->kids.push_back(mul_expr()); l = std::move(n); }
        else break;
    }
    return l;
}
NodeP Parser::mul_expr() {
    NodeP l = pow_expr();
    for (;;) {
        const char* op = at(Tok::Star) ? "*" : at(Tok::Slash) ? "/" : nullptr;
        if (!op) break;
        int ln = cur().line; ++pos;
        auto n = mk(NK::Binary, ln); n->name = op;
        n->kids.push_back(std::move(l)); n->kids.push_back(pow_expr()); l = std::move(n);
    }
    return l;
}
NodeP Parser::pow_expr() {
    if (stack_exhausted()) err("expression nested too deeply");   // [auditA]
    NodeP l = as_expr();
    if (at(Tok::Caret)) { int ln = cur().line; ++pos;                    // right-assoc
        auto n = mk(NK::Binary, ln); n->name = "^";
        n->kids.push_back(std::move(l)); n->kids.push_back(pow_expr()); return n; }
    return l;
}
NodeP Parser::as_expr() {
    NodeP l = unary_expr();
    while (at(Tok::KwAs)) { int ln = cur().line; ++pos;
        if (!at(Tok::Ident)) err("expected type name after 'as'");
        auto n = mk(NK::AsCast, ln); n->name = lower(cur().text); ++pos;
        n->kids.push_back(std::move(l)); l = std::move(n);
    }
    return l;
}
NodeP Parser::unary_expr() {
    if (stack_exhausted()) err("expression nested too deeply");   // [auditA]
    if (at(Tok::Minus)) { int ln = cur().line; ++pos;
        auto n = mk(NK::Unary, ln); n->name = "-"; n->kids.push_back(unary_expr()); return n; }
    return postfix_expr(true);
}

// argument operand: primary + postfix, but no further juxtaposition and no binary ops
NodeP Parser::operand() {
    if (stack_exhausted()) err("expression nested too deeply");   // [auditA]
    if (at(Tok::Minus)) { int ln = cur().line; ++pos;
        auto n = mk(NK::Unary, ln); n->name = "-"; n->kids.push_back(operand()); return n; }
    if (at(Tok::Amp)) { int ln = cur().line; ++pos;                     // '&var' by-reference argument
        auto n = mk(NK::Unary, ln); n->name = "&"; n->kids.push_back(operand()); return n; }
    return postfix_expr(false);
}

NodeP Parser::postfix_expr(bool allow_call) {
    NodeP e = primary(allow_call);
    for (;;) {
        if (at(Tok::Dot) && !cur().nl_before) { int ln = cur().line; ++pos;
            if (cur().text.empty()) err("expected property name after '.'");
            auto n = mk(NK::Prop, ln); n->name = lower(cur().text); ++pos;
            n->kids.push_back(std::move(e));
            // Zero-arg METHOD call: obj.method(). Previously only bare f()
            // was handled, so 'primitives[p].matid()' -- ordinary in importers
            // that expose struct accessors -- failed to parse.
            if (at(Tok::LParen) && peek().t == Tok::RParen && !cur().nl_before) {
                pos += 2;
                auto c2 = mk(NK::Call, ln);
                c2->kids.push_back(std::move(n));
                e = std::move(c2);
            } else e = std::move(n);
        }
        // '[' is an INDEX only when glued to the expression: a[1].
        // With a space before it, it opens a point literal argument:
        //   print [1,2,3]   /  f [1,2,3]
        // This is MAXScript's own rule and the same shape as the '-'
        // disambiguation in Parser.h.
        else if (at(Tok::LBracket) && !cur().nl_before && !cur().sp_before) { int ln = cur().line; ++pos;
            auto n = mk(NK::Index, ln);
            n->kids.push_back(std::move(e)); n->kids.push_back(expr());
            expect(Tok::RBracket, "']'");
            e = std::move(n);
        }
        else break;
    }
    // juxtaposition call:  f a b kw:v  — only in call position, args on same line
    if (allow_call && (e->k == NK::Ident || e->k == NK::Prop)) {
        bool have_args = false;
        auto call = mk(NK::Call, e->line);
        call->kids.push_back(std::move(e));
        for (;;) {
            const Token& tk = cur();
            if (tk.nl_before) break;
            // keyword arg: ident ':' operand  (colon must be glued to the ident)
            if (!tk.text.empty() && peek().t == Tok::Colon && !tk.sp_after) {
                std::string kw = lower(tk.text); pos += 2;
                call->kw_names.push_back(std::move(kw));
                call->kids.push_back(operand());
                have_args = true; continue;
            }
            if (starts_operand(tk) || arg_minus()) {
                // MAXScript permits a positional arg AFTER keyword args:
                //   maxOps.cloneNodes objs newNodes:&arr #nodialog
                // The evaluator expects kids laid out as [callee][positional...]
                // [keyword...], so a late positional is INSERTED ahead of the
                // keyword kids rather than appended -- otherwise the two lists
                // interleave and every argument binds to the wrong parameter.
                NodeP pv = operand();
                call->kids.insert(call->kids.begin() + 1 + call->pos_argc, std::move(pv));
                ++call->pos_argc;
                have_args = true; continue;
            }
            break;
        }
        if (have_args) return call;
        e = std::move(call->kids[0]);
    }
    return e;
}

NodeP Parser::primary(bool /*allow_call*/) {
    if (stack_exhausted()) err("expression nested too deeply");   // [auditA]
    int ln = cur().line;

    // '&expr' -- by-reference argument. Handled HERE, not just in operand(),
    // because it appears inside parenthesised calls too:
    //     crap.read(&f)        vertexData[i].write_vertex_data(&s)
    // which reach primary() through block_paren() and never see operand().
    // We pass by value (documented limitation); the syntax must still parse,
    // and for the common importer case -- a struct or array being mutated --
    // the shared-pointer value semantics give the caller the update anyway.
    if (at(Tok::Amp)) { ++pos;
        auto n = mk(NK::Unary, ln); n->name = "&"; n->kids.push_back(postfix_expr(false)); return n; }

    // Point literal: [x,y] / [x,y,z] / [x,y,z,w].
    // Only valid HERE, in operand position -- postfix_expr() consumes '[' as
    // an index when it follows an expression, so the two never collide. This
    // is exactly how MAXScript disambiguates them, and it is why the earlier
    // "bracket means index, use (point3 x y z)" limitation was unnecessary.
    if (at(Tok::LBracket)) {
        ++pos;
        auto n = mk(NK::PointLit, ln);
        if (!at(Tok::RBracket)) {
            n->kids.push_back(expr_no_assign());
            while (accept(Tok::Comma)) n->kids.push_back(expr_no_assign());
        }
        if (!accept(Tok::RBracket)) err("expected ']' closing point literal");
        if (n->kids.size() < 2 || n->kids.size() > 4)
            err("point literal needs 2 to 4 components");
        return n;
    }

    switch (cur().t) {
        case Tok::Number: case Tok::String: case Tok::NameLit: {
            auto n = mk(NK::Lit, ln); n->lit = cur().v; ++pos; return n; }
        case Tok::KwTrue:  { ++pos; auto n = mk(NK::Lit, ln); n->lit = Value::B(true);  return n; }
        case Tok::KwOn:    { ++pos; auto n = mk(NK::Lit, ln); n->lit = Value::B(true);  return n; }
        case Tok::KwFalse: { ++pos; auto n = mk(NK::Lit, ln); n->lit = Value::B(false); return n; }
        case Tok::KwOff:   { ++pos; auto n = mk(NK::Lit, ln); n->lit = Value::B(false); return n; }
        case Tok::KwUndefined:  { ++pos; return mk(NK::Lit, ln); }
        case Tok::KwOk:         { ++pos; auto n = mk(NK::Lit, ln); n->lit = Value::ok(); return n; }
        case Tok::KwUnsupplied: { ++pos; auto n = mk(NK::Lit, ln); n->lit = Value::unsupplied(); return n; }
        case Tok::Ident: {
            // [mxsfix 1c] 'max <words>': the max command construct. Only when the next token is a
            // word on the same line, so 'max = 3' or 'x.max' keep their meaning.
            if (lower(cur().text) == "max" && !peek().nl_before && (peek().t == Tok::Ident || peek().t == Tok::Query ||
                                                                    (!peek().text.empty() && peek().t != Tok::Dollar))) {
                ++pos;
                auto c = mk(NK::MaxCmd, ln);
                while (!at(Tok::End) && !cur().nl_before && (at(Tok::Query) || (!cur().text.empty() && !at(Tok::Dollar)))) {
                    if (!c->name.empty()) c->name += ' ';
                    c->name += at(Tok::Query) ? std::string("?") : lower(cur().text);
                    ++pos;
                }
                return c;
            }
            auto n = mk(NK::Ident, ln); n->name = lower(cur().text); ++pos;
            // zero-arg call syntax: f()   (paren glued or spaced; empty parens only)
            if (at(Tok::LParen) && peek().t == Tok::RParen && !cur().nl_before) {
                pos += 2;
                auto c = mk(NK::Call, ln); c->kids.push_back(std::move(n)); return c;
            }
            return n; }
        case Tok::LParen: return block_paren();
        // '?' -- the previous top-level result (Listener convention)
        case Tok::Query: { ++pos; auto n = mk(NK::Ident, ln); n->name = "?"; return n; }
        case Tok::ArrayOpen: { ++pos;
            auto n = mk(NK::ArrayLit, ln);
            if (!accept(Tok::RParen)) {
                for (;;) { n->kids.push_back(expr()); if (accept(Tok::Comma)) continue; expect(Tok::RParen, "')'"); break; }
            }
            return n; }
        case Tok::BitArrayOpen: { ++pos;
            auto n = mk(NK::BitArrayLit, ln);
            if (!accept(Tok::RBrace)) {
                for (;;) {
                    NodeP a = expr_no_assign();
                    if (accept(Tok::DotDot)) {                     // range  a..b
                        auto r = mk(NK::Binary, ln); r->name = "..";
                        r->kids.push_back(std::move(a)); r->kids.push_back(expr_no_assign());
                        a = std::move(r);
                    }
                    n->kids.push_back(std::move(a));
                    if (accept(Tok::Comma)) continue;
                    expect(Tok::RBrace, "'}'"); break;
                }
            }
            return n; }
        case Tok::Dollar: {
            // Scene path: $foo, $*, $Spine*, $'quoted name'.
            // Resolved at runtime through Engine::scene_path_hook, which the
            // GX2 scene bindings register.
            std::string path = cur().text;    // lexed pathname spelling (see lexer)
            ++pos;
            if (!path.empty()) {}
            else if (at(Tok::Star) && !cur().sp_before) { path = "*"; ++pos; }
            else if (at(Tok::String)) { path = *cur().v.s; ++pos; }
            else {
                // Accumulate token by token, using each token's OWN spelling.
                // Appending "/" for anything without text turned '$Spine*'
                // into '$Spine/' and silently broke every wildcard path.
                while (!at(Tok::End) && !cur().sp_before) {
                    if (at(Tok::Dot)) break;              // '.' starts a property
                    if (!cur().text.empty())      path += cur().text;
                    else if (at(Tok::Star))       path += "*";
                    else if (at(Tok::Query))      path += "?";
                    else if (at(Tok::Number))     path += cur().v.to_string();
                    else break;
                    ++pos;
                    if (path.size() > 256) break;
                }
            }
            auto n = mk(NK::ScenePath, ln); n->name = path; return n;
        }
        default: err("unexpected token");
    }
}

NodeP Parser::block_paren() {
    int ln = cur().line;
    expect(Tok::LParen, "'('");
    auto blk = mk(NK::Block, ln);
    skip_separators();
    while (!at(Tok::RParen)) {
        if (at(Tok::End)) err("unterminated '(' block");
        blk->kids.push_back(expr());
        skip_separators();
    }
    ++pos; // ')'
    return blk;
}

// Inside an if/while condition MAXScript reads a single '=' as EQUALITY:
//     if exponent = 255 - 127 + 15 then ...
// Conditions are parsed with expr_no_assign(), so the assignment path never
// sees them; fold the '=' into '==' here instead.
NodeP Parser::condition_expr() {
    NodeP cond = expr_no_assign();
    if (at(Tok::Assign)) {
        const int cl = cur().line; ++pos;
        auto eq = mk(NK::Binary, cl); eq->name = "==";
        eq->kids.push_back(std::move(cond));
        eq->kids.push_back(expr_no_assign());
        return eq;
    }
    return cond;
}

NodeP Parser::if_expr() {
    int ln = cur().line; ++pos;
    auto n = mk(NK::If, ln);
    n->kids.push_back(condition_expr());
    if (accept(Tok::KwThen)) {
        n->kids.push_back(expr());
        if (accept(Tok::KwElse)) { n->flag = true; n->kids.push_back(expr()); }
    } else if (accept(Tok::KwDo)) {
        n->kids.push_back(expr());
    } else err("expected 'then' or 'do' after if condition");
    return n;
}

// [mxs25 F] Change handlers (MAXScript-Help "Change Handlers and When Constructs"):
//     when <attribute> <objects> change[s] [id:<name>] [handleAt:#redrawViews|#timeChange] [<param>] do <expr>
//     when <objects> deleted [id:<name>] [handleAt:...] [<param>] do <expr>
// 'when' is not reserved: it starts the construct only when a 'change'/'changes'/'deleted' word
// follows on the same line (outside brackets) before 'do'.
bool Parser::at_when_start() const {
    if (!at(Tok::Ident) || lower(cur().text) != "when") return false;
    int depth = 0;
    for (size_t k = pos + 1; k < toks.size(); ++k) {
        const Token& t = toks[k];
        if (t.t == Tok::End || (depth == 0 && t.nl_before)) return false;
        if (t.t == Tok::LParen || t.t == Tok::ArrayOpen || t.t == Tok::BitArrayOpen || t.t == Tok::LBracket) ++depth;
        else if (t.t == Tok::RParen || t.t == Tok::RBracket || t.t == Tok::RBrace) { if (--depth < 0) return false; }
        else if (depth == 0 && t.t == Tok::KwDo) return false;
        else if (depth == 0 && t.t == Tok::Ident && k > pos + 1) {
            std::string w = lower(t.text);
            if (w == "change" || w == "changes" || w == "deleted") return true;
        }
        else if (depth == 0 && (t.t == Tok::Assign || t.t == Tok::Semi)) return false;
    }
    return false;
}
NodeP Parser::when_expr() {
    auto n = mk(NK::When, cur().line); ++pos;
    // the deciding word tells the form apart
    bool deletedForm = false;
    { int depth = 0;
      for (size_t k = pos; k < toks.size(); ++k) {
          const Token& t = toks[k];
          if (t.t == Tok::LParen || t.t == Tok::ArrayOpen || t.t == Tok::BitArrayOpen || t.t == Tok::LBracket) ++depth;
          else if (t.t == Tok::RParen || t.t == Tok::RBracket || t.t == Tok::RBrace) --depth;
          else if (depth == 0 && t.t == Tok::Ident) {
              std::string w = lower(t.text);
              if (w == "change" || w == "changes") break;
              if (w == "deleted") { deletedForm = true; break; }
          }
      } }
    if (deletedForm) n->name = "deleted";
    else {
        if (cur().text.empty()) err("expected change attribute after 'when'");   // 'parameters' is a keyword token
        n->name = lower(cur().text); ++pos;
    }
    n->kids.push_back(operand());
    if (!at(Tok::Ident)) err(deletedForm ? "expected 'deleted' in when construct" : "expected 'change' or 'changes' in when construct");
    { std::string w = lower(cur().text);
      if (deletedForm ? w != "deleted" : (w != "change" && w != "changes"))
          err(deletedForm ? "expected 'deleted' in when construct" : "expected 'change' or 'changes' in when construct");
      ++pos; }
    while (!at(Tok::KwDo) && !at(Tok::End)) {
        if (at(Tok::Ident) && peek().t == Tok::Colon) {
            std::string k = lower(cur().text); pos += 2;
            if (k != "id" && k != "handleat") err("unexpected keyword in when construct: " + k);
            n->kw_names.push_back(k);
            n->kids.push_back(operand());
            continue;
        }
        if (at(Tok::Ident) && n->fn_params.empty()) { n->fn_params.push_back(lower(cur().text)); ++pos; continue; }
        err("expected 'do' in when construct");
    }
    expect(Tok::KwDo, "'do'");
    n->kids.push_back(expr());
    return n;
}

// After 'for x in', decide whether this is a numeric range ('in 2 to c') or a
// collection ('in verts'). Scan ahead for a 'to' before the header ends.
bool Parser::peek_is_numeric_range() const {
    for (size_t k = pos + 1; k < toks.size(); ++k) {
        const Tok t = toks[k].t;
        if (t == Tok::KwTo) return true;
        if (t == Tok::KwDo || t == Tok::KwCollect || t == Tok::KwWhere ||
            t == Tok::End || toks[k].nl_before) return false;
    }
    return false;
}

NodeP Parser::for_expr() {
    int ln = cur().line; ++pos;
    if (!at(Tok::Ident)) err("expected loop variable after 'for'");
    std::string var = lower(cur().text); ++pos;
    // [where <expr>] [while <expr>] (either order) then do|collect <body>.
    // kw_names = [do|collect, where/while...]; their expressions sit in kids
    // right before the body, in the same order.
    auto tail = [&](NodeP& n) {
        std::vector<std::pair<std::string, NodeP>> extra;
        for (;;) {
            if (accept(Tok::KwWhere)) { extra.emplace_back("where", expr_no_assign()); continue; }
            if (accept(Tok::KwWhile)) { extra.emplace_back("while", expr_no_assign()); continue; }
            break;
        }
        bool collect = false;
        if (accept(Tok::KwCollect)) collect = true; else expect(Tok::KwDo, "'do' or 'collect'");
        n->kw_names.push_back(collect ? "collect" : "do");
        for (auto& x : extra) { n->kw_names.push_back(x.first); n->kids.push_back(std::move(x.second)); }
        n->kids.push_back(expr());
    };
    if ((at(Tok::KwIn) && peek_is_numeric_range()) || at(Tok::Assign)) {
        // 'for i in a to b [by s]' is the same loop as 'for i = a to b',
        // just spelled with 'in'. Real scripts use both interchangeably.
        ++pos;
        auto n = mk(NK::ForNum, ln); n->name = var;
        n->kids.push_back(expr_no_assign());
        expect(Tok::KwTo, "'to'");
        ++stop_by; try { n->kids.push_back(expr_no_assign()); } catch (...) { --stop_by; throw; } --stop_by;
        if (at(Tok::Ident) && lower(cur().text) == "by") { ++pos; n->flag = true; n->kids.push_back(expr_no_assign()); }
        tail(n);
        return n;
    }
    if (accept(Tok::KwIn)) {                                       // for x in coll
        auto n = mk(NK::ForIn, ln); n->name = var;
        n->kids.push_back(expr_no_assign());
        tail(n);
        return n;
    }
    err("expected '=' or 'in' in for loop");
}

NodeP Parser::while_expr() {
    int ln = cur().line; ++pos;
    auto n = mk(NK::While, ln);
    n->kids.push_back(expr_no_assign());
    expect(Tok::KwDo, "'do'");
    n->kids.push_back(expr());
    return n;
}
NodeP Parser::do_expr() {
    int ln = cur().line; ++pos;
    auto n = mk(NK::DoWhile, ln);
    n->kids.push_back(expr());
    expect(Tok::KwWhile, "'while'");
    n->kids.push_back(expr_no_assign());
    return n;
}

NodeP Parser::case_expr() {
    int ln = cur().line; ++pos;
    auto n = mk(NK::Case, ln);
    if (!accept(Tok::KwOf)) { n->flag = true; n->kids.push_back(expr_no_assign()); expect(Tok::KwOf, "'of'"); }
    expect(Tok::LParen, "'('");
    skip_separators();
    // labels: (factor | default) ':' expr   — stored as [label, body] pairs (label==nullptr for default)
    while (!at(Tok::RParen)) {
        if (at(Tok::End)) err("unterminated case block");
        if (accept(Tok::KwDefault)) {
            expect(Tok::Colon, "':'");
            n->kw_names.push_back("default");
            n->kids.push_back(nullptr);
        } else {
            n->kw_names.push_back("label");
            n->kids.push_back(operand());
            expect(Tok::Colon, "':'");
        }
        n->kids.push_back(expr());
        skip_separators();
    }
    ++pos;
    return n;
}

NodeP Parser::fn_expr() {
    int ln = cur().line; ++pos;
    if (!at(Tok::Ident)) err("expected function name");
    auto n = mk(NK::FnDef, ln); n->name = lower(cur().text); ++pos;
    // params until '='
    while (!at(Tok::Assign)) {
        if (at(Tok::End)) err("expected '=' in function definition");
        // '&name' marks a by-reference parameter. We pass by value (documented
        // limitation), but the DECLARATION must parse -- 'fn read &f = ...' is
        // in essentially every binary importer.
        const bool byref = accept(Tok::Amp);
        (void)byref;
        if (!at(Tok::Ident)) err("expected parameter name");
        std::string pn = lower(cur().text); ++pos;
        if (accept(Tok::Colon)) {                                  // keyword param, optional default
            n->fn_kwparams.push_back(pn);
            bool next_is_param = !cur().text.empty() && peek().t == Tok::Colon && !cur().sp_after;
            if (at(Tok::Assign) || next_is_param) {                // bare 'name:' -> unsupplied
                auto d = mk(NK::Lit, cur().line); d->lit = Value::unsupplied();
                n->kids.push_back(std::move(d));
            } else n->kids.push_back(operand());
        } else {
            n->fn_params.push_back(pn);
        }
        accept(Tok::Amp);                                          // tolerate &ref markers
    }
    ++pos; // '='
    n->kids.push_back(expr());                                     // body (last kid)
    return n;
}

NodeP Parser::struct_expr() {
    int ln = cur().line; ++pos;
    if (!at(Tok::Ident)) err("expected struct name");
    auto n = mk(NK::StructDef, ln); n->name = lower(cur().text); ++pos;
    expect(Tok::LParen, "'('");
    skip_separators();
    while (!at(Tok::RParen)) {
        if (at(Tok::End)) err("unterminated struct");
        if (at(Tok::Ident) && (lower(cur().text) == "private" || lower(cur().text) == "public") &&
            peek().t != Tok::Assign && peek().t != Tok::Comma && peek().t != Tok::RParen) {
            ++pos;                                                 // access specifier: no effect here
            continue;
        }
        if (at(Tok::KwOn)) {                                       // on create do <expr> / on clone do <expr>
            ++pos;
            if (cur().text.empty()) err("expected event name after 'on' in struct");
            std::string ev = lower(cur().text); ++pos;
            expect(Tok::KwDo, "'do' in struct event handler");
            n->kw_names.push_back("on:" + ev);
            n->kids.push_back(expr());
            skip_separators();
            if (accept(Tok::Comma)) skip_separators();
            continue;
        }
        if (at(Tok::Ident) && lower(cur().text) == "mapped" && peek().t == Tok::KwFn) {
            ++pos;
            NodeP f = fn_expr();
            f->flag = true;
            n->kw_names.push_back("fn:" + f->name);
            n->kids.push_back(std::move(f));
            skip_separators();
            if (accept(Tok::Comma)) skip_separators();
            continue;
        }
        if (at(Tok::KwFn)) {                                       // member function
            NodeP f = fn_expr();
            n->kw_names.push_back("fn:" + f->name);
            n->kids.push_back(std::move(f));
        } else {
            if (!at(Tok::Ident)) err("expected struct member");
            std::string mn = lower(cur().text); ++pos;
            n->kw_names.push_back(mn);
            if (accept(Tok::Assign)) n->kids.push_back(expr_no_assign());
            else n->kids.push_back(nullptr);
        }
        skip_separators();
        if (accept(Tok::Comma)) skip_separators();
    }
    ++pos;
    return n;
}


NodeP Parser::local_expr() {
    int ln = cur().line; ++pos;
    auto n = mk(NK::LocalDecl, ln);
    for (;;) {
        if (!at(Tok::Ident)) err("expected variable name after 'local'");
        n->kw_names.push_back(lower(cur().text)); ++pos;
        // expr(), not expr_no_assign(): the initialiser may itself be an
        // assignment -- 'local i = obj.numsubs = n' appears in real scripts.
        if (accept(Tok::Assign)) n->kids.push_back(expr());
        else n->kids.push_back(nullptr);
        if (!accept(Tok::Comma)) break;
    }
    return n;
}
NodeP Parser::global_expr() {
    int ln = cur().line; ++pos;
    auto n = mk(NK::GlobalDecl, ln);
    for (;;) {
        if (!at(Tok::Ident)) err("expected variable name after 'global'");
        n->kw_names.push_back(lower(cur().text)); ++pos;
        // expr(), not expr_no_assign(): the initialiser may itself be an
        // assignment -- 'local i = obj.numsubs = n' appears in real scripts.
        if (accept(Tok::Assign)) n->kids.push_back(expr());
        else n->kids.push_back(nullptr);
        if (!accept(Tok::Comma)) break;
    }
    return n;
}


// ------------------------------------------------------------ contexts ----
// <context> { , <context> } <expr>
//   [with] animate <bool>      at time <time>      at level <node>
//   in <node>                  [in] coordsys <local|world|parent|screen|grid|gimbal|node>
//   about <pivot|selection|center|coordsys|node|point3>
//   [with] undo [<label>] <bool>   [with] redraw <bool>   [with] quiet <bool>
//   set <context> <qualifier>  (persistent; no body)
// Each clause becomes a WithCtx node: str = context word, flag = has qualifier,
// kids = [label?] qualifier body (pos_argc = 1 when a label is present).
static bool is_ctx_word(const std::string& w) {
    return w == "animate" || w == "undo" || w == "redraw" || w == "quiet" || w == "coordsys" ||
           w == "time" || w == "level" || w == "about" || w == "printallelements" ||
           w == "defaultaction" || w == "mxscallstackcaptureenabled";
}
static bool is_onoff(Tok t) { return t == Tok::KwOn || t == Tok::KwOff || t == Tok::KwTrue || t == Tok::KwFalse; }

bool Parser::at_context_start() const {
    if (at(Tok::KwWith)) return true;
    if (at(Tok::KwAt)) {
        const std::string w = lower(peek().text);
        return w == "time" || w == "level";
    }
    if (at(Tok::KwAbout)) return true;
    if (at(Tok::KwIn) ) return true;
    if (!at(Tok::Ident)) return false;
    const std::string w = lower(cur().text);
    if (w == "set" && !peek().nl_before && is_ctx_word(lower(peek().text)) && peek(2).t != Tok::Assign)
        return true;
    if (w == "animate" || w == "redraw" || w == "quiet") return is_onoff(peek().t) && !peek().nl_before;
    if (w == "undo") {
        if (peek().nl_before) return false;
        if (is_onoff(peek().t)) return true;
        return (peek().t == Tok::String || peek().t == Tok::Ident) && is_onoff(peek(2).t);
    }
    if (w == "coordsys") {
        const Token& q = peek();
        if (q.nl_before) return false;
        return q.t == Tok::KwLocal || q.t == Tok::Ident || q.t == Tok::Dollar || q.t == Tok::LParen;
    }
    return false;
}

NodeP Parser::ctx_clause(const std::string& word, int ln) {
    auto n = mk(NK::WithCtx, ln);
    n->str = word;
    n->flag = true;
    auto nameLit = [&](const std::string& nm) { auto l = mk(NK::Lit, cur().line); l->lit = Value::N(nm); return l; };
    if (word == "coordsys" || word == "about") {
        // symbolic qualifiers read as names; anything else is an operand (node, point3)
        if (at(Tok::KwLocal)) { n->kids.push_back(nameLit("local")); ++pos; }
        else if (at(Tok::Ident)) {
            const std::string q = lower(cur().text);
            static const char* syms[] = {"world","parent","screen","grid","gimbal","local","pivot",
                                         "selection","center","coordsys","working_pivot", nullptr};
            bool sym = false;
            for (const char* const* k = syms; *k; ++k) if (q == *k) sym = true;
            if (sym) { n->kids.push_back(nameLit(q)); ++pos; }
            else n->kids.push_back(operand());
        } else n->kids.push_back(operand());
    } else if (word == "undo") {
        if (at(Tok::String) || (at(Tok::Ident) && is_onoff(peek().t))) {
            n->kids.push_back(operand());          // label
            n->pos_argc = 1;
        }
        n->kids.push_back(operand());
    } else {
        n->kids.push_back(operand());
    }
    return n;
}

NodeP Parser::context_expr() {
    const int ln = cur().line;
    // 'set <ctx> <q>'
    if (at(Tok::Ident) && lower(cur().text) == "set") {
        ++pos;
        std::string w = lower(cur().text); ++pos;
        NodeP c = ctx_clause(w, ln);
        c->name = "set";
        return c;
    }
    std::vector<NodeP> clauses;
    bool first = true;
    for (;;) {
        bool with = false;
        if (accept(Tok::KwWith)) with = true;
        else if (!first && !at(Tok::Ident) && !at(Tok::KwAt) && !at(Tok::KwIn) && !at(Tok::KwAbout)) break;
        const int cl = cur().line;
        if (at(Tok::KwAt)) {
            ++pos;
            std::string w = lower(cur().text); ++pos;      // time / level
            clauses.push_back(ctx_clause(w, cl));
        } else if (at(Tok::KwAbout)) {
            ++pos;
            clauses.push_back(ctx_clause("about", cl));
        } else if (at(Tok::KwIn)) {
            ++pos;
            if (at(Tok::Ident) && lower(cur().text) == "coordsys") { ++pos; clauses.push_back(ctx_clause("coordsys", cl)); }
            else clauses.push_back(ctx_clause("in", cl));
        } else if (!cur().text.empty()) {
            std::string w = lower(cur().text);
            if (!with && !is_ctx_word(w)) break;
            ++pos;
            clauses.push_back(ctx_clause(w, cl));
        } else {
            if (with) err("expected context after 'with'");
            break;
        }
        first = false;
        if (!accept(Tok::Comma)) break;
    }
    NodeP body = expr();
    for (size_t i = clauses.size(); i-- > 0;) {
        clauses[i]->kids.push_back(std::move(body));
        body = std::move(clauses[i]);
    }
    return body;
}

// ------------------------------------------------- scripted definitions ----
//   macroScript <name> [<"title">] {<kw>:<operand>} ( <body> )
//   plugin <superclass> <name> {<kw>:<operand>} ( <body> )
//   tool <name> {<kw>:<operand>} ( <body> )
// <body> items: on <event> {<arg>} (do|return) <expr> | parameters/rollout
// blocks | nested tool | any other expression (locals, fns ...).
// Node: str = kind, name, lit = superclass (plugin) or title (macro);
// the first pos_argc kids are header keyword values (kw_names), then items.
bool Parser::at_scripted_start(std::string& kind) const {
    if (!at(Tok::Ident)) return false;
    const std::string w = lower(cur().text);
    if (w == "macroscript" && peek().t == Tok::Ident) { kind = "macroscript"; return true; }
    if (w == "plugin" && peek().t == Tok::Ident && peek(2).t == Tok::Ident) { kind = "plugin"; return true; }
    if (w == "tool" && peek().t == Tok::Ident &&
        (peek(2).t == Tok::LParen || (peek(2).t == Tok::Ident && peek(3).t == Tok::Colon))) { kind = "tool"; return true; }
    return false;
}

NodeP Parser::scripted_expr(const std::string& kind) {
    const int ln = cur().line;
    const size_t start = cur().off;
    ++pos;
    auto n = mk(NK::Scripted, ln);
    n->str = kind;
    if (kind == "plugin") { n->lit = Value::S(lower(cur().text)); ++pos; }
    if (cur().text.empty()) err("expected " + kind + " name");
    n->name = cur().text; ++pos;
    if (kind == "macroscript" && at(Tok::String)) { n->lit = cur().v; ++pos; }
    while (!at(Tok::LParen) && !at(Tok::End)) {
        if (!cur().text.empty() && peek().t == Tok::Colon) {
            n->kw_names.push_back(lower(cur().text)); pos += 2;
            n->kids.push_back(operand());
            ++n->pos_argc;
        } else err("unexpected token in " + kind + " header");
    }
    expect(Tok::LParen, "'(' opening body");
    skip_separators();
    while (!at(Tok::RParen)) {
        if (at(Tok::End)) err("unterminated " + kind + " body");
        if (at(Tok::KwOn)) {
            const int hl = cur().line; ++pos;
            auto h = mk(NK::OnHandler, hl);
            if (cur().text.empty()) err("expected event name after 'on'");
            h->name = lower(cur().text); ++pos;
            while (!at(Tok::KwDo) && !at(Tok::KwReturn) && !at(Tok::End)) {
                if (at(Tok::Ident)) { h->fn_params.push_back(lower(cur().text)); ++pos; }
                else err("expected 'do' in '" + h->name + "' handler");
            }
            if (!accept(Tok::KwDo)) expect(Tok::KwReturn, "'do' or 'return'");
            h->kids.push_back(expr());
            n->kids.push_back(std::move(h));
        } else {
            std::string k2;
            if (at_scripted_start(k2) && k2 == "tool") n->kids.push_back(scripted_expr(k2));
            else n->kids.push_back(expr());
        }
        skip_separators();
    }
    n->src = source.substr(start, cur().end_off - start);
    ++pos;   // ')'
    return n;
}

NodeP Parser::try_expr() {
    int ln = cur().line; ++pos;
    auto n = mk(NK::TryCatch, ln);
    n->kids.push_back(expr());
    expect(Tok::KwCatch, "'catch'");
    n->kids.push_back(expr());
    return n;
}

} // namespace mxs
