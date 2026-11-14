#include "mdlive.h"
#include "markdown.h"
#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace np {

static bool IsBlank(const std::wstring& s) { for (wchar_t c : s) if (c != L' ' && c != L'\t') return false; return true; }
static int Indent(const std::wstring& s) { int i = 0; while (i < (int)s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i; return i; }
static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}
static bool IsFence(const std::wstring& s) {
    int i = Indent(s); if (i > 3) return false;
    return s.compare(i, 3, L"```") == 0 || s.compare(i, 3, L"~~~") == 0;
}
static bool IsRule(const std::wstring& s) {
    std::wstring t; for (wchar_t c : s) if (c != L' ' && c != L'\t') t += c;
    if (t.size() < 3) return false;
    wchar_t c = t[0];
    if (c != L'-' && c != L'*' && c != L'_') return false;
    for (wchar_t x : t) if (x != c) return false;
    return Indent(s) <= 3;
}
std::vector<std::wstring> SplitTableRow(const std::wstring& line) {
    std::wstring s = Trim(line);
    if (!s.empty() && s[0] == L'|') s.erase(0, 1);
    if (!s.empty() && s.back() == L'|' && (s.size() < 2 || s[s.size() - 2] != L'\\')) s.pop_back();
    std::vector<std::wstring> cells;
    std::wstring cur; bool code = false;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < s.size() && s[i + 1] == L'|') { cur += L"\\|"; ++i; continue; }
        if (c == L'`') code = !code;
        if (c == L'|' && !code) { cells.push_back(Trim(cur)); cur.clear(); continue; }
        cur += c;
    }
    cells.push_back(Trim(cur));
    return cells;
}
static bool IsDelimRow(const std::wstring& s, std::vector<int>* align) {
    if (s.find(L'-') == std::wstring::npos) return false;
    auto cells = SplitTableRow(s);
    if (cells.empty()) return false;
    if (s.find(L'|') == std::wstring::npos && cells.size() < 2) return false;
    std::vector<int> al;
    for (auto& c : cells) {
        if (c.empty()) return false;
        bool l = c.front() == L':', r = c.back() == L':';
        size_t a = l ? 1 : 0, b = c.size() - (r ? 1 : 0);
        if (b <= a) return false;
        for (size_t k = a; k < b; ++k) if (c[k] != L'-') return false;
        al.push_back(l && r ? 1 : r ? 2 : l ? 3 : 0);
    }
    if (align) *align = al;
    return true;
}

void ScanLive(const TextDoc& doc, std::vector<LiveLine>& out) {
    int n = doc.LineCount();
    out.assign(n, LiveLine{});
    bool fence = false;
    for (int i = 0; i < n; ++i) {
        const std::wstring& s = doc.Line(i);
        LiveLine& L = out[i];
        if (fence) { if (IsFence(s)) { L.kind = LiveKind::Fence; fence = false; } else L.kind = LiveKind::Code; continue; }
        if (IsFence(s)) { L.kind = LiveKind::Fence; fence = true; continue; }
        if (IsBlank(s)) { L.kind = LiveKind::Blank; continue; }
        int ind = Indent(s);
        // table: header + delimiter row
        if (s.find(L'|') != std::wstring::npos && i + 1 < n && IsDelimRow(doc.Line(i + 1), nullptr)
            && SplitTableRow(s).size() == SplitTableRow(doc.Line(i + 1)).size()) {
            int last = i + 1;
            while (last + 1 < n && !IsBlank(doc.Line(last + 1)) && doc.Line(last + 1).find(L'|') != std::wstring::npos) ++last;
            L.kind = LiveKind::TableHead; L.tableEnd = last;
            for (int k = i + 1; k <= last; ++k) { out[k].kind = LiveKind::TableHidden; out[k].tableEnd = last; }
            i = last; continue;
        }
        if (ind <= 3 && s[ind] == L'#') {
            int h = 0; while (ind + h < (int)s.size() && s[ind + h] == L'#') ++h;
            if (h <= 6 && (ind + h == (int)s.size() || s[ind + h] == L' ')) {
                L.kind = LiveKind::Heading; L.level = (uint8_t)h;
                int p = ind + h; while (p < (int)s.size() && s[p] == L' ') ++p;
                L.prefix = p; continue;
            }
        }
        if (IsRule(s)) { L.kind = LiveKind::Rule; L.prefix = (int)s.size(); continue; }
        if (ind < (int)s.size() && (s[ind] == L'-' || s[ind] == L'*' || s[ind] == L'+') && ind + 1 < (int)s.size() && s[ind + 1] == L' ') {
            L.markerAt = ind; L.level = (uint8_t)std::min(ind / 2, 6);
            if (ind + 5 < (int)s.size() && s[ind + 2] == L'[' && (s[ind + 3] == L' ' || s[ind + 3] == L'x' || s[ind + 3] == L'X') && s[ind + 4] == L']'
                && (ind + 5 == (int)s.size() || s[ind + 5] == L' ')) {
                L.kind = LiveKind::Task; L.checked = s[ind + 3] != L' '; L.prefix = std::min((int)s.size(), ind + 6);
            } else { L.kind = LiveKind::Bullet; L.prefix = ind + 2; }
            continue;
        }
        if (ind < (int)s.size() && iswdigit(s[ind])) {
            int p = ind; while (p < (int)s.size() && iswdigit(s[p]) && p - ind < 9) ++p;
            if (p < (int)s.size() && (s[p] == L'.' || s[p] == L')') && (p + 1 == (int)s.size() || s[p + 1] == L' ')) {
                L.kind = LiveKind::Ordered; L.markerAt = ind; L.prefix = std::min((int)s.size(), p + 2); L.level = (uint8_t)std::min(ind / 3, 6); continue;
            }
        }
        if (ind <= 3 && s[ind] == L'>') {
            L.kind = LiveKind::Quote; L.markerAt = ind; L.prefix = ind + 1 + (ind + 1 < (int)s.size() && s[ind + 1] == L' ' ? 1 : 0);
            continue;
        }
        L.kind = LiveKind::Para;
    }
}

