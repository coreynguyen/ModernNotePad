// evaluate.cpp - "Evaluate expression": calculator + text generator in the editor, powered by the
// MAXScript interpreter from MAX2Modern (third_party/mxs, no 3ds Max dependencies).
//
//   4+6            -> 4+6=10            (result appended; spacing follows the expression)
//   4+6=           -> 4+6=10            (just the answer after the '=')
//   Total 12*3.5   -> Total 12*3.5=42   (trailing arithmetic is found even inside prose)
//   for i = 1 to 3 do format "#%\n" i   -> output lines inserted below the code
//   x = 5 / x*x    -> variables persist between evaluations for the session
// "Replace with result" swaps the expression (or script) for its result/output instead.
//
// Sandbox: file and process natives are denied (Engine::CapFile | CapProcess), each run has a 3 s
// watchdog and output is capped, so a runaway loop or a pasted script can't touch the disk or hang the UI.
#include "app.h"
#include "../../third_party/mxs/include/util_mxs/mxs.h"
#include <cmath>
#include <cwctype>

namespace app {

static std::unique_ptr<mxs::Engine> s_eng;
static std::string s_out;
static bool s_outCapped = false;

static std::string U8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
static std::wstring W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}

static mxs::Engine& Eng() {
    if (!s_eng) {
        s_eng = std::make_unique<mxs::Engine>();
        s_eng->on_output = [](const std::string& s) {
            if (s_out.size() > (4u << 20)) { s_outCapped = true; s_eng->request_abort(); return; }
            s_out += s;
        };
        s_eng->denied_caps = mxs::Engine::CapFile | mxs::Engine::CapProcess;
        s_eng->timeout_ms = 3000;
        // calculator conveniences
        s_eng->set_global("e", mxs::Value::F(2.718281828459045));
    }
    return *s_eng;
}

static std::wstring FormatNumber(double v) {
    if (std::isnan(v)) return L"NaN";
    if (std::isinf(v)) return v > 0 ? L"Infinity" : L"-Infinity";
    if (std::fabs(v - std::round(v)) < 1e-9 * std::max(1.0, std::fabs(v)) && std::fabs(v) < 1e15) {
        wchar_t b[40]; swprintf(b, 40, L"%.0f", std::round(v)); return std::wstring(b) == L"-0" ? L"0" : b;
    }
    wchar_t b[64]; swprintf(b, 64, L"%.12g", v);
    return b;
}
static std::wstring FormatValue(const mxs::Value& v, bool& has) {
    has = true;
    switch (v.t) {
    case mxs::Type::Undefined: case mxs::Type::Ok: case mxs::Type::Unsupplied: has = false; return L"";
    case mxs::Type::Int: return std::to_wstring(v.i);
    case mxs::Type::Float: return FormatNumber(v.f);
    case mxs::Type::String: return W(v.s ? *v.s : std::string());
    default: return W(v.to_display());
    }
}

struct EvalOut { bool ok = false; std::wstring err, value, output; bool hasValue = false; };
static EvalOut Run(const std::wstring& code) {
    EvalOut r;
    mxs::Engine& e = Eng();
    s_out.clear(); s_outCapped = false;
    try {
        mxs::Value v = e.execute(U8(code), "<editor>");
        r.value = FormatValue(v, r.hasValue);
        r.ok = true;
    } catch (const mxs::MxsError& x) {
        r.err = W(x.what());
        if (s_outCapped) { r.ok = true; r.err.clear(); }
    } catch (const std::exception& x) {
        r.err = W(x.what());
    } catch (...) { r.err = L"evaluation failed"; }
    r.output = W(s_out);
    if (s_outCapped) r.output += L"\n... (output truncated at 4 MB)";
    while (!r.output.empty() && (r.output.back() == L'\n' || r.output.back() == L'\r')) r.output.pop_back();
    return r;
}

// Longest trailing run that looks like arithmetic ("Total: 12*3.5" -> "12*3.5").
static bool TrailingMath(const std::wstring& s, size_t& start) {
    int i = (int)s.size();
    bool digit = false, op = false;
    while (i > 0) {
        wchar_t c = s[i - 1];
        if (iswdigit(c)) digit = true;
        else if (wcschr(L"+-*/^%", c)) op = true;
        else if (!wcschr(L"(). \t", c)) break;
        --i;
    }
    while (i < (int)s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L')' || s[i] == L'*' || s[i] == L'/' || s[i] == L'^')) ++i;
    start = i;
    return digit && op && i < (int)s.size();
}
// Calculator semantics for one-line expressions: MAXScript (like C) does integer division, so 1850/3 is 616.
// When a line divides, integer literals are written as floats first (strings and identifiers untouched);
// whole results still print without a decimal point.
static std::wstring FloatLiterals(const std::wstring& s) {
    std::wstring o; bool str = false;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'"' && (i == 0 || s[i - 1] != L'\\')) str = !str;
        if (!str && iswdigit(c) && (i == 0 || (!iswalnum(s[i - 1]) && s[i - 1] != L'_' && s[i - 1] != L'.'))) {
            size_t j = i; while (j < s.size() && iswdigit(s[j])) ++j;
            bool more = j < s.size() && (s[j] == L'.' || iswalpha(s[j]) || s[j] == L'_');
            o.append(s, i, j - i); if (!more) o += L".0";
            i = j - 1; continue;
        }
        o += c;
    }
    return o;
}
// 'for i = 1 to 10 do format "#%\n"' : a one-line loop whose format string has placeholders but no
// arguments gets the loop variable supplied, which is clearly what was meant ("#1", "#2", ...).
static std::wstring SupplyLoopVar(const std::wstring& s) {
    std::wstring low = s; for (auto& c : low) c = towlower(c);
    size_t f = low.find_first_not_of(L" \t");
    if (f == std::wstring::npos || low.compare(f, 4, L"for ") != 0) return s;
    size_t v0 = low.find_first_not_of(L" \t", f + 4), v1 = v0;
    while (v1 < low.size() && (iswalnum(low[v1]) || low[v1] == L'_')) ++v1;
    if (v0 == std::wstring::npos || v1 == v0) return s;
    size_t fm = low.rfind(L"format ");
    if (fm == std::wstring::npos) return s;
    size_t q0 = s.find(L'"', fm); if (q0 == std::wstring::npos) return s;
    size_t q1 = q0 + 1; while (q1 < s.size() && s[q1] != L'"') { if (s[q1] == L'\\') ++q1; ++q1; }
    if (q1 >= s.size() || s.find_first_not_of(L" \t", q1 + 1) != std::wstring::npos) return s;   // already has args
    int n = 0; for (size_t i = q0 + 1; i < q1; ++i) if (s[i] == L'%') ++n;
    std::wstring o = s;
    for (int i = 0; i < n; ++i) o += L" " + s.substr(v0, v1 - v0);
    return o;
}
static bool EndsWithAssignEq(const std::wstring& t) {
    if (t.empty() || t.back() != L'=') return false;
    if (t.size() >= 2 && wcschr(L"=!<>+-*/", t[t.size() - 2])) return false;
    return true;
}

