#include "markup.h"
#include "markdown.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <functional>

namespace np {

const wchar_t* DialectName(Dialect d) { return d == Dialect::Markdown ? L"Markdown" : d == Dialect::BBCode ? L"BBCode" : L"HTML"; }

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}
static std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
static std::vector<std::wstring> Lines(const std::wstring& s) {
    std::vector<std::wstring> o; size_t a = 0;
    for (;;) { size_t e = s.find(L'\n', a); o.push_back(s.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a)); if (e == std::wstring::npos) break; a = e + 1; }
    return o;
}
static std::wstring Join(const std::vector<std::wstring>& v, const wchar_t* sep = L"\n") {
    std::wstring o; for (size_t i = 0; i < v.size(); ++i) { if (i) o += sep; o += v[i]; } return o;
}
static bool Starts(const std::wstring& s, const std::wstring& p) { return s.compare(0, p.size(), p) == 0; }
static bool Ends(const std::wstring& s, const std::wstring& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
static std::wstring HtmlEsc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) { switch (c) { case L'<': o += L"&lt;"; break; case L'>': o += L"&gt;"; break; case L'&': o += L"&amp;"; break; case L'"': o += L"&quot;"; break; default: o += c; } }
    return o;
}

// ------------------------------------------------------------------ inline wrap
static void Markers(Dialect d, Inline k, const std::wstring& url, std::wstring& open, std::wstring& close) {
    switch (d) {
    case Dialect::Markdown:
        switch (k) {
        case Inline::Bold: open = close = L"**"; break; case Inline::Italic: open = close = L"*"; break;
        case Inline::Strike: open = close = L"~~"; break; case Inline::Code: open = close = L"`"; break;
        case Inline::Underline: open = L"<u>"; close = L"</u>"; break;
        case Inline::Link: open = L"["; close = L"](" + url + L")"; break;
        } break;
    case Dialect::BBCode:
        switch (k) {
        case Inline::Bold: open = L"[b]"; close = L"[/b]"; break; case Inline::Italic: open = L"[i]"; close = L"[/i]"; break;
        case Inline::Strike: open = L"[strike]"; close = L"[/strike]"; break; case Inline::Underline: open = L"[u]"; close = L"[/u]"; break;
        case Inline::Code: open = L"[code]"; close = L"[/code]"; break;
        case Inline::Link: open = L"[url=" + url + L"]"; close = L"[/url]"; break;
        } break;
    case Dialect::Html:
        switch (k) {
        case Inline::Bold: open = L"<strong>"; close = L"</strong>"; break; case Inline::Italic: open = L"<em>"; close = L"</em>"; break;
        case Inline::Strike: open = L"<s>"; close = L"</s>"; break; case Inline::Underline: open = L"<u>"; close = L"</u>"; break;
        case Inline::Code: open = L"<code>"; close = L"</code>"; break;
        case Inline::Link: open = L"<a href=\"" + url + L"\">"; close = L"</a>"; break;
        } break;
    }
}
MarkupEdit WrapInline(Dialect d, Inline k, const std::wstring& sel, const std::wstring& url) {
    std::wstring o, c; Markers(d, k, url, o, c);
    MarkupEdit e;
    // unwrap if already wrapped (link: any url)
    if (k != Inline::Link && sel.size() >= o.size() + c.size() && Starts(sel, o) && Ends(sel, c)) {
        bool ok = true;
        if (d == Dialect::Markdown && k == Inline::Italic && (Starts(sel, L"**") && !Starts(sel, L"***"))) ok = false;
        if (ok) { e.text = sel.substr(o.size(), sel.size() - o.size() - c.size()); e.selStart = 0; e.selEnd = (int)e.text.size(); return e; }
    }
    if (k == Inline::Link && d == Dialect::Markdown && Starts(sel, L"[")) {
        size_t m = sel.find(L"](");
        if (m != std::wstring::npos && Ends(sel, L")")) { e.text = sel.substr(1, m - 1); e.selEnd = (int)e.text.size(); return e; }
    }
    e.text = o + sel + c;
    if (k == Inline::Link) {
        // select the URL placeholder when there is text, else the text slot
        if (sel.empty()) { e.selStart = e.selEnd = (int)o.size(); }
        else {
            size_t u = e.text.find(url, d == Dialect::Markdown ? o.size() + sel.size() : 0);
            e.selStart = (int)u; e.selEnd = (int)(u + url.size());
        }
    } else { e.selStart = (int)o.size(); e.selEnd = (int)(o.size() + sel.size()); }
    return e;
}

// ------------------------------------------------------------------ line ops
static std::wstring StripMdLinePrefix(const std::wstring& l) {
    size_t i = 0; while (i < l.size() && (l[i] == L' ' || l[i] == L'\t')) ++i;
    std::wstring s = l.substr(i);
    if (Starts(s, L"#")) { size_t k = 0; while (k < s.size() && s[k] == L'#') ++k; if (k < s.size() && s[k] == L' ') return s.substr(k + 1); }
    if (Starts(s, L"> ")) return s.substr(2);
    if (Starts(s, L"- [ ] ") || Starts(s, L"- [x] ") || Starts(s, L"- [X] ")) return s.substr(6);
    if (Starts(s, L"- ") || Starts(s, L"* ") || Starts(s, L"+ ")) return s.substr(2);
    size_t k = 0; while (k < s.size() && iswdigit(s[k])) ++k;
    if (k && k + 1 < s.size() && (s[k] == L'.' || s[k] == L')') && s[k + 1] == L' ') return s.substr(k + 2);
    return s;
}
static std::wstring StripTags(const std::wstring& s, wchar_t open, wchar_t close) {
    std::wstring o; bool in = false;
    for (wchar_t c : s) { if (c == open) in = true; else if (c == close && in) in = false; else if (!in) o += c; }
    return o;
}

