#include "regex.h"
#include <algorithm>
#include <chrono>
#include <cwctype>

namespace np {

enum Op : uint8_t { CHAR, ANY, CLASS, MATCH, SPLIT, JMP, SAVE, BOL, EOL, WB, NWB };

namespace {
struct Node {
    enum K { LIT, DOT, CLS, CAT, ALT, REP, GRP, ABOL, AEOL, AWB, ANWB, EMPTY } k = EMPTY;
    wchar_t ch = 0; int cls = -1; std::vector<Node> kids; int mn = 0, mx = 0; bool greedy = true; int cap = -1;
};
long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool IsW(wchar_t c) { return c == L'_' || iswalnum(c); }
}

struct ReCompiler {
    Regex& r; const std::wstring& p; size_t i = 0; std::wstring err; int caps = 1;
    ReCompiler(Regex& rr, const std::wstring& pp) : r(rr), p(pp) {}
    bool More() const { return i < p.size(); }
    bool Fail(const std::wstring& m) { if (err.empty()) err = m; return false; }

    using Ranges = std::vector<std::pair<wchar_t, wchar_t>>;
    static Ranges Complement(Ranges in) {
        std::sort(in.begin(), in.end());
        Ranges out; unsigned next = 0;
        for (auto& rg : in) { if (rg.first > next) out.push_back({ (wchar_t)next, (wchar_t)(rg.first - 1) }); next = std::max<unsigned>(next, (unsigned)rg.second + 1); }
        if (next <= 0xFFFF) out.push_back({ (wchar_t)next, (wchar_t)0xFFFF });
        return out;
    }
    static Ranges Predef(wchar_t c) {
        Ranges d{ { L'0', L'9' } };
        Ranges w{ { L'0', L'9' }, { L'A', L'Z' }, { L'a', L'z' }, { L'_', L'_' } };
        Ranges s{ { L'\t', L'\r' }, { L' ', L' ' }, { 0xA0, 0xA0 }, { 0x1680, 0x1680 }, { 0x2000, 0x200A }, { 0x2028, 0x2029 }, { 0x202F, 0x202F }, { 0x205F, 0x205F }, { 0x3000, 0x3000 }, { 0xFEFF, 0xFEFF } };
        switch (c) { case L'd': return d; case L'D': return Complement(d); case L'w': return w; case L'W': return Complement(w); case L's': return s; case L'S': return Complement(s); }
        return {};
    }
    int AddClass(const Ranges& rg, bool neg) { r.classes_.push_back(rg); r.classNeg_.push_back(neg); return (int)r.classes_.size() - 1; }
    bool Hex(int digits, wchar_t& out) {
        if (i + digits > p.size()) return false;
        unsigned v = 0;
        for (int k = 0; k < digits; ++k) { wchar_t c = p[i + k]; if (!iswxdigit(c)) return false; v = v * 16 + (iswdigit(c) ? c - L'0' : (towlower(c) - L'a' + 10)); }
        i += digits; out = (wchar_t)v; return true;
    }
    // escape after '\' ; returns 0 literal (in ch), 1 class (in rg), 2 assertion (b/B in ch), -1 error
    int Escape(wchar_t& ch, Ranges& rg) {
        if (!More()) { Fail(L"pattern ends with a backslash"); return -1; }
        wchar_t c = p[i++];
        switch (c) {
        case L'd': case L'D': case L'w': case L'W': case L's': case L'S': rg = Predef(c); return 1;
        case L'b': case L'B': ch = c; return 2;
        case L't': ch = L'\t'; return 0; case L'n': ch = L'\n'; return 0; case L'r': ch = L'\r'; return 0;
        case L'f': ch = L'\f'; return 0; case L'v': ch = L'\v'; return 0; case L'0': ch = 0; return 0;
        case L'x': if (!Hex(2, ch)) { Fail(L"\\x needs two hex digits"); return -1; } return 0;
        case L'u': if (!Hex(4, ch)) { Fail(L"\\u needs four hex digits"); return -1; } return 0;
        }
        if (c >= L'1' && c <= L'9') { Fail(L"backreferences (\\1...) are not supported"); return -1; }
        ch = c; return 0;
    }
    bool ParseClass(Node& n) {
        bool neg = false; Ranges rg;
        if (More() && p[i] == L'^') { neg = true; ++i; }
        bool first = true;
        while (More() && (p[i] != L']' || first)) {
            first = false;
            wchar_t lo;
            if (p[i] == L'\\') {
                ++i; Ranges pre; int k = Escape(lo, pre);
                if (k < 0) return false;
                if (k == 1) { rg.insert(rg.end(), pre.begin(), pre.end()); continue; }
                if (k == 2) lo = lo == L'b' ? L'\b' : L'B';
            } else lo = p[i++];
            wchar_t hi = lo;
            if (i + 1 < p.size() && p[i] == L'-' && p[i + 1] != L']') {
                ++i;
                if (p[i] == L'\\') { ++i; Ranges pre; int k = Escape(hi, pre); if (k != 0) return Fail(L"invalid range in character class"); }
                else hi = p[i++];
                if (hi < lo) return Fail(L"character class range is out of order");
            }
            rg.push_back({ lo, hi });
        }
        if (!More()) return Fail(L"missing ]");
        ++i;
        n.k = Node::CLS; n.cls = AddClass(rg, neg);
        return true;
    }
    bool ParseAtom(Node& n) {
        wchar_t c = p[i];
        if (c == L'(') {
            ++i;
            int cap = -1;
            if (i + 1 < p.size() && p[i] == L'?') {
                if (p[i + 1] == L':') i += 2;
                else return Fail(L"lookahead / lookbehind are not supported");
            } else cap = caps++;
            Node inner; if (!ParseAlt(inner)) return false;
            if (!More() || p[i] != L')') return Fail(L"missing )");
            ++i;
            n.k = Node::GRP; n.cap = cap; n.kids.push_back(std::move(inner));
            return true;
        }
        if (c == L'[') { ++i; return ParseClass(n); }
        if (c == L'.') { ++i; n.k = Node::DOT; return true; }
        if (c == L'^') { ++i; n.k = Node::ABOL; return true; }
        if (c == L'$') { ++i; n.k = Node::AEOL; return true; }
        if (c == L'*' || c == L'+' || c == L'?') return Fail(L"nothing to repeat before " + std::wstring(1, c));
        if (c == L'\\') {
            ++i; wchar_t ch; Ranges rg; int k = Escape(ch, rg);
            if (k < 0) return false;
            if (k == 1) { n.k = Node::CLS; n.cls = AddClass(rg, false); return true; }
            if (k == 2) { n.k = ch == L'b' ? Node::AWB : Node::ANWB; return true; }
            n.k = Node::LIT; n.ch = ch; return true;
        }
        ++i; n.k = Node::LIT; n.ch = c; return true;
    }
    bool ParseCount(int& mn, int& mx) {   // at '{'; leaves i unchanged when it is not a quantifier
        size_t j = i + 1; auto num = [&](int& v) { size_t s = j; v = 0; while (j < p.size() && iswdigit(p[j]) && j - s < 6) v = v * 10 + (p[j++] - L'0'); return j > s; };
        if (!num(mn)) return false;
        mx = mn;
        if (j < p.size() && p[j] == L',') { ++j; if (j < p.size() && p[j] == L'}') mx = -1; else if (!num(mx)) return false; }
        if (j >= p.size() || p[j] != L'}') return false;
        i = j + 1; return true;
    }
    bool ParseCat(Node& n) {
        n.k = Node::CAT;
        while (More() && p[i] != L'|' && p[i] != L')') {
            Node a; if (!ParseAtom(a)) return false;
            for (;;) {
                if (!More()) break;
                int mn, mx; wchar_t q = p[i];
                if (q == L'*') { mn = 0; mx = -1; ++i; }
                else if (q == L'+') { mn = 1; mx = -1; ++i; }
                else if (q == L'?') { mn = 0; mx = 1; ++i; }
                else if (q == L'{' && ParseCount(mn, mx)) {}
                else break;
                if (mx >= 0 && mx < mn) return Fail(L"{n,m} with m < n");
                if (mn > 1000 || mx > 1000) return Fail(L"repeat counts above 1000 are not supported");
                if (a.k == Node::ABOL || a.k == Node::AEOL || a.k == Node::AWB || a.k == Node::ANWB) return Fail(L"an anchor cannot be repeated");
                Node rep; rep.k = Node::REP; rep.mn = mn; rep.mx = mx;
                if (More() && p[i] == L'?') { rep.greedy = false; ++i; }
                rep.kids.push_back(std::move(a)); a = std::move(rep);
            }
            n.kids.push_back(std::move(a));
        }
        return true;
    }
    bool ParseAlt(Node& n) {
        Node first; if (!ParseCat(first)) return false;
        if (!More() || p[i] != L'|') { n = std::move(first); return true; }
        n.k = Node::ALT; n.kids.push_back(std::move(first));
        while (More() && p[i] == L'|') { ++i; Node b; if (!ParseCat(b)) return false; n.kids.push_back(std::move(b)); }
        return true;
    }