static void Span(std::vector<LiveSpan>& o, int a, int len, LiveSpanKind k, const std::wstring& url = L"", const std::wstring& alt = L"", int arg = 0) {
    if (len <= 0) return;
    LiveSpan s; s.a = a; s.len = len; s.kind = k; s.url = url; s.alt = alt; s.arg = arg; o.push_back(std::move(s));
}

static void Inl(const std::wstring& s, int i, int n, std::vector<LiveSpan>& out) {
    while (i < n) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < n && wcschr(L"\\`*_{}[]()#+-.!|~<>", s[i + 1])) { Span(out, i, 1, LiveSpanKind::Escape); i += 2; continue; }
        if (c == L'`') {
            int t = 0; while (i + t < n && s[i + t] == L'`') ++t;
            size_t e = s.find(std::wstring(t, L'`'), i + t);
            if (e != std::wstring::npos && (int)e < n) {
                Span(out, i, t, LiveSpanKind::Hide); Span(out, i + t, (int)e - i - t, LiveSpanKind::Code); Span(out, (int)e, t, LiveSpanKind::Hide);
                i = (int)e + t; continue;
            }
            i += t; continue;
        }
        if ((c == L'*' || c == L'_') && i + 1 < n && s[i + 1] == c && i + 2 < n && !iswspace(s[i + 2])) {
            size_t e = s.find(std::wstring(2, c), i + 2);
            if (e != std::wstring::npos && (int)e < n && (int)e > i + 2 && !iswspace(s[e - 1])) {
                Span(out, i, 2, LiveSpanKind::Hide); Span(out, i + 2, (int)e - i - 2, LiveSpanKind::Strong);
                Inl(s, i + 2, (int)e, out); Span(out, (int)e, 2, LiveSpanKind::Hide);
                i = (int)e + 2; continue;
            }
        }
        if (c == L'~' && i + 1 < n && s[i + 1] == L'~') {
            size_t e = s.find(L"~~", i + 2);
            if (e != std::wstring::npos && (int)e < n && (int)e > i + 2) {
                Span(out, i, 2, LiveSpanKind::Hide); Span(out, i + 2, (int)e - i - 2, LiveSpanKind::Strike);
                Inl(s, i + 2, (int)e, out); Span(out, (int)e, 2, LiveSpanKind::Hide);
                i = (int)e + 2; continue;
            }
        }
        if ((c == L'*' || c == L'_') && i + 1 < n && !iswspace(s[i + 1]) && s[i + 1] != c && (c == L'*' || i == 0 || !iswalnum(s[i - 1]))) {
            int e = i + 1;
            while (e < n) {
                if (s[e] == c) {
                    if (e + 1 < n && s[e + 1] == c) { e += 2; continue; }
                    if (!iswspace(s[e - 1]) && (c == L'*' || e + 1 >= n || !iswalnum(s[e + 1]))) break;
                }
                ++e;
            }
            if (e < n) {
                Span(out, i, 1, LiveSpanKind::Hide); Span(out, i + 1, e - i - 1, LiveSpanKind::Em);
                Inl(s, i + 1, e, out); Span(out, e, 1, LiveSpanKind::Hide);
                i = e + 1; continue;
            }
        }
        if (c == L'[' || (c == L'!' && i + 1 < n && s[i + 1] == L'[')) {
            int lb = c == L'!' ? i + 1 : i;
            int depth = 0, rb = -1;
            for (int k = lb; k < n; ++k) { if (s[k] == L'[') ++depth; else if (s[k] == L']' && --depth == 0) { rb = k; break; } }
            if (rb >= 0 && rb + 1 < n && s[rb + 1] == L'(') {
                size_t rp = s.find(L')', rb + 2);
                if (rp != std::wstring::npos && (int)rp < n) {
                    std::wstring url = Trim(s.substr(rb + 2, rp - rb - 2));
                    if (!url.empty() && url[0] == L'<') { size_t gt = url.find(L'>'); url = url.substr(1, gt == std::wstring::npos ? std::wstring::npos : gt - 1); }
                    else { size_t sp = url.find(L' '); if (sp != std::wstring::npos) url.resize(sp); }
                    if (c == L'!') Span(out, i, (int)rp + 1 - i, LiveSpanKind::Image, url, s.substr(lb + 1, rb - lb - 1));
                    else {
                        Span(out, i, 1, LiveSpanKind::Hide); Span(out, i + 1, rb - i - 1, LiveSpanKind::Link, url);
                        Inl(s, i + 1, rb, out); Span(out, rb, (int)rp + 1 - rb, LiveSpanKind::Hide);
                    }
                    i = (int)rp + 1; continue;
                }
            }
        }
        if (c == L'<') {
            size_t e = s.find(L'>', i);
            if (e != std::wstring::npos && (int)e < n) {
                std::wstring u = s.substr(i + 1, e - i - 1);
                if ((u.rfind(L"http", 0) == 0 || u.rfind(L"file:", 0) == 0 || u.find(L'@') != std::wstring::npos) && u.find(L' ') == std::wstring::npos) {
                    Span(out, i, 1, LiveSpanKind::Hide); Span(out, i + 1, (int)u.size(), LiveSpanKind::Link, u); Span(out, (int)e, 1, LiveSpanKind::Hide);
                    i = (int)e + 1; continue;
                }
            }
        }
        if ((c == L'h' || c == L'f') && (i == 0 || !iswalnum(s[i - 1])) &&
            (s.compare(i, 7, L"http://") == 0 || s.compare(i, 8, L"https://") == 0 || s.compare(i, 8, L"file:///") == 0)) {
            int e = i; while (e < n && !iswspace(s[e]) && s[e] != L')' && s[e] != L'>') ++e;
            while (e > i && wcschr(L".,;:!?'\"", s[e - 1])) --e;
            Span(out, i, e - i, LiveSpanKind::Link, s.substr(i, e - i));
            i = e; continue;
        }
        ++i;
    }
}

