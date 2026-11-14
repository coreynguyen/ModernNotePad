#include "markdown.h"
#include <algorithm>
#include <cwctype>
#include <cwchar>

namespace np {

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}
static int LeadSpaces(const std::wstring& s) {
    int w = 0; for (wchar_t c : s) { if (c == L' ') ++w; else if (c == L'\t') w += 4; else break; } return w;
}
std::wstring RunsText(const std::vector<MdRun>& r) { std::wstring o; for (auto& x : r) o += x.text; return o; }

static void Add(std::vector<MdRun>& out, const std::wstring& t, unsigned st, const std::wstring& url = L"") {
    if (t.empty()) return;
    if (!out.empty() && out.back().style == st && out.back().url == url) { out.back().text += t; return; }
    out.push_back({ t, st, url });
}

static void Inline(const std::wstring& s, size_t i, size_t n, unsigned st, std::vector<MdRun>& out) {
    std::wstring buf;
    auto flush = [&] { Add(out, buf, st); buf.clear(); };
    while (i < n) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < n && wcschr(L"\\`*_{}[]()#+-.!|~<>", s[i + 1])) { buf += s[i + 1]; i += 2; continue; }
        if (c == L'`') {
            size_t ticks = 0; while (i + ticks < n && s[i + ticks] == L'`') ++ticks;
            std::wstring fence(ticks, L'`');
            size_t e = s.find(fence, i + ticks);
            if (e != std::wstring::npos && e < n) {
                flush();
                Add(out, Trim(s.substr(i + ticks, e - i - ticks)), st | MS_CODE);
                i = e + ticks; continue;
            }
        }
        if ((c == L'*' || c == L'_') && i + 1 < n && s[i + 1] == c) {
            std::wstring m(2, c);
            size_t e = s.find(m, i + 2);
            if (e != std::wstring::npos && e < n && e > i + 2) { flush(); Inline(s, i + 2, e, st | MS_STRONG, out); i = e + 2; continue; }
        }
        if (c == L'~' && i + 1 < n && s[i + 1] == L'~') {
            size_t e = s.find(L"~~", i + 2);
            if (e != std::wstring::npos && e < n) { flush(); Inline(s, i + 2, e, st | MS_STRIKE, out); i = e + 2; continue; }
        }
        if ((c == L'*' || c == L'_') && i + 1 < n && !iswspace(s[i + 1]) && (c == L'*' || i == 0 || !iswalnum(s[i - 1]))) {
            size_t e = i + 1;
            while ((e = s.find(c, e)) != std::wstring::npos && e < n) {
                if (e + 1 < n && s[e + 1] == c) { e += 2; continue; }
                if (!iswspace(s[e - 1])) break;
                ++e;
            }
            if (e != std::wstring::npos && e < n) { flush(); Inline(s, i + 1, e, st | MS_EM, out); i = e + 1; continue; }
        }
        if (c == L'[' || (c == L'!' && i + 1 < n && s[i + 1] == L'[')) {
            size_t lb = c == L'!' ? i + 1 : i;
            int depth = 0; size_t rb = std::wstring::npos;
            for (size_t k = lb; k < n; ++k) { if (s[k] == L'[') ++depth; else if (s[k] == L']' && --depth == 0) { rb = k; break; } }
            if (rb != std::wstring::npos && rb + 1 < n && s[rb + 1] == L'(') {
                size_t rp = s.find(L')', rb + 2);
                if (rp != std::wstring::npos && rp < n) {
                    flush();
                    std::wstring url = Trim(s.substr(rb + 2, rp - rb - 2));
                    size_t sp = url.find(L' '); if (sp != std::wstring::npos) url.resize(sp);
                    std::vector<MdRun> inner;
                    Inline(s, lb + 1, rb, st | MS_LINK, inner);
                    if (c == L'!') Add(out, L"\U0001F5BC ", st | MS_LINK, url);   // image placeholder
                    for (auto& r : inner) { r.url = url; out.push_back(r); }
                    i = rp + 1; continue;
                }
            }
        }
        if (c == L'<') {
            size_t e = s.find(L'>', i);
            if (e != std::wstring::npos && e < n) {
                std::wstring u = s.substr(i + 1, e - i - 1);
                if (u.rfind(L"http", 0) == 0 || u.find(L'@') != std::wstring::npos) { flush(); Add(out, u, st | MS_LINK, u); i = e + 1; continue; }
            }
        }
        if ((s.compare(i, 7, L"http://") == 0 || s.compare(i, 8, L"https://") == 0) && !(st & MS_LINK)) {
            size_t e = i; while (e < n && !iswspace(s[e]) && s[e] != L')' && s[e] != L'>') ++e;
            while (e > i && wcschr(L".,;:!?", s[e - 1])) --e;
            flush(); std::wstring u = s.substr(i, e - i); Add(out, u, st | MS_LINK, u); i = e; continue;
        }
        buf += c; ++i;
    }
    flush();
}
std::vector<MdRun> ParseInline(const std::wstring& s) { std::vector<MdRun> o; Inline(s, 0, s.size(), 0, o); return o; }