std::wstring ApplyLineOp(Dialect d, LineOp op, const std::wstring& block, int level) {
    auto ls = Lines(block);
    level = std::max(1, std::min(6, level));
    if (d == Dialect::Markdown) {
        bool all = true;
        auto has = [&](const std::wstring& l) -> bool {
            switch (op) {
            case LineOp::Bullet: return Starts(l, L"- ") && !Starts(l, L"- [");
            case LineOp::Task: return Starts(l, L"- [ ] ") || Starts(l, L"- [x] ");
            case LineOp::Quote: return Starts(l, L"> ");
            case LineOp::Number: { size_t k = 0; while (k < l.size() && iswdigit(l[k])) ++k; return k && k + 1 < l.size() && l[k] == L'.'; }
            case LineOp::Heading: return Starts(l, std::wstring(level, L'#') + L" ");
            default: return false;
            }
        };
        for (auto& l : ls) if (!l.empty() && !has(l)) all = false;
        if (op == LineOp::CodeBlock) {
            if (ls.size() >= 2 && Starts(ls.front(), L"```") && Starts(ls.back(), L"```")) return Join(std::vector<std::wstring>(ls.begin() + 1, ls.end() - 1));
            return L"```\n" + block + L"\n```";
        }
        int n = 1;
        for (auto& l : ls) {
            if (l.empty() && ls.size() > 1) continue;
            std::wstring core = StripMdLinePrefix(l);
            if (all && op != LineOp::Paragraph) { l = core; continue; }   // toggle off
            switch (op) {
            case LineOp::Heading: l = std::wstring(level, L'#') + L" " + core; break;
            case LineOp::Paragraph: l = core; break;
            case LineOp::Bullet: l = L"- " + core; break;
            case LineOp::Number: l = std::to_wstring(n++) + L". " + core; break;
            case LineOp::Task: l = L"- [ ] " + core; break;
            case LineOp::Quote: l = L"> " + core; break;
            default: break;
            }
        }
        return Join(ls);
    }
    bool bb = d == Dialect::BBCode;
    std::wstring t = Trim(block);
    auto wrapped = [&](const std::wstring& o, const std::wstring& c) { return Starts(t, o) && Ends(t, c); };
    auto unwrap = [&](const std::wstring& o, const std::wstring& c) { return Trim(t.substr(o.size(), t.size() - o.size() - c.size())); };
    auto listItems = [&](const wchar_t* item, const wchar_t* itemClose) {
        std::wstring o;
        for (auto& l : ls) { std::wstring x = Trim(l); if (x.empty()) continue; o += L"\n"; o += item; o += x; o += itemClose; }
        return o;
    };
    switch (op) {
    case LineOp::Heading: {
        std::wstring o = bb ? L"[h" + std::to_wstring(std::min(level, 3)) + L"]" : L"<h" + std::to_wstring(level) + L">";
        std::wstring c = bb ? L"[/h" + std::to_wstring(std::min(level, 3)) + L"]" : L"</h" + std::to_wstring(level) + L">";
        if (wrapped(o, c)) return unwrap(o, c);
        std::vector<std::wstring> out;
        for (auto& l : ls) out.push_back(Trim(l).empty() ? l : o + Trim(l) + c);
        return Join(out); }
    case LineOp::Paragraph:
        if (bb) return ClearFormatting(d, block);
        if (wrapped(L"<p>", L"</p>")) return unwrap(L"<p>", L"</p>");
        return L"<p>" + t + L"</p>";
    case LineOp::Bullet:
        if (bb) { if (wrapped(L"[list]", L"[/list]")) return Trim(StripTags(unwrap(L"[list]", L"[/list]"), L'[', L']')); return L"[list]" + listItems(L"[*]", L"") + L"\n[/list]"; }
        if (wrapped(L"<ul>", L"</ul>")) return Trim(StripTags(unwrap(L"<ul>", L"</ul>"), L'<', L'>'));
        return L"<ul>" + listItems(L"  <li>", L"</li>") + L"\n</ul>";
    case LineOp::Number:
        if (bb) { if (wrapped(L"[olist]", L"[/olist]")) return Trim(StripTags(unwrap(L"[olist]", L"[/olist]"), L'[', L']')); return L"[olist]" + listItems(L"[*]", L"") + L"\n[/olist]"; }
        if (wrapped(L"<ol>", L"</ol>")) return Trim(StripTags(unwrap(L"<ol>", L"</ol>"), L'<', L'>'));
        return L"<ol>" + listItems(L"  <li>", L"</li>") + L"\n</ol>";
    case LineOp::Task: {
        std::vector<std::wstring> out;
        for (auto& l : ls) { std::wstring x = Trim(l); if (x.empty()) { out.push_back(l); continue; }
            if (Starts(x, L"☐ ")) out.push_back(x.substr(2));
            else out.push_back(bb ? L"☐ " + x : L"<label><input type=\"checkbox\"> " + x + L"</label><br>"); }
        return Join(out); }
    case LineOp::Quote:
        if (bb) { if (wrapped(L"[quote]", L"[/quote]")) return unwrap(L"[quote]", L"[/quote]"); return L"[quote]" + t + L"[/quote]"; }
        if (wrapped(L"<blockquote>", L"</blockquote>")) return unwrap(L"<blockquote>", L"</blockquote>");
        return L"<blockquote>" + t + L"</blockquote>";
    case LineOp::CodeBlock:
        if (bb) { if (wrapped(L"[code]", L"[/code]")) return unwrap(L"[code]", L"[/code]"); return L"[code]\n" + block + L"\n[/code]"; }
        if (wrapped(L"<pre><code>", L"</code></pre>")) return unwrap(L"<pre><code>", L"</code></pre>");
        return L"<pre><code>" + HtmlEsc(block) + L"</code></pre>";
    }
    return block;
}