    std::vector<Regex::Inst>& P() { return r.prog_; }
    int Emit(Op op, int x = 0, int y = 0) { P().push_back({ (uint8_t)op, x, y }); return (int)P().size() - 1; }
    bool Gen(const Node& n) {
        if (P().size() > 200000) return Fail(L"pattern is too large");
        switch (n.k) {
        case Node::LIT: Emit(CHAR, r.icase_ ? (int)towlower(n.ch) : (int)n.ch); break;
        case Node::DOT: Emit(ANY); break;
        case Node::CLS: Emit(CLASS, n.cls); break;
        case Node::ABOL: Emit(BOL); break;
        case Node::AEOL: Emit(EOL); break;
        case Node::AWB: Emit(WB); break;
        case Node::ANWB: Emit(NWB); break;
        case Node::EMPTY: break;
        case Node::CAT: for (auto& k : n.kids) if (!Gen(k)) return false; break;
        case Node::GRP:
            if (n.cap >= 0) Emit(SAVE, 2 * n.cap);
            if (!Gen(n.kids[0])) return false;
            if (n.cap >= 0) Emit(SAVE, 2 * n.cap + 1);
            break;
        case Node::ALT: {
            std::vector<int> ends;
            for (size_t k = 0; k < n.kids.size(); ++k) {
                if (k + 1 < n.kids.size()) {
                    int sp = Emit(SPLIT);
                    P()[sp].x = sp + 1;
                    if (!Gen(n.kids[k])) return false;
                    ends.push_back(Emit(JMP));
                    P()[sp].y = (int)P().size();
                } else if (!Gen(n.kids[k])) return false;
            }
            for (int e : ends) P()[e].x = (int)P().size();
            break; }
        case Node::REP: {
            const Node& b = n.kids[0];
            for (int k = 0; k < n.mn; ++k) if (!Gen(b)) return false;
            if (n.mx < 0) {
                int L = Emit(SPLIT);
                if (!Gen(b)) return false;
                Emit(JMP, L);
                int out = (int)P().size();
                P()[L].x = n.greedy ? L + 1 : out; P()[L].y = n.greedy ? out : L + 1;
            } else {
                std::vector<int> splits;
                for (int k = n.mn; k < n.mx; ++k) {
                    int sp = Emit(SPLIT); splits.push_back(sp);
                    P()[sp].x = sp + 1;   // body (patched for lazy below)
                    if (!Gen(b)) return false;
                }
                int out = (int)P().size();
                for (int sp : splits) { if (n.greedy) P()[sp].y = out; else { P()[sp].y = sp + 1; P()[sp].x = out; } }
            }
            break; }
        }
        return true;
    }
};

bool Regex::Compile(const std::wstring& pattern, bool ignoreCase, std::wstring* error) {
    prog_.clear(); classes_.clear(); classNeg_.clear(); icase_ = ignoreCase; anchoredStart_ = false;
    ReCompiler c(*this, pattern);
    Node root;
    bool ok = c.ParseAlt(root);
    if (ok && c.More()) ok = c.Fail(L"unmatched )");
    if (ok) {
        ncap_ = c.caps * 2;
        c.Emit(SAVE, 0);
        ok = c.Gen(root);
        c.Emit(SAVE, 1);
        c.Emit(MATCH);
        if (ok) anchoredStart_ = prog_.size() > 1 && prog_[1].op == BOL;
    }
    if (!ok) { prog_.clear(); if (error) *error = c.err; return false; }
    return true;
}

int Regex::Run(const wchar_t* t, size_t n, size_t start, size_t limitStart, bool anchoredEnd, size_t endAt, ReMatch& m, long long deadline) const {
    const int P = (int)prog_.size(), NC = ncap_;
    struct List { std::vector<int> pcs; std::vector<long long> caps; std::vector<uint32_t> mark; uint32_t gen = 0; };
    List a, b; List* cl = &a; List* nl = &b;
    for (List* l : { cl, nl }) { l->mark.assign(P, 0); l->pcs.reserve(P); l->caps.reserve((size_t)P * NC); }
    uint32_t genCounter = 1;
    auto fresh = [&](List* l) { l->pcs.clear(); l->caps.clear(); l->gen = ++genCounter; };
    std::vector<long long> cur(NC, -1);
    struct Fr { int pc; int slot; long long old; };
    std::vector<Fr> stack;
    auto inClass = [&](int ci, wchar_t c) {
        auto test = [&](wchar_t x) { for (auto& r : classes_[ci]) if (x >= r.first && x <= r.second) return true; return false; };
        bool in = test(c) || (icase_ && (test((wchar_t)towlower(c)) || test((wchar_t)towupper(c))));
        return in != (bool)classNeg_[ci];
    };
    auto add = [&](List* l, int pc0, const long long* caps, size_t pos) {
        std::copy(caps, caps + NC, cur.begin());
        stack.clear(); stack.push_back({ pc0, -1, 0 });
        while (!stack.empty()) {
            Fr f = stack.back(); stack.pop_back();
            if (f.slot >= 0) { cur[f.slot] = f.old; continue; }   // restore after a SAVE subtree
            int pc = f.pc;
            if (l->mark[pc] == l->gen) continue;
            l->mark[pc] = l->gen;
            const Inst& in = prog_[pc];
            switch (in.op) {
            case JMP: stack.push_back({ in.x, -1, 0 }); break;
            case SPLIT: stack.push_back({ in.y, -1, 0 }); stack.push_back({ in.x, -1, 0 }); break;
            case SAVE: stack.push_back({ 0, in.x, cur[in.x] }); cur[in.x] = (long long)pos; stack.push_back({ pc + 1, -1, 0 }); break;
            case BOL: if (pos == 0 || t[pos - 1] == L'\n') stack.push_back({ pc + 1, -1, 0 }); break;
            case EOL: if (pos >= n || t[pos] == L'\n' || t[pos] == L'\r') stack.push_back({ pc + 1, -1, 0 }); break;
            case WB: case NWB: {
                bool w1 = pos > 0 && IsW(t[pos - 1]), w2 = pos < n && IsW(t[pos]);
                if ((w1 != w2) == (in.op == WB)) stack.push_back({ pc + 1, -1, 0 });
                break; }
            default: l->pcs.push_back(pc); l->caps.insert(l->caps.end(), cur.begin(), cur.end()); break;
            }
        }
    };
    std::vector<long long> init(NC, -1);
    bool matched = false;
    std::vector<long long> best;
    // literal first character -> skip ahead with a scan while no thread is alive
    int firstCh = prog_.size() > 1 && prog_[1].op == CHAR ? prog_[1].x : -1;
    fresh(cl);
    for (size_t pos = start;; ++pos) {
        if (deadline && (pos & 4095) == 0 && NowMs() > deadline) return -1;
        if (!matched && pos <= limitStart) {
            if (cl->pcs.empty()) {   // nothing alive: jump to the next plausible start position
                if (anchoredStart_) {
                    while (pos <= n && pos <= limitStart && !(pos == 0 || t[pos - 1] == L'\n')) ++pos;
                } else if (firstCh >= 0) {
                    while (pos < n && pos <= limitStart && (icase_ ? (int)towlower(t[pos]) : (int)t[pos]) != firstCh) {
                        ++pos;
                        if (deadline && (pos & 65535) == 0 && NowMs() > deadline) return -1;
                    }
                    if (pos >= n) break;
                }
                if (pos > limitStart || pos > n) break;
            }
            if (cl->pcs.empty()) fresh(cl);   // new generation: marks from a dead start thread must not block this one
            add(cl, 0, init.data(), pos);
        }
        if (cl->pcs.empty()) { if (matched || pos >= limitStart || pos >= n) break; continue; }   // start thread died in an assertion
        fresh(nl);
        wchar_t c = pos < n ? t[pos] : 0;
        wchar_t cf = icase_ ? (wchar_t)towlower(c) : c;
        for (size_t k = 0; k < cl->pcs.size(); ++k) {
            int pc = cl->pcs[k]; const Inst& in = prog_[pc];
            const long long* caps = &cl->caps[k * NC];
            bool ok = false;
            switch (in.op) {
            case CHAR: ok = pos < n && (int)cf == in.x; break;
            case ANY: ok = pos < n && c != L'\n'; break;
            case CLASS: ok = pos < n && inClass(in.x, c); break;
            case MATCH:
                if (anchoredEnd && pos != endAt) break;
                matched = true; best.assign(caps, caps + NC);
                k = cl->pcs.size();   // lower-priority threads are cut
                continue;
            }
            if (ok) add(nl, pc + 1, caps, pos + 1);
        }
        std::swap(cl, nl);
        if (pos >= n) break;
    }
    if (!matched) return 0;
    m.groups.assign(NC / 2, { -1, -1 });
    for (int g = 0; g < NC / 2; ++g) if (best[2 * g] >= 0 && best[2 * g + 1] >= 0) m.groups[g] = { best[2 * g], best[2 * g + 1] };
    m.off = (size_t)m.groups[0].first; m.len = (size_t)(m.groups[0].second - m.groups[0].first);
    return 1;
}

int Regex::Search(const wchar_t* text, size_t n, size_t start, ReMatch& m, long long deadlineMs) const {
    if (prog_.empty() || start > n) return 0;
    return Run(text, n, start, n, false, 0, m, deadlineMs);
}
bool Regex::MatchExact(const wchar_t* text, size_t n, size_t b, size_t e, ReMatch& m) const {
    if (prog_.empty() || b > e || e > n) return false;
    return Run(text, n, b, b, true, e, m, 0) == 1;
}

std::wstring Regex::Format(const std::wstring& r, const wchar_t* t, size_t n, const ReMatch& m) {
    std::wstring o;
    for (size_t i = 0; i < r.size(); ++i) {
        if (r[i] != L'$' || i + 1 >= r.size()) { o += r[i]; continue; }
        wchar_t c = r[i + 1];
        if (c == L'$') { o += L'$'; ++i; continue; }
        if (c == L'&') { o.append(t + m.off, m.len); ++i; continue; }
        if (c == L'`') { o.append(t, m.off); ++i; continue; }
        if (c == L'\'') { size_t e = m.off + m.len; if (e < n) o.append(t + e, n - e); ++i; continue; }
        if (iswdigit(c)) {
            int g = c - L'0'; size_t j = i + 2;
            if (j < r.size() && iswdigit(r[j]) && g * 10 + (r[j] - L'0') < (int)m.groups.size()) { g = g * 10 + (r[j] - L'0'); ++j; }
            if (g > 0 && g < (int)m.groups.size()) {
                if (m.groups[g].first >= 0) o.append(t + m.groups[g].first, (size_t)(m.groups[g].second - m.groups[g].first));
                i = j - 1; continue;
            }
        }
        o += L'$';
    }
    return o;
}

} // namespace np