void LiveSpans(const std::wstring& s, const LiveLine& L, std::vector<LiveSpan>& out) {
    out.clear();
    int n = (int)s.size();
    switch (L.kind) {
    case LiveKind::Heading: Span(out, 0, L.prefix, LiveSpanKind::Hide); break;
    case LiveKind::Bullet: Span(out, L.markerAt, 2, LiveSpanKind::Bullet, L"", L"", L.level); break;
    case LiveKind::Task: Span(out, L.markerAt, L.prefix - L.markerAt, LiveSpanKind::Task, L"", L"", L.checked); break;
    case LiveKind::Quote: Span(out, L.markerAt, L.prefix - L.markerAt, LiveSpanKind::Quote); break;
    case LiveKind::Rule: Span(out, 0, n, LiveSpanKind::Rule); return;
    case LiveKind::Code: case LiveKind::Fence: case LiveKind::Blank: case LiveKind::TableHead: case LiveKind::TableHidden: return;
    default: break;
    }
    // trailing "#"s of an ATX heading
    int end = n;
    if (L.kind == LiveKind::Heading) {
        int e = n; while (e > L.prefix && s[e - 1] == L' ') --e;
        int h = e; while (h > L.prefix && s[h - 1] == L'#') --h;
        if (h < e && h > L.prefix && s[h - 1] == L' ') { Span(out, h - 1, n - h + 1, LiveSpanKind::Hide); end = h - 1; }
    }
    Inl(s, L.prefix, end, out);
}