static bool IsRule(const std::wstring& t) {
    if (t.size() < 3) return false;
    wchar_t m = t[0]; if (m != L'-' && m != L'*' && m != L'_') return false;
    int c = 0; for (wchar_t x : t) { if (x == m) ++c; else if (x != L' ') return false; }
    return c >= 3;
}
static std::vector<std::wstring> SplitRow(std::wstring t) {
    t = Trim(t);
    if (!t.empty() && t[0] == L'|') t.erase(0, 1);
    if (!t.empty() && t.back() == L'|') t.pop_back();
    std::vector<std::wstring> cells; std::wstring cur;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == L'\\' && i + 1 < t.size() && t[i + 1] == L'|') { cur += L'|'; ++i; continue; }
        if (t[i] == L'|') { cells.push_back(Trim(cur)); cur.clear(); } else cur += t[i];
    }
    cells.push_back(Trim(cur));
    return cells;
}
static bool IsDelimRow(const std::wstring& t, std::vector<int>& align) {
    if (t.find(L'-') == std::wstring::npos || t.find(L'|') == std::wstring::npos) return false;
    align.clear();
    for (auto& c : SplitRow(t)) {
        if (c.empty()) return false;
        for (wchar_t x : c) if (x != L'-' && x != L':' && x != L' ') return false;
        bool l = c.front() == L':', r = c.back() == L':';
        align.push_back(l && r ? 1 : r ? 2 : 0);
    }
    return true;
}