std::wstring ClearFormatting(Dialect d, const std::wstring& s) {
    if (d == Dialect::BBCode) return StripTags(s, L'[', L']');
    if (d == Dialect::Html) return StripTags(s, L'<', L'>');
    auto ls = Lines(s);
    for (auto& l : ls) {
        std::wstring x = StripMdLinePrefix(l);
        std::wstring o;
        for (size_t i = 0; i < x.size(); ++i) {
            wchar_t c = x[i];
            if (c == L'*' || c == L'_' || c == L'~' || c == L'`') continue;
            if (c == L'[') {   // [text](url) -> text
                size_t m = x.find(L"](", i);
                size_t e = m == std::wstring::npos ? m : x.find(L')', m);
                if (e != std::wstring::npos) { o += x.substr(i + 1, m - i - 1); i = e; continue; }
            }
            o += c;
        }
        l = o;
    }
    return Join(ls);
}

std::wstring MakeTable(Dialect d, int rows, int cols) {
    rows = std::max(1, rows); cols = std::max(1, cols);
    std::wstring o;
    if (d == Dialect::Markdown) {
        for (int r = 0; r <= rows; ++r) {
            o += L"|";
            for (int c = 0; c < cols; ++c) o += r == 0 ? L" Column " + std::to_wstring(c + 1) + L" |" : L"          |";
            o += L"\n";
            if (r == 0) { o += L"|"; for (int c = 0; c < cols; ++c) o += L" -------- |"; o += L"\n"; }
        }
        return o;
    }
    bool bb = d == Dialect::BBCode;
    auto tag = [&](const wchar_t* t, bool close) { return bb ? (close ? L"[/" : L"[") + std::wstring(t) + L"]" : (close ? L"</" : L"<") + std::wstring(t) + L">"; };
    o = tag(L"table", false) + L"\n";
    for (int r = 0; r <= rows; ++r) {
        o += L"  " + tag(L"tr", false);
        for (int c = 0; c < cols; ++c) {
            const wchar_t* cell = r == 0 ? L"th" : L"td";
            o += tag(cell, false) + (r == 0 ? L"Column " + std::to_wstring(c + 1) : L"") + tag(cell, true);
        }
        o += tag(L"tr", true) + L"\n";
    }
    return o + tag(L"table", true) + L"\n";
}
std::wstring MakeRule(Dialect d) { return d == Dialect::Markdown ? L"\n---\n" : d == Dialect::BBCode ? L"\n[hr][/hr]\n" : L"\n<hr>\n"; }
std::wstring MakeImage(Dialect d, const std::wstring& url, const std::wstring& alt) {
    if (d == Dialect::Markdown) return L"![" + alt + L"](" + url + L")";
    if (d == Dialect::BBCode) return L"[img]" + url + L"[/img]";
    return L"<img src=\"" + url + L"\" alt=\"" + HtmlEsc(alt) + L"\">";
}
std::wstring MakeChartBlock(Dialect d, const std::wstring& type) {
    std::wstring body = L"type: " + type + L"\ntitle: Quarterly sales\nQ1: 12\nQ2: 19\nQ3: 7\nQ4: 15\n";
    if (d == Dialect::Html) { ChartSpec c; ParseChart(body, c); return ChartSvg(c) + L"\n"; }
    if (d == Dialect::BBCode) return L"[code]\n" + body + L"[/code]\n";
    return L"```chart\n" + body + L"```\n";
}