bool ParseLiveTable(const TextDoc& doc, int first, int last, LiveTable& t) {
    t = LiveTable{}; t.first = first; t.last = last;
    if (last < first + 1 || last >= doc.LineCount()) return false;
    if (!IsDelimRow(doc.Line(first + 1), &t.align)) return false;
    t.rows.push_back(SplitTableRow(doc.Line(first)));
    for (int i = first + 2; i <= last; ++i) t.rows.push_back(SplitTableRow(doc.Line(i)));
    size_t cols = 0; for (auto& r : t.rows) cols = std::max(cols, r.size());
    cols = std::max(cols, t.align.size());
    for (auto& r : t.rows) r.resize(cols);
    t.align.resize(cols, 0);
    return true;
}

static int DispW(const std::wstring& s) {
    int w = 0;
    for (wchar_t c : s) {
        if (c >= 0xDC00 && c <= 0xDFFF) continue;
        w += (c >= 0x1100 && (c <= 0x115F || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xD800 && c <= 0xDBFF))) ? 2 : 1;
    }
    return w;
}
std::wstring SerializeLiveTable(const LiveTable& t) {
    int cols = t.Cols();
    if (!cols) return L"";
    std::vector<int> w(cols, 3);
    for (auto& r : t.rows) for (int c = 0; c < cols && c < (int)r.size(); ++c) w[c] = std::max(w[c], DispW(r[c]));
    auto pad = [&](const std::wstring& v, int c) {
        int sp = w[c] - DispW(v);
        int al = c < (int)t.align.size() ? t.align[c] : 0;
        if (al == 2) return std::wstring(sp, L' ') + v;
        if (al == 1) return std::wstring(sp / 2, L' ') + v + std::wstring(sp - sp / 2, L' ');
        return v + std::wstring(sp, L' ');
    };
    std::wstring o;
    for (size_t r = 0; r < t.rows.size(); ++r) {
        o += L"|";
        for (int c = 0; c < cols; ++c) { std::wstring v = c < (int)t.rows[r].size() ? t.rows[r][c] : L""; o += L" " + pad(v, c) + L" |"; }
        o += L"\n";
        if (r == 0) {
            o += L"|";
            for (int c = 0; c < cols; ++c) {
                int al = c < (int)t.align.size() ? t.align[c] : 0;
                std::wstring d(w[c], L'-');
                if (al == 1) { d[0] = L':'; d.back() = L':'; } else if (al == 2) d.back() = L':'; else if (al == 3) d[0] = L':';
                o += L" " + d + L" |";
            }
            o += L"\n";
        }
    }
    if (!o.empty()) o.pop_back();
    return o;
}
std::wstring StripInline(const std::wstring& cell) {
    std::wstring s = RunsText(ParseInline(cell));
    size_t p; while ((p = s.find(L"\\|")) != std::wstring::npos) s.replace(p, 2, L"|");
    return s;
}

std::wstring PlainTitle(const std::wstring& line) {
    std::wstring s;
    // drop BBCode [tag], [tag=..], [/tag] and HTML <tag ...> / </tag>
    for (size_t i = 0; i < line.size(); ++i) {
        wchar_t c = line[i];
        if (c == L'[' || c == L'<') {
            wchar_t close = c == L'[' ? L']' : L'>';
            size_t e = line.find(close, i + 1);
            size_t k = i + 1; if (k < line.size() && line[k] == L'/') ++k;
            bool tag = e != std::wstring::npos && k < e && (iswalpha(line[k]) || line[k] == L'*');
            if (tag) { size_t j = k; while (j < e && (iswalnum(line[j]) || line[j] == L'*')) ++j; tag = j == e || line[j] == L'=' || line[j] == L' '; }
            if (tag && c == L'[' && i + 1 < line.size() && line.find(L"](", i) == e) tag = false;   // markdown link text
            if (tag) { i = e; continue; }
        }
        s += c;
    }
    size_t a = 0;
    while (a < s.size() && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'#' || s[a] == L'>')) ++a;
    if (a + 1 < s.size() && (s[a] == L'-' || s[a] == L'*' || s[a] == L'+') && s[a + 1] == L' ') a += 2;
    if (s.compare(a, 4, L"[ ] ") == 0 || s.compare(a, 4, L"[x] ") == 0) a += 4;
    std::wstring t = StripInline(s.substr(a));
    while (!t.empty() && (t.back() == L' ' || t.back() == L'\t')) t.pop_back();
    size_t b = t.find_first_not_of(L" \t\U0001F5BC");
    return b == std::wstring::npos ? L"" : t.substr(b);
}

} // namespace np