std::vector<MdBlock> ParseMarkdown(const std::wstring& text) {
    std::vector<std::wstring> L;
    { size_t s = 0; for (;;) { size_t e = text.find(L'\n', s); L.push_back(text.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s)); if (e == std::wstring::npos) break; s = e + 1; } }
    std::vector<MdBlock> out;
    size_t i = 0, n = L.size();
    auto listMarker = [](const std::wstring& s, size_t& contentAt, bool& ordered, int& num) -> bool {
        size_t k = 0; while (k < s.size() && (s[k] == L' ' || s[k] == L'\t')) ++k;
        if (k < s.size() && (s[k] == L'-' || s[k] == L'*' || s[k] == L'+') && k + 1 < s.size() && s[k + 1] == L' ') { contentAt = k + 2; ordered = false; return true; }
        size_t j = k; while (j < s.size() && iswdigit(s[j])) ++j;
        if (j > k && j - k < 10 && j + 1 < s.size() && (s[j] == L'.' || s[j] == L')') && s[j + 1] == L' ') { num = std::stoi(s.substr(k, j - k)); contentAt = j + 2; ordered = true; return true; }
        return false;
    };
    while (i < n) {
        const std::wstring& line = L[i];
        std::wstring t = Trim(line);
        int lead = LeadSpaces(line);
        if (t.empty()) { ++i; continue; }
        // fenced code
        if (lead < 4 && (t.rfind(L"```", 0) == 0 || t.rfind(L"~~~", 0) == 0)) {
            std::wstring fence = t.substr(0, 3);
            MdBlock b; b.kind = MdKind::Code; b.lang = Trim(t.substr(3)); b.srcLine = (int)i;
            ++i;
            while (i < n && Trim(L[i]).rfind(fence, 0) != 0) { if (!b.code.empty() || i > (size_t)b.srcLine + 1) b.code += L'\n'; b.code += L[i]; ++i; }
            if (i < n) ++i;
            out.push_back(std::move(b)); continue;
        }
        // indented code
        if (lead >= 4) {
            MdBlock b; b.kind = MdKind::Code; b.srcLine = (int)i;
            while (i < n && (LeadSpaces(L[i]) >= 4 || Trim(L[i]).empty())) {
                std::wstring x = L[i]; size_t cut = 0; int w = 0;
                while (cut < x.size() && w < 4) { w += x[cut] == L'\t' ? 4 : 1; ++cut; }
                if (!b.code.empty()) b.code += L'\n';
                b.code += x.substr(cut);
                ++i;
            }
            while (!b.code.empty() && b.code.back() == L'\n') b.code.pop_back();
            out.push_back(std::move(b)); continue;
        }
        if (t[0] == L'#') {
            int lv = 0; while (lv < (int)t.size() && t[lv] == L'#') ++lv;
            if (lv <= 6 && (lv == (int)t.size() || t[lv] == L' ')) {
                std::wstring h = Trim(t.substr(lv));
                while (!h.empty() && h.back() == L'#') h.pop_back();
                MdBlock b; b.kind = MdKind::Heading; b.level = lv; b.runs = ParseInline(Trim(h)); b.srcLine = (int)i;
                out.push_back(std::move(b)); ++i; continue;
            }
        }
        if (IsRule(t)) { MdBlock b; b.kind = MdKind::Rule; b.srcLine = (int)i; out.push_back(b); ++i; continue; }
        if (t[0] == L'>') {
            MdBlock b; b.kind = MdKind::Quote; b.srcLine = (int)i; b.level = 1;
            std::wstring acc;
            while (i < n && !Trim(L[i]).empty() && Trim(L[i])[0] == L'>') {
                std::wstring x = Trim(L[i]).substr(1);
                int depth = 1; while (!x.empty() && (x[0] == L'>' || x[0] == L' ')) { if (x[0] == L'>') ++depth; x.erase(0, 1); }
                b.level = std::max(b.level, depth);
                if (!acc.empty()) acc += L' ';
                acc += x; ++i;
            }
            b.runs = ParseInline(acc);
            out.push_back(std::move(b)); continue;
        }
        // table
        std::vector<int> align;
        if (t.find(L'|') != std::wstring::npos && i + 1 < n && IsDelimRow(Trim(L[i + 1]), align)) {
            MdBlock b; b.kind = MdKind::Table; b.align = align; b.srcLine = (int)i;
            auto addRow = [&](const std::wstring& r) {
                std::vector<std::vector<MdRun>> row;
                for (auto& c : SplitRow(r)) row.push_back(ParseInline(c));
                row.resize(align.size());
                b.rows.push_back(std::move(row));
            };
            addRow(t); i += 2;
            while (i < n && !Trim(L[i]).empty() && L[i].find(L'|') != std::wstring::npos) { addRow(L[i]); ++i; }
            out.push_back(std::move(b)); continue;
        }
        size_t at; bool ord; int num = 0;
        if (listMarker(line, at, ord, num)) {
            MdBlock b; b.kind = MdKind::ListItem; b.ordered = ord; b.number = num; b.level = lead / 2; b.srcLine = (int)i;
            std::wstring content = line.substr(at);
            if (content.rfind(L"[ ] ", 0) == 0) { b.task = 0; content = content.substr(4); }
            else if (content.rfind(L"[x] ", 0) == 0 || content.rfind(L"[X] ", 0) == 0) { b.task = 1; content = content.substr(4); }
            ++i;
            // lazy continuation lines
            while (i < n && !Trim(L[i]).empty()) {
                size_t a2; bool o2; int n2;
                std::wstring tt = Trim(L[i]);
                if (listMarker(L[i], a2, o2, n2) || tt[0] == L'#' || tt[0] == L'>' || tt.rfind(L"```", 0) == 0 || IsRule(tt)) break;
                content += L' '; content += tt; ++i;
            }
            b.runs = ParseInline(Trim(content));
            out.push_back(std::move(b)); continue;
        }
        // paragraph (+ setext heading)
        MdBlock b; b.kind = MdKind::Para; b.srcLine = (int)i;
        std::wstring acc = t; ++i;
        bool hardBreak = line.size() >= 2 && line.compare(line.size() - 2, 2, L"  ") == 0;
        while (i < n) {
            std::wstring tt = Trim(L[i]);
            if (tt.empty()) break;
            if (!tt.empty() && (tt.find_first_not_of(L'=') == std::wstring::npos || (tt.find_first_not_of(L'-') == std::wstring::npos && tt.size() >= 2))) {
                b.kind = MdKind::Heading; b.level = tt[0] == L'=' ? 1 : 2; ++i; break;
            }
            size_t a2; bool o2; int n2;
            if (tt[0] == L'#' || tt[0] == L'>' || tt.rfind(L"```", 0) == 0 || IsRule(tt) || listMarker(L[i], a2, o2, n2)) break;
            acc += hardBreak ? L"\n" : L" ";
            acc += tt;
            hardBreak = L[i].size() >= 2 && L[i].compare(L[i].size() - 2, 2, L"  ") == 0;
            ++i;
        }
        b.runs = ParseInline(acc);
        out.push_back(std::move(b));
    }
    return out;
}

} // namespace np