// ------------------------------------------------------------------ charts
bool ParseChart(const std::wstring& body, ChartSpec& c) {
    c = ChartSpec{};
    for (auto& raw : Lines(body)) {
        std::wstring l = Trim(raw);
        if (l.empty() || l[0] == L'#') continue;
        size_t k = l.rfind(L':');
        if (k == std::wstring::npos) continue;
        std::wstring key = Trim(l.substr(0, k)), val = Trim(l.substr(k + 1));
        std::wstring lk = Lower(key);
        if (lk == L"type") { c.type = Lower(val); continue; }
        if (lk == L"title") { c.title = val; continue; }
        wchar_t* e = nullptr; double v = wcstod(val.c_str(), &e);
        if (e != val.c_str()) c.data.push_back({ key, v });
    }
    if (c.type != L"bar" && c.type != L"line" && c.type != L"pie") c.type = L"bar";
    return !c.data.empty();
}
static const wchar_t* kPalette[] = { L"#4E79A7", L"#F28E2B", L"#E15759", L"#76B7B2", L"#59A14F", L"#EDC948", L"#B07AA1", L"#FF9DA7" };
std::wstring ChartSvg(const ChartSpec& c, int w, int h) {
    wchar_t b[512];
    std::wstring s;
    swprintf(b, 512, L"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\" font-family=\"Segoe UI, sans-serif\" font-size=\"12\">", w, h, w, h);
    s += b;
    if (!c.title.empty()) { swprintf(b, 512, L"<text x=\"%d\" y=\"18\" text-anchor=\"middle\" font-weight=\"600\">", w / 2); s += b; s += HtmlEsc(c.title) + L"</text>"; }
    double mx = 0, sum = 0; for (auto& d : c.data) { mx = std::max(mx, d.second); sum += std::max(0.0, d.second); }
    if (mx <= 0) mx = 1;
    int top = 30, bottom = h - 28, left = 36, right = w - 12, n = (int)c.data.size();
    if (c.type == L"pie" && sum > 0) {
        double a = -3.14159265358979 / 2, cx = w / 2.0, cy = (top + bottom) / 2.0, r = std::min(w, bottom - top) / 2.0 - 6;
        for (int i = 0; i < n; ++i) {
            double frac = std::max(0.0, c.data[i].second) / sum, a2 = a + frac * 6.28318530717958;
            swprintf(b, 512, L"<path d=\"M%.1f %.1f L%.1f %.1f A%.1f %.1f 0 %d 1 %.1f %.1f Z\" fill=\"%ls\"/>", cx, cy, cx + r * cos(a), cy + r * sin(a), r, r,
                     frac > 0.5 ? 1 : 0, cx + r * cos(a2), cy + r * sin(a2), kPalette[i % 8]);
            s += b;
            double am = (a + a2) / 2;
            swprintf(b, 512, L"<text x=\"%.1f\" y=\"%.1f\" text-anchor=\"middle\" fill=\"#fff\">", cx + r * 0.62 * cos(am), cy + r * 0.62 * sin(am)); s += b;
            s += HtmlEsc(c.data[i].first) + L"</text>";
            a = a2;
        }
        return s + L"</svg>";
    }
    swprintf(b, 512, L"<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#999\"/>", left, bottom, right, bottom); s += b;
    double step = n ? (right - left) / (double)n : 0;
    std::wstring pts;
    for (int i = 0; i < n; ++i) {
        double v = c.data[i].second, bh = (bottom - top) * std::max(0.0, v) / mx;
        double x = left + step * i;
        if (c.type == L"line") { swprintf(b, 512, L"%.1f,%.1f ", x + step / 2, bottom - bh); pts += b; }
        else { swprintf(b, 512, L"<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"%ls\" rx=\"2\"/>", x + step * 0.15, bottom - bh, step * 0.7, bh, kPalette[i % 8]); s += b; }
        swprintf(b, 512, L"<text x=\"%.1f\" y=\"%d\" text-anchor=\"middle\">", x + step / 2, bottom + 16); s += b;
        s += HtmlEsc(c.data[i].first) + L"</text>";
    }
    if (c.type == L"line") { s += L"<polyline fill=\"none\" stroke=\"#4E79A7\" stroke-width=\"2\" points=\"" + pts + L"\"/>"; }
    return s + L"</svg>";
}