void EvaluateCommand(Doc* d, bool replace) {
    if (!d || d->hex || d->grid || d->loading || d->binaryOnly) return;
    np::Pos a = std::min(d->caret, d->anchor), b = std::max(d->caret, d->anchor);
    bool sel = a != b;
    if (!sel) { a = { d->caret.line, 0 }; b = { d->caret.line, (int)d->text.Line(d->caret.line).size() }; }
    std::wstring src = d->text.GetText(a, b);
    // trim trailing whitespace off the range
    while (!src.empty() && (src.back() == L' ' || src.back() == L'\t' || src.back() == L'\n' || src.back() == L'\r')) {
        src.pop_back(); b = d->text.PosAt(d->text.Offset(a) + src.size());
    }
    if (src.find_first_not_of(L" \t\r\n") == std::wstring::npos) { SetStatus(L"Evaluate: nothing to evaluate on this line"); return; }
    bool multi = src.find(L'\n') != std::wstring::npos;
    bool eq = !multi && EndsWithAssignEq(src);
    std::wstring code = eq ? src.substr(0, src.size() - 1) : src;
    size_t lead = code.find_first_not_of(L" \t");
    // markdown/list noise in front of a calculation ("- 4*5", "> 2^8")
    if (!multi && lead != std::wstring::npos && code.size() > lead + 2 && wcschr(L"-*>", code[lead]) && code[lead + 1] == L' ') lead += 2;
    std::wstring body = lead == std::wstring::npos ? code : code.substr(lead);
    if (!multi) body = SupplyLoopVar(body);

    bool divides = !multi && body.find(L'/') != std::wstring::npos;
    EvalOut r = Run(divides ? FloatLiterals(body) : body);
    if (divides && !r.ok) r = Run(body);
    size_t fragAt = std::wstring::npos;
    if ((!r.ok || (!r.hasValue && r.output.empty())) && !multi) {   // prose with arithmetic at the end
        size_t st;
        if (TrailingMath(code, st)) {
            std::wstring frag = code.substr(st);
            EvalOut f = Run(frag.find(L'/') != std::wstring::npos ? FloatLiterals(frag) : frag);
            if (f.ok && f.hasValue) { r = f; fragAt = st; }
        }
    }
    if (!r.ok) { SetStatus(L"Evaluate: " + r.err); MessageBeep(MB_ICONWARNING); return; }
    if (!r.hasValue && r.output.empty()) { SetStatus(L"Evaluate: OK (no result)"); return; }

    std::wstring ins; np::Pos ia = b, ib = b;
    std::wstring nl = L"\n";
    if (replace) {
        ia = a; ib = b;
        ins = !r.output.empty() ? r.output : r.value;
        if (!sel && lead != std::wstring::npos && lead > 0 && !multi && r.output.empty()) ins = src.substr(0, lead) + ins;   // keep indentation
    } else if (!r.output.empty()) {
        ins = nl + r.output + (r.hasValue && multi ? L"" : L"");
    } else if (multi) {
        ins = nl + r.value;
    } else {
        bool spaced = body.find(L' ') != std::wstring::npos && fragAt == std::wstring::npos;
        if (eq) { ins = (src.size() >= 2 && src[src.size() - 2] == L' ') ? L" " + r.value : r.value; }
        else ins = (spaced ? L" = " : L"=") + r.value;
    }
    d->text.BreakCoalesce();
    d->anchor = ia; d->caret = ib;
    EditorInsert(d, ins);
    if (!replace && !r.output.empty()) {   // select the generated text so it can be moved / deleted at once
        np::Pos e = d->caret; np::Pos s = d->text.PosAt(d->text.Offset(e) - (ins.size() - nl.size()));
        EditorSelect(d, s, e);
    }
    std::wstring shown = r.output.empty() ? r.value : (std::to_wstring(std::count(r.output.begin(), r.output.end(), L'\n') + 1) + L" line(s) generated");
    if (shown.size() > 80) shown = shown.substr(0, 80) + L"…";
    SetStatus(L"= " + shown);
}

void EvaluateReset() { s_eng.reset(); SetStatus(L"Evaluate: variables cleared"); }

} // namespace app