// ------------------------------------------------------------------ Markdown -> HTML / BBCode
static std::wstring RunsHtml(const std::vector<MdRun>& runs) {
    std::wstring o;
    for (auto& r : runs) {
        std::wstring t = HtmlEsc(r.text);
        for (size_t p; (p = t.find(L'\n')) != std::wstring::npos;) t.replace(p, 1, L"<br>");
        if (r.style & MS_CODE) t = L"<code>" + t + L"</code>";
        if (r.style & MS_STRIKE) t = L"<s>" + t + L"</s>";
        if (r.style & MS_EM) t = L"<em>" + t + L"</em>";
        if (r.style & MS_STRONG) t = L"<strong>" + t + L"</strong>";
        if (r.style & MS_LINK) t = L"<a href=\"" + HtmlEsc(r.url) + L"\">" + t + L"</a>";
        o += t;
    }
    return o;
}
static std::wstring RunsBB(const std::vector<MdRun>& runs) {
    std::wstring o;
    for (auto& r : runs) {
        std::wstring t = r.text;
        if (r.style & MS_CODE) t = L"[code]" + t + L"[/code]";
        if (r.style & MS_STRIKE) t = L"[strike]" + t + L"[/strike]";
        if (r.style & MS_EM) t = L"[i]" + t + L"[/i]";
        if (r.style & MS_STRONG) t = L"[b]" + t + L"[/b]";
        if (r.style & MS_LINK) t = L"[url=" + r.url + L"]" + t + L"[/url]";
        o += t;
    }
    return o;
}
std::wstring MarkdownToHtml(const std::wstring& md, bool full, const std::wstring& title) {
    auto blocks = ParseMarkdown(md);
    std::wstring o;
    std::vector<std::pair<bool, int>> lists;   // (ordered, level)
    auto closeLists = [&](int toLevel) {
        while (!lists.empty() && lists.back().second >= toLevel) { o += lists.back().first ? L"</ol>\n" : L"</ul>\n"; lists.pop_back(); }
    };
    for (auto& b : blocks) {
        if (b.kind != MdKind::ListItem) closeLists(0);
        switch (b.kind) {
        case MdKind::Heading: o += L"<h" + std::to_wstring(b.level) + L">" + RunsHtml(b.runs) + L"</h" + std::to_wstring(b.level) + L">\n"; break;
        case MdKind::Para: o += L"<p>" + RunsHtml(b.runs) + L"</p>\n"; break;
        case MdKind::Quote: o += L"<blockquote>" + RunsHtml(b.runs) + L"</blockquote>\n"; break;
        case MdKind::Rule: o += L"<hr>\n"; break;
        case MdKind::Code: {
            ChartSpec c;
            if (Lower(b.lang) == L"chart" && ParseChart(b.code, c)) { o += L"<figure>" + ChartSvg(c) + L"</figure>\n"; break; }
            o += L"<pre><code" + (b.lang.empty() ? std::wstring() : L" class=\"language-" + HtmlEsc(b.lang) + L"\"") + L">" + HtmlEsc(b.code) + L"</code></pre>\n";
            break; }
        case MdKind::ListItem: {
            if (!lists.empty() && lists.back().second > b.level) closeLists(b.level + 1);
            if (!lists.empty() && lists.back().second == b.level && lists.back().first != b.ordered) closeLists(b.level);
            if (lists.empty() || lists.back().second < b.level) { lists.push_back({ b.ordered, b.level }); o += b.ordered ? L"<ol>\n" : L"<ul>\n"; }
            std::wstring box = b.task < 0 ? L"" : b.task ? L"<input type=\"checkbox\" checked disabled> " : L"<input type=\"checkbox\" disabled> ";
            o += L"<li>" + box + RunsHtml(b.runs) + L"</li>\n";
            break; }
        case MdKind::Table: {
            o += L"<table>\n";
            for (size_t r = 0; r < b.rows.size(); ++r) {
                o += L"<tr>";
                for (size_t c = 0; c < b.rows[r].size(); ++c) {
                    const wchar_t* t = r == 0 ? L"th" : L"td";
                    const wchar_t* al = c < b.align.size() ? (b.align[c] == 1 ? L" style=\"text-align:center\"" : b.align[c] == 2 ? L" style=\"text-align:right\"" : L"") : L"";
                    o += std::wstring(L"<") + t + al + L">" + RunsHtml(b.rows[r][c]) + L"</" + t + L">";
                }
                o += L"</tr>\n";
            }
            o += L"</table>\n";
            break; }
        }
    }
    closeLists(0);
    if (!full) return o;
    return L"<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>" + HtmlEsc(title) + L"</title>\n<style>\n"
           L"body{font-family:'Segoe UI',sans-serif;max-width:860px;margin:2em auto;padding:0 1em;line-height:1.55}\n"
           L"pre{background:#f4f4f4;padding:.8em;border-radius:6px;overflow:auto}code{font-family:Consolas,monospace}\n"
           L"table{border-collapse:collapse}th,td{border:1px solid #ccc;padding:.3em .7em}blockquote{border-left:4px solid #8ab;margin:0;padding-left:1em;color:#555}\n"
           L"</style>\n</head>\n<body>\n" + o + L"</body>\n</html>\n";
}
std::wstring MarkdownToBBCode(const std::wstring& md) {
    auto blocks = ParseMarkdown(md);
    std::wstring o;
    int openList = -1;   // 0 list, 1 olist
    for (auto& b : blocks) {
        if (b.kind != MdKind::ListItem && openList >= 0) { o += openList ? L"[/olist]\n" : L"[/list]\n"; openList = -1; }
        switch (b.kind) {
        case MdKind::Heading: { int h = std::min(3, b.level); o += L"[h" + std::to_wstring(h) + L"]" + RunsBB(b.runs) + L"[/h" + std::to_wstring(h) + L"]\n"; break; }
        case MdKind::Para: o += RunsBB(b.runs) + L"\n\n"; break;
        case MdKind::Quote: o += L"[quote]" + RunsBB(b.runs) + L"[/quote]\n"; break;
        case MdKind::Rule: o += L"[hr][/hr]\n"; break;
        case MdKind::Code: o += L"[code]" + b.code + L"[/code]\n"; break;
        case MdKind::ListItem:
            if (openList != (b.ordered ? 1 : 0)) { if (openList >= 0) o += openList ? L"[/olist]\n" : L"[/list]\n"; openList = b.ordered ? 1 : 0; o += b.ordered ? L"[olist]\n" : L"[list]\n"; }
            o += L"[*]" + std::wstring(b.task < 0 ? L"" : b.task ? L"☑ " : L"☐ ") + RunsBB(b.runs) + L"\n";
            break;
        case MdKind::Table:
            o += L"[table]\n";
            for (size_t r = 0; r < b.rows.size(); ++r) {
                o += L"[tr]";
                for (auto& cell : b.rows[r]) o += (r == 0 ? L"[th]" : L"[td]") + RunsBB(cell) + (r == 0 ? L"[/th]" : L"[/td]");
                o += L"[/tr]\n";
            }
            o += L"[/table]\n";
            break;
        }
    }
    if (openList >= 0) o += openList ? L"[/olist]\n" : L"[/list]\n";
    while (!o.empty() && o.back() == L'\n') o.pop_back();
    return o + L"\n";
}

// ------------------------------------------------------------------ DOM readers
std::wstring MNode::Attr(const wchar_t* k) const { for (auto& a : attrs) if (a.first == k) return a.second; return L""; }

static bool IsNameChar(wchar_t c) { return iswalnum(c) || c == L'-' || c == L'_' || c == L'*' || c == L':'; }

// BBCode: tokenizer + stack builder (shortcode grammar)
MNode ParseBBCode(const std::wstring& s) {
    struct Tag { bool close = false, self = false; std::wstring name; std::vector<std::pair<std::wstring, std::wstring>> attrs; size_t end = 0; };
    auto readTag = [&](size_t i, Tag& t) -> bool {
        if (s[i] != L'[') return false;
        size_t p = i + 1;
        if (p < s.size() && s[p] == L'/') { t.close = true; ++p; }
        size_t n0 = p;
        while (p < s.size() && IsNameChar(s[p])) ++p;
        if (p == n0) return false;
        t.name = Lower(s.substr(n0, p - n0));
        auto value = [&](std::wstring& v) {
            if (p < s.size() && (s[p] == L'"' || s[p] == L'\'')) { wchar_t q = s[p++]; size_t e = s.find(q, p); if (e == std::wstring::npos) return false; v = s.substr(p, e - p); p = e + 1; return true; }
            size_t a = p; while (p < s.size() && s[p] != L']' && s[p] != L' ' && !(s[p] == L'/' && p + 1 < s.size() && s[p + 1] == L']')) ++p;
            v = s.substr(a, p - a); return true;
        };
        if (!t.close && p < s.size() && s[p] == L'=') { ++p; std::wstring v; if (!value(v)) return false; t.attrs.push_back({ L"", v }); }
        while (!t.close && p < s.size() && s[p] == L' ') {
            while (p < s.size() && s[p] == L' ') ++p;
            size_t k0 = p; while (p < s.size() && IsNameChar(s[p])) ++p;
            if (p == k0) break;
            std::wstring k = Lower(s.substr(k0, p - k0)), v;
            if (p < s.size() && s[p] == L'=') { ++p; if (!value(v)) return false; }
            t.attrs.push_back({ k, v });
        }
        while (p < s.size() && s[p] == L' ') ++p;
        if (p + 1 < s.size() && s[p] == L'/' && s[p + 1] == L']') { t.self = true; p += 1; }
        if (p >= s.size() || s[p] != L']') return false;
        t.end = p + 1;
        return true;
    };
    static const wchar_t* kVerbatim[] = { L"code", L"noparse", L"pre" };
    MNode root; root.name = L"#root";
    std::vector<MNode*> st{ &root };
    std::wstring text;
    auto flush = [&] { if (!text.empty()) { MNode t; t.name = L"#text"; t.text = text; st.back()->kids.push_back(t); text.clear(); } };
    size_t i = 0;
    while (i < s.size()) {
        Tag t;
        if (s[i] == L'[' && readTag(i, t)) {
            if (t.close) {
                int k = (int)st.size() - 1;
                while (k > 0 && st[k]->name != t.name) --k;
                if (k > 0) { flush(); st.resize(k); i = t.end; continue; }
                text += s.substr(i, t.end - i); i = t.end; continue;   // unmatched close -> literal
            }
            flush();
            if (t.name == L"*") {   // list item: close previous item at the same level
                if (st.back()->name == L"*") st.pop_back();
                MNode n; n.name = L"*"; st.back()->kids.push_back(n); st.push_back(&st.back()->kids.back());
                i = t.end; continue;
            }
            MNode n; n.name = t.name; n.attrs = t.attrs;
            bool verb = false; for (auto* v : kVerbatim) if (t.name == v) verb = true;
            if (t.self) { st.back()->kids.push_back(n); i = t.end; continue; }
            if (verb) {
                std::wstring closeTag = L"[/" + t.name + L"]";
                size_t e = Lower(s).find(closeTag, t.end);
                MNode tx; tx.name = L"#text"; tx.text = s.substr(t.end, e == std::wstring::npos ? std::wstring::npos : e - t.end);
                n.kids.push_back(tx);
                st.back()->kids.push_back(n);
                i = e == std::wstring::npos ? s.size() : e + closeTag.size();
                continue;
            }
            // closing [/list] must also close an open [*]
            st.back()->kids.push_back(n);
            st.push_back(&st.back()->kids.back());
            i = t.end; continue;
        }
        text += s[i++];
    }
    flush();
    // tags never closed: keep their content (lenient)
    return root;
}

static std::wstring DecodeEntities(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'&') {
            size_t e = s.find(L';', i);
            if (e != std::wstring::npos && e - i <= 10) {
                std::wstring n = s.substr(i + 1, e - i - 1);
                wchar_t c = 0;
                if (n == L"lt") c = L'<'; else if (n == L"gt") c = L'>'; else if (n == L"amp") c = L'&'; else if (n == L"quot") c = L'"';
                else if (n == L"apos") c = L'\''; else if (n == L"nbsp") c = 0xA0;
                else if (!n.empty() && n[0] == L'#') c = (wchar_t)wcstoul(n.c_str() + (n.size() > 1 && (n[1] == L'x' || n[1] == L'X') ? 2 : 1), nullptr, n.size() > 1 && (n[1] == L'x' || n[1] == L'X') ? 16 : 10);
                if (c) { o += c; i = e; continue; }
            }
        }
        o += s[i];
    }
    return o;
}

MNode ParseHtml(const std::wstring& s) {
    static const wchar_t* kVoid[] = { L"br", L"hr", L"img", L"input", L"meta", L"link", L"col", L"wbr", L"source" };
    MNode root; root.name = L"#root";
    std::vector<MNode*> st{ &root };
    std::wstring text;
    auto flush = [&] { if (!text.empty()) { MNode t; t.name = L"#text"; t.text = DecodeEntities(text); st.back()->kids.push_back(t); text.clear(); } };
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == L'<') {
            if (s.compare(i, 4, L"<!--") == 0) { size_t e = s.find(L"-->", i); i = e == std::wstring::npos ? s.size() : e + 3; continue; }
            if (i + 1 < s.size() && (s[i + 1] == L'!' || s[i + 1] == L'?')) { size_t e = s.find(L'>', i); i = e == std::wstring::npos ? s.size() : e + 1; continue; }
            size_t p = i + 1; bool close = false;
            if (p < s.size() && s[p] == L'/') { close = true; ++p; }
            size_t n0 = p; while (p < s.size() && (iswalnum(s[p]) || s[p] == L'-')) ++p;
            if (p == n0) { text += s[i++]; continue; }
            std::wstring name = Lower(s.substr(n0, p - n0));
            MNode n; n.name = name;
            bool self = false;
            while (p < s.size() && s[p] != L'>') {
                if (s[p] == L'/' && p + 1 < s.size() && s[p + 1] == L'>') { self = true; ++p; break; }
                if (iswspace(s[p])) { ++p; continue; }
                size_t k0 = p; while (p < s.size() && !iswspace(s[p]) && s[p] != L'=' && s[p] != L'>' && s[p] != L'/') ++p;
                std::wstring k = Lower(s.substr(k0, p - k0)), v;
                if (p < s.size() && s[p] == L'=') {
                    ++p;
                    if (p < s.size() && (s[p] == L'"' || s[p] == L'\'')) { wchar_t q = s[p++]; size_t e = s.find(q, p); if (e == std::wstring::npos) e = s.size(); v = s.substr(p, e - p); p = e + 1; }
                    else { size_t a = p; while (p < s.size() && !iswspace(s[p]) && s[p] != L'>') ++p; v = s.substr(a, p - a); }
                }
                if (!k.empty()) n.attrs.push_back({ k, DecodeEntities(v) });
                if (p == k0) ++p;
            }
            size_t end = std::min(s.size(), p + 1);
            flush();
            if (close) {
                int k = (int)st.size() - 1;
                while (k > 0 && st[k]->name != name) --k;
                if (k > 0) st.resize(k);
                i = end; continue;
            }
            if (name == L"script" || name == L"style") { std::wstring ct = L"</" + name; size_t e = Lower(s).find(ct, end); size_t g = e == std::wstring::npos ? s.size() : s.find(L'>', e); i = g == std::wstring::npos ? s.size() : g + 1; continue; }
            if (name == L"pre") {
                std::wstring ct = L"</pre"; size_t e = Lower(s).find(ct, end);
                std::wstring inner = s.substr(end, e == std::wstring::npos ? std::wstring::npos : e - end);
                MNode tx; tx.name = L"#text"; tx.text = DecodeEntities(StripTags(inner, L'<', L'>')); n.kids.push_back(tx);
                st.back()->kids.push_back(n);
                size_t g = e == std::wstring::npos ? s.size() : s.find(L'>', e); i = g == std::wstring::npos ? s.size() : g + 1; continue;
            }
            bool isVoid = self; for (auto* v : kVoid) if (name == v) isVoid = true;
            if (name == L"li" && st.back()->name == L"li") st.pop_back();
            if (name == L"p" && st.back()->name == L"p") st.pop_back();
            st.back()->kids.push_back(n);
            if (!isVoid) st.push_back(&st.back()->kids.back());
            i = end; continue;
        }
        text += s[i++];
    }
    flush();
    return root;
}

// ------------------------------------------------------------------ DOM -> Markdown
struct MdW {
    std::wstring out;
    void Block(const std::wstring& b) { while (!out.empty() && out.back() == L'\n' && out.size() >= 2 && out[out.size() - 2] == L'\n') out.pop_back(); if (!out.empty() && out.back() != L'\n') out += L"\n"; if (!out.empty()) out += L"\n"; out += b; out += L"\n"; }
};
static std::wstring Norm(const std::wstring& n) {
    if (n == L"b" || n == L"strong") return L"strong";
    if (n == L"i" || n == L"em") return L"em";
    if (n == L"s" || n == L"strike" || n == L"del") return L"s";
    if (n == L"url" || n == L"a") return L"a";
    if (n == L"list" || n == L"ul") return L"ul";
    if (n == L"olist" || n == L"ol") return L"ol";
    if (n == L"*" || n == L"li") return L"li";
    if (n == L"quote" || n == L"blockquote") return L"blockquote";
    if (n == L"code" || n == L"noparse") return L"code";
    return n;
}
static std::wstring InlineMd(const MNode& n, bool bb);
static std::wstring KidsInline(const MNode& n, bool bb) { std::wstring o; for (auto& k : n.kids) o += InlineMd(k, bb); return o; }
static std::wstring CollapseWs(const std::wstring& s) {
    std::wstring o; bool sp = false;
    for (wchar_t c : s) { if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ') { if (!sp) o += L' '; sp = true; } else { o += c; sp = false; } }
    return o;
}
static std::wstring InlineMd(const MNode& n, bool bb) {
    if (n.name == L"#text") return bb ? n.text : CollapseWs(n.text);
    std::wstring nm = Norm(n.name), in = KidsInline(n, bb);
    if (nm == L"strong") return L"**" + Trim(in) + L"**";
    if (nm == L"em") return L"*" + Trim(in) + L"*";
    if (nm == L"s") return L"~~" + Trim(in) + L"~~";
    if (nm == L"u") return L"<u>" + in + L"</u>";
    if (nm == L"code") return L"`" + in + L"`";
    if (nm == L"br") return L"  \n";
    if (nm == L"a") { std::wstring u = bb ? n.Attr(L"") : n.Attr(L"href"); if (u.empty()) u = Trim(in); return L"[" + Trim(in) + L"](" + u + L")"; }
    if (nm == L"img") { std::wstring u = bb ? Trim(in) : n.Attr(L"src"); return L"![" + n.Attr(L"alt") + L"](" + u + L")"; }
    return in;   // color/size/font/span/etc: keep content
}
static void BlockMd(const MNode& n, bool bb, MdW& w, int listDepth = 0);
static void Kids(const MNode& n, bool bb, MdW& w, int depth) {
    std::wstring inl;
    auto flushInl = [&] {
        std::wstring t = bb ? inl : Trim(inl);
        if (bb) {   // BBCode: newlines are significant; split paragraphs on blank lines
            size_t a = 0;
            for (;;) {
                size_t e = t.find(L"\n\n", a);
                std::wstring para = Trim(t.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a));
                if (!para.empty()) { for (size_t p; (p = para.find(L'\n')) != std::wstring::npos;) para.replace(p, 1, L"  \r"); for (auto& c : para) if (c == L'\r') c = L'\n'; w.Block(para); }
                if (e == std::wstring::npos) break;
                a = e + 2;
            }
        } else if (!t.empty()) w.Block(t);
        inl.clear();
    };
    static const wchar_t* blocks[] = { L"p", L"div", L"h1", L"h2", L"h3", L"h4", L"h5", L"h6", L"ul", L"ol", L"blockquote", L"pre", L"hr", L"table", L"section", L"article", L"body", L"html", L"header", L"footer", L"main", L"figure" };
    for (auto& k : n.kids) {
        std::wstring nm = Norm(k.name);
        bool isBlock = false; for (auto* b : blocks) if (nm == b) isBlock = true;
        if (bb && nm == L"code") isBlock = true;
        if (isBlock) { flushInl(); BlockMd(k, bb, w, depth); }
        else inl += InlineMd(k, bb);
    }
    flushInl();
}
static void BlockMd(const MNode& n, bool bb, MdW& w, int depth) {
    std::wstring nm = Norm(n.name);
    if (nm.size() == 2 && nm[0] == L'h' && nm[1] >= L'1' && nm[1] <= L'6') { w.Block(std::wstring(nm[1] - L'0', L'#') + L" " + Trim(KidsInline(n, bb))); return; }
    if (nm == L"hr") { w.Block(L"---"); return; }
    if (nm == L"pre" || (bb && nm == L"code")) {
        std::wstring t; for (auto& k : n.kids) t += k.name == L"#text" ? k.text : KidsInline(k, bb);
        while (!t.empty() && t.front() == L'\n') t.erase(0, 1);
        while (!t.empty() && t.back() == L'\n') t.pop_back();
        w.Block(L"```\n" + t + L"\n```"); return;
    }
    if (nm == L"blockquote") {
        MdW inner; Kids(n, bb, inner, depth);
        auto ls = Lines(Trim(inner.out));
        for (auto& l : ls) l = L"> " + l;
        w.Block(Join(ls)); return;
    }
    if (nm == L"ul" || nm == L"ol") {
        int num = 1; std::wstring o;
        for (auto& k : n.kids) {
            if (Norm(k.name) != L"li") { std::wstring t = Trim(InlineMd(k, bb)); if (!t.empty() && bb) o += std::wstring(depth * 2, L' ') + L"- " + t + L"\n"; continue; }
            std::wstring item; MdW sub;
            for (auto& c : k.kids) {
                std::wstring cn = Norm(c.name);
                if (cn == L"ul" || cn == L"ol") BlockMd(c, bb, sub, depth + 1); else item += InlineMd(c, bb);
            }
            o += std::wstring(depth * 2, L' ') + (nm == L"ol" ? std::to_wstring(num++) + L". " : L"- ") + Trim(CollapseWs(item)) + L"\n";
            std::wstring st = sub.out; while (!st.empty() && st.front() == L'\n') st.erase(0, 1);
            if (!st.empty()) { o += st; if (o.back() != L'\n') o += L"\n"; }
        }
        while (!o.empty() && o.back() == L'\n') o.pop_back();
        if (depth > 0) { w.out += o + L"\n"; return; }
        w.Block(o); return;
    }
    if (nm == L"table") {
        std::vector<std::vector<std::wstring>> rows;
        std::function<void(const MNode&)> walk = [&](const MNode& x) {
            for (auto& k : x.kids) {
                if (k.name == L"tr") { std::vector<std::wstring> r; for (auto& c : k.kids) if (c.name == L"td" || c.name == L"th") r.push_back(Trim(CollapseWs(KidsInline(c, bb)))); rows.push_back(r); }
                else if (k.name != L"#text") walk(k);
            }
        };
        walk(n);
        if (rows.empty()) return;
        size_t cols = 0; for (auto& r : rows) cols = std::max(cols, r.size());
        std::wstring o;
        for (size_t r = 0; r < rows.size(); ++r) {
            o += L"|"; for (size_t c = 0; c < cols; ++c) o += L" " + (c < rows[r].size() ? rows[r][c] : L"") + L" |";
            o += L"\n";
            if (r == 0) { o += L"|"; for (size_t c = 0; c < cols; ++c) o += L" --- |"; o += L"\n"; }
        }
        o.pop_back();
        w.Block(o); return;
    }
    Kids(n, bb, w, depth);
}
static std::wstring Finish(std::wstring s) {
    while (!s.empty() && (s.front() == L'\n')) s.erase(0, 1);
    while (!s.empty() && s.back() == L'\n') s.pop_back();
    return s + L"\n";
}
std::wstring BBCodeToMarkdown(const std::wstring& bb) { MdW w; BlockMd(ParseBBCode(bb), true, w); return Finish(w.out); }
std::wstring HtmlToMarkdown(const std::wstring& html) { MdW w; BlockMd(ParseHtml(html), false, w); return Finish(w.out); }

} // namespace np
