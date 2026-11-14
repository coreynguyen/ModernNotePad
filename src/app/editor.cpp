// editor.cpp - the text editor view: DirectWrite layouts per logical line (cached by the
// line's stamp), syntax colouring via drawing effects, wrap, folding, gutter, selection,
// find highlights, spell squiggles, custom overlay scrollbars, caret & keyboard/mouse editing.
#include "app.h"
#include <cmath>
#include <cwctype>

namespace app {

using np::Pos;

bool g_caretOn = true;
std::vector<np::Match> g_findMatches;
np::Searcher g_searcher;
bool g_findActive = false;

struct LineLayout {
    Com<IDWriteTextLayout> L;
    float width = 0, height = 0, wrapW = 0;
    int stateIn = 0;
    np::Lang lang = np::L_TEXT;
    bool syntax = false;
    uint64_t live = 0;   // formatted-view key (0 = plain/syntax layout)
};
static std::unordered_map<uint32_t, LineLayout> s_cache;
static std::unordered_map<uint32_t, std::vector<std::pair<int, int>>> s_spell;
static Doc* s_cacheDoc = nullptr;
static Com<IDWriteTextFormat> s_fmt, s_fmtGutter;
static float s_fontPx = 0, s_lineH = 16, s_spaceW = 8, s_digitW = 8, s_baseline = 12;
static float s_wrapW = 0, s_maxW = 0;
static EditorRects s_r;
static std::vector<float> s_rowY;   // painted row tops (for hit test), parallel to s_rowLine
static std::vector<int> s_rowLine;
static uint64_t s_lastEdit = 0;
// mouse state
static int s_drag = 0;               // 1 text, 2 gutter lines, 3 vscroll, 4 hscroll
static int s_clickCount = 0; static DWORD s_lastClick = 0; static POINT s_lastClickPt{};
static Pos s_dragOrigA, s_dragOrigB;
static float s_dragGrab = 0, s_lastMx = 0, s_lastMy = 0;
static bool s_hoverV = false, s_hoverH = false, s_hoverGutter = false;

float EditorLineHeight() { return s_lineH; }

void EditorClearCache() { s_cache.clear(); s_spell.clear(); s_maxW = 0; }

static void EnsureFormat() {
    float px = EditorFontPx();
    if (s_fmt && std::fabs(px - s_fontPx) < 0.01f) return;
    s_fontPx = px; s_cache.clear(); s_cache.reserve(8192); s_maxW = 0;
    s_fmt.Reset(); s_fmtGutter.Reset();
    std::wstring fam = MonoFont(g_set.fontFace);
    g_dw->CreateTextFormat(fam.c_str(), nullptr, g_set.fontBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                           g_set.fontItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", &s_fmt);
    g_dw->CreateTextFormat(fam.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px * 0.92f, L"", &s_fmtGutter);
    s_fmtGutter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    s_fmtGutter->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    // metrics from a probe layout
    Com<IDWriteTextLayout> l;
    g_dw->CreateTextLayout(L" ", 1, s_fmt, 1000, 1000, &l);
    DWRITE_TEXT_METRICS m; l->GetMetrics(&m); s_spaceW = m.widthIncludingTrailingWhitespace;
    DWRITE_LINE_METRICS lm; UINT32 cnt = 0; l->GetLineMetrics(&lm, 1, &cnt);
    s_lineH = std::ceil(lm.height * 1.0f + px * 0.18f); s_baseline = lm.baseline;
    s_fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, s_lineH, s_baseline + (s_lineH - lm.height) / 2);
    s_fmt->SetIncrementalTabStop(s_spaceW * g_set.tabSize);
    Com<IDWriteTextLayout> d; g_dw->CreateTextLayout(L"0", 1, s_fmtGutter, 1000, 1000, &d);
    d->GetMetrics(&m); s_digitW = m.widthIncludingTrailingWhitespace;
}

// Robust range -> rects (fixed buffer first, grow on E_NOT_SUFFICIENT_BUFFER).
static void RangeRects(IDWriteTextLayout* L, UINT32 pos, UINT32 len, std::vector<DWRITE_HIT_TEST_METRICS>& out) {
    out.resize(16);
    UINT32 cnt = 0;
    HRESULT hr = L->HitTestTextRange(pos, len, 0, 0, out.data(), (UINT32)out.size(), &cnt);
    if (hr == E_NOT_SUFFICIENT_BUFFER || cnt > out.size()) { out.resize(cnt); hr = L->HitTestTextRange(pos, len, 0, 0, out.data(), cnt, &cnt); }
    out.resize(SUCCEEDED(hr) ? cnt : 0);
}

static const LineLayout& GetLayout(Doc* d, int line) {
    EnsureFormat();   // may be reached from timers before the first paint
    uint32_t stamp = d->text.Stamp(line);
    bool lv = LiveOn(d);
    float avail = s_wrapW > 0 ? s_wrapW : std::max(200.f, RW(s_r.text) - 12);
    uint64_t lk = lv ? LiveKey(d, line, line == d->caret.line, avail) : 0;
    bool syn = g_set.syntax && d->lang != np::L_TEXT && !d->large && !lv;
    int st = syn ? d->lex.StateAt(d->text, d->lang, line) : 0;
    float ww = g_set.wordWrap ? s_wrapW : 0;
    auto it = s_cache.find(stamp);
    if (it != s_cache.end() && it->second.stateIn == st && it->second.lang == d->lang && it->second.syntax == syn && it->second.wrapW == ww && it->second.live == lk)
        return it->second;
    if (s_cache.size() > 6000) s_cache.clear();
    LineLayout& e = s_cache[stamp];
    e = LineLayout{};
    const std::wstring& s = d->text.Line(line);
    UINT32 n = (UINT32)std::min<size_t>(s.size(), 200000);
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), n, s_fmt, ww > 0 ? ww : 1e7f, 1e6f, &e.L)) || !e.L) {
        n = 0; g_dw->CreateTextLayout(L"", 0, s_fmt, 1e7f, 1e6f, &e.L);
        if (!e.L) { e.height = s_lineH; static LineLayout dummy; return e; }
    }
    if (ww > 0) e.L->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (lv && LiveStyle(d, line, line == d->caret.line, e.L, s_fmt, s_fontPx, avail)) e.L->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_DEFAULT, 0, 0);
    if (syn && n) {
        std::vector<np::Span> sp;
        np::LexLine(d->lang, s, st, sp);
        for (auto& x : sp) {
            if (x.tok == np::T_TEXT) continue;
            DWRITE_TEXT_RANGE r{ (UINT32)x.start, (UINT32)x.len };
            e.L->SetDrawingEffect(Brush(g_th.tok[x.tok]), r);
            if (x.tok == np::T_HEADING || x.tok == np::T_STRONG) e.L->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
            if (x.tok == np::T_EMPH || (x.tok == np::T_COMMENT && d->lang != np::L_MD)) e.L->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
            if (x.tok == np::T_LINK) e.L->SetUnderline(TRUE, r);
        }
    }
    DWRITE_TEXT_METRICS m; e.L->GetMetrics(&m);
    e.width = m.widthIncludingTrailingWhitespace;
    e.height = std::max(s_lineH, m.height);
    e.wrapW = ww; e.stateIn = st; e.lang = d->lang; e.syntax = syn; e.live = lk;
    if (e.width > s_maxW) s_maxW = e.width;
    return e;
}

// ------------------------------------------------------------------ rows / folding
void EditorUpdateRows(Doc* d) {
    if (!d->rowsDirty && d->rowsVer == d->text.Version()) return;
    int n = d->text.LineCount();
    if (LiveOn(d)) {   // formatted view: table bodies are drawn by their header line
        LiveSync(d);
        d->rows.clear();
        if (d->liveTables) {
            d->rowsIdentity = false; d->rows.reserve(n);
            for (int i = 0; i < n; ++i) if (d->live[i].kind != np::LiveKind::TableHidden) d->rows.push_back(i);
        } else d->rowsIdentity = true;
        d->rowsDirty = false; d->rowsVer = d->text.Version();
        if (d->topRow >= RowCount(d)) d->topRow = std::max(0, RowCount(d) - 1);
        return;
    }
    if (!d->anyFolded || d->folds.empty()) {   // fast path: no folding -> row == line, O(1)
        d->rowsIdentity = true; d->rows.clear();
    } else {
        d->rowsIdentity = false;
        d->rows.clear(); d->rows.reserve(n);
        size_t fi = 0; bool any = false;
        for (int i = 0; i < n; ++i) {
            d->rows.push_back(i);
            if (d->text.Flags(i) & np::LF_FOLDED) {
                while (fi < d->folds.size() && d->folds[fi].start < i) ++fi;
                if (fi < d->folds.size() && d->folds[fi].start == i) { i = d->folds[fi].end; any = true; }
                else d->text.Flags(i) &= ~np::LF_FOLDED;
            }
        }
        if (!any) { d->anyFolded = false; d->rowsIdentity = true; d->rows.clear(); }
    }
    d->rowsDirty = false; d->rowsVer = d->text.Version();
    if (d->topRow >= RowCount(d)) d->topRow = std::max(0, RowCount(d) - 1);
}
static int RowOf(Doc* d, int line) {
    EditorUpdateRows(d);
    if (d->rowsIdentity) return std::max(0, std::min(line, d->text.LineCount() - 1));
    auto it = std::upper_bound(d->rows.begin(), d->rows.end(), line);
    return std::max(0, (int)(it - d->rows.begin()) - 1);
}
static const np::FoldRegion* RegionAt(Doc* d, int line) {
    auto it = std::lower_bound(d->folds.begin(), d->folds.end(), line, [](const np::FoldRegion& r, int l) { return r.start < l; });
    return (it != d->folds.end() && it->start == line) ? &*it : nullptr;
}
void EditorRecomputeFolds(Doc* d) {
    if (g_set.folding && !d->hex && !d->large && !LiveOn(d)) np::ComputeFolds(d->text, d->lang, d->lex, d->folds);
    else d->folds.clear();
    d->foldsVer = d->text.Version();
    d->rowsDirty = true;
}
void EditorUnfoldFor(Doc* d, int line) {
    bool ch = false;
    for (auto& r : d->folds) if (r.start < line && line <= r.end && (d->text.Flags(r.start) & np::LF_FOLDED)) { d->text.Flags(r.start) &= ~np::LF_FOLDED; ch = true; }
    if (ch) d->rowsDirty = true;
}
void EditorToggleFold(Doc* d, int line, int force) {
    if (d->foldsVer != d->text.Version()) EditorRecomputeFolds(d);
    const np::FoldRegion* r = RegionAt(d, line);
    if (!r) {   // fold the innermost region containing line
        const np::FoldRegion* best = nullptr;
        for (auto& x : d->folds) if (x.start < line && line <= x.end && (!best || x.start > best->start)) best = &x;
        if (!best) return;
        r = best;
    }
    uint8_t& f = d->text.Flags(r->start);
    bool fold = force < 0 ? !(f & np::LF_FOLDED) : force == 1;
    if (fold) { f |= np::LF_FOLDED; d->anyFolded = true; } else f &= ~np::LF_FOLDED;
    d->rowsDirty = true;
    if (fold && d->caret.line > r->start && d->caret.line <= r->end) {
        d->caret = d->anchor = { r->start, (int)d->text.Line(r->start).size() };
    }
    Invalidate();
}
void EditorFoldAll(Doc* d, bool fold) {
    if (d->foldsVer != d->text.Version()) EditorRecomputeFolds(d);
    for (auto& r : d->folds) { if (fold) d->text.Flags(r.start) |= np::LF_FOLDED; else d->text.Flags(r.start) &= ~np::LF_FOLDED; }
    if (fold && !d->folds.empty()) d->anyFolded = true;
    if (fold) {   // keep caret visible
        for (auto& r : d->folds) if (d->caret.line > r.start && d->caret.line <= r.end) { d->caret = d->anchor = { r.start, 0 }; break; }
    }
    d->rowsDirty = true; Invalidate();
}

// ------------------------------------------------------------------ geometry
static void Layout(Doc* d, const RectF& area) {
    EnsureFormat();
    s_r.area = area;
    float gw = 8;
    if (g_set.lineNumbers) {
        int digits = 1, n = d->text.LineCount(); while (n >= 10) { n /= 10; ++digits; }
        gw = std::max(3, digits) * s_digitW + 22;
    }
    if (g_set.folding) gw += 14;
    float vbw = 12, hbw = g_set.wordWrap ? 0.f : 12.f;
    s_r.gutter = { area.left, area.top, area.left + gw, area.bottom - hbw };
    s_r.text = { area.left + gw, area.top, area.right - vbw, area.bottom - hbw };
    s_r.vbar = { area.right - vbw, area.top, area.right, area.bottom - hbw };
    s_r.hbar = { area.left + gw, area.bottom - hbw, area.right - vbw, area.bottom };
    float ww = std::max(40.f, RW(s_r.text) - 12);
    if (g_set.wordWrap && std::fabs(ww - s_wrapW) > 0.5f) { s_wrapW = ww; }
    if (!g_set.wordWrap) s_wrapW = 0;
}
static float TextX0(Doc* d) { return s_r.text.left + 6 - (g_set.wordWrap ? 0 : d->scrollX); }
static int VisibleRowCount() { return std::max(1, (int)(RH(s_r.text) / s_lineH)); }

// ------------------------------------------------------------------ painting
static void SpellFor(Doc* d, int line, const LineLayout& ll, std::vector<std::pair<int, int>>& out) {
    out.clear();
    uint32_t stamp = d->text.Stamp(line);
    auto it = s_spell.find(stamp);
    if (it != s_spell.end()) { out = it->second; return; }
    const std::wstring& s = d->text.Line(line);
    std::vector<std::pair<int, int>> errs;
    if (!s.empty() && s.size() < 5000) SpellCheckLine(s, errs);
    if (d->lang != np::L_TEXT && !errs.empty()) {
        std::vector<np::Span> sp;
        np::LexLine(d->lang, s, ll.stateIn, sp);
        std::vector<std::pair<int, int>> keep;
        for (auto& e : errs)
            for (auto& x : sp) if (e.first >= x.start && e.first < x.start + x.len) { if (np::IsProseTok(d->lang, x.tok)) keep.push_back(e); break; }
        errs.swap(keep);
    }
    if (s_spell.size() > 20000) s_spell.clear();
    s_spell[stamp] = errs;
    out = errs;
}
static void Squiggle(float x0, float x1, float y, Color c) {
    Com<ID2D1PathGeometry> g; g_d2d->CreatePathGeometry(&g);
    Com<ID2D1GeometrySink> k; g->Open(&k);
    float a = 1.6f, step = 2.2f;
    k->BeginFigure({ x0, y }, D2D1_FIGURE_BEGIN_HOLLOW);
    int i = 0;
    for (float x = x0 + step; x <= x1 + 0.01f; x += step, ++i) k->AddLine({ x, y + ((i & 1) ? 0 : a) });
    k->EndFigure(D2D1_FIGURE_END_OPEN); k->Close();
    g_rt->DrawGeometry(g, Brush(c), 1.f);
}

void EditorPaint(Doc* d, const RectF& area) {
    if (d->loading) {
        FillR(area, g_th.editor);
        wchar_t b[128]; swprintf(b, 128, L"Opening %ls  (%.1f MB)\u2026", d->Name().c_str(), d->loadMB);
        Text(d->path.empty() ? std::wstring(b) : std::wstring(b), RF(area.left, area.top, RW(area), RH(area) * 0.6f), UiFmt(15), g_th.dim, 1);
        float t = (GetTickCount64() % 1200) / 1200.f, w = 160, x = (area.left + area.right) / 2 - w / 2, y = area.top + RH(area) * 0.6f / 2 + 26;
        FillRR(RF(x, y, w, 3), 1.5f, g_th.border); FillRR(RF(x + (w - 48) * t, y, 48, 3), 1.5f, g_th.accent);
        return;
    }
    if (s_cacheDoc != d) { EditorClearCache(); s_cacheDoc = d; }
    Layout(d, area);
    if (d->foldsVer != d->text.Version() && GetTickCount64() - s_lastEdit > 350) {
        // keep folded flags valid against fresh regions
        EditorRecomputeFolds(d);
    }
    EditorUpdateRows(d);
    FillR(area, g_th.editor);
    PaintPattern(area, d->topRow * s_lineH);
    if (g_th.pink && d->text.Length() == 0) PaintPinkEmpty(area);
    g_rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    Pos sa = d->caret, sb = d->anchor; if (sb < sa) std::swap(sa, sb);
    bool hasSel = sa != sb;
    float x0 = TextX0(d);
    float y = s_r.text.top + 2;
    s_rowY.clear(); s_rowLine.clear();
    bool focus = GetFocus() == g_hwnd;
    bool lv = LiveOn(d);
    if (lv) LiveBeginFrame();
    std::vector<std::pair<int, int>> errs;
    for (int r = d->topRow; r < RowCount(d) && y < s_r.text.bottom; ++r) {
        int line = RowLine(d, r);
        const LineLayout& ll = GetLayout(d, line);
        float h = ll.height;
        s_rowY.push_back(y); s_rowLine.push_back(line);
        if (line == d->caret.line && !hasSel && !(lv && d->live[line].kind == np::LiveKind::TableHead)) FillR({ area.left, y, s_r.text.right, y + h }, g_th.lineHi);
        g_rt->PushAxisAlignedClip({ s_r.text.left, s_r.text.top, s_r.text.right, s_r.text.bottom }, D2D1_ANTIALIAS_MODE_ALIASED);
        if (lv) LiveDecor(d, line, ll.L, x0, y, h, s_r.text);
        // find highlights
        if (g_findActive && !g_findMatches.empty()) {
            size_t lo = d->text.Offset({ line, 0 }), hi = lo + d->text.Line(line).size();
            auto it = std::lower_bound(g_findMatches.begin(), g_findMatches.end(), lo, [](const np::Match& m, size_t v) { return m.off + m.len <= v; });
            for (; it != g_findMatches.end() && it->off <= hi; ++it) {
                size_t a = std::max(it->off, lo), b = std::min(it->off + it->len, hi);
                if (b <= a) continue;
                std::vector<DWRITE_HIT_TEST_METRICS> hm; RangeRects(ll.L, (UINT32)((UINT32)(a - lo)), (UINT32)((UINT32)(b - a)), hm);
                for (auto& m : hm) FillRR(RF(x0 + m.left, y + m.top, std::max(2.f, m.width), m.height), 2, g_th.findHi);
            }
        }
        // selection
        if (hasSel && line >= sa.line && line <= sb.line) {
            int ca = line == sa.line ? sa.col : 0;
            int cb = line == sb.line ? sb.col : (int)d->text.Line(line).size();
            Color sc = focus ? g_th.selection : g_th.selectionInactive;
            if (cb > ca) {
                std::vector<DWRITE_HIT_TEST_METRICS> hm; RangeRects(ll.L, (UINT32)(ca), (UINT32)(cb - ca), hm);
                for (auto& m : hm) FillR(RF(x0 + m.left, y + m.top, m.width, m.height), sc);
            }
            if (line < sb.line) {   // newline marker
                FLOAT px, py; DWRITE_HIT_TEST_METRICS m;
                ll.L->HitTestTextPosition((UINT32)d->text.Line(line).size(), FALSE, &px, &py, &m);
                FillR(RF(x0 + px, y + py, s_spaceW * 0.6f, s_lineH), sc);
            }
        }
        g_rt->DrawTextLayout({ x0, y }, ll.L, Brush(g_th.text), (D2D1_DRAW_TEXT_OPTIONS)4 /*ENABLE_COLOR_FONT*/);
        // spelling
        if (g_set.spell && !d->large && SpellAvailable() && d->lang != np::L_JSON && d->lang != np::L_ASM) {
            SpellFor(d, line, ll, errs);
            for (auto& e : errs) {
                std::vector<DWRITE_HIT_TEST_METRICS> hm; RangeRects(ll.L, (UINT32)(e.first), (UINT32)(e.second), hm);
                for (auto& m : hm) Squiggle(x0 + m.left, x0 + m.left + m.width, y + m.top + m.height - 3, g_th.squiggle);
            }
        }
        // folded marker
        if (d->text.Flags(line) & np::LF_FOLDED) {
            const np::FoldRegion* fr = RegionAt(d, line);
            float fx = x0 + std::min(ll.width, ll.wrapW > 0 ? ll.wrapW : ll.width) + 8;
            RectF box = RF(fx, y + 2, s_spaceW * 3 + 8, s_lineH - 4);
            FillRR(box, 3, g_th.codeBg); StrokeRR(box, 3, g_th.border);
            for (int k = -1; k <= 1; ++k) g_rt->FillEllipse(D2D1::Ellipse({ (box.left + box.right) / 2 + k * 4.5f, (box.top + box.bottom) / 2 }, 1.3f, 1.3f), Brush(g_th.dim));
            (void)fr;
        }
        // caret
        if (line == d->caret.line && focus && g_caretOn) {
            FLOAT px, py; DWRITE_HIT_TEST_METRICS m;
            ll.L->HitTestTextPosition(d->caret.col, FALSE, &px, &py, &m);
            if (d->overwrite && !hasSel) FillR(RF(x0 + px, y + py, std::max(m.width, s_spaceW), m.height), Alpha(g_th.text, 0.35f));
            else FillR(RF(std::floor(x0 + px), y + py, 1.6f, m.height), g_th.text);
        }
        g_rt->PopAxisAlignedClip();
        // gutter
        if (g_set.lineNumbers) {
            wchar_t num[16]; swprintf(num, 16, L"%d", line + 1);
            float numR = s_r.gutter.right - (g_set.folding ? 18 : 8);
            s_fmtGutter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            g_rt->DrawText(num, (UINT32)wcslen(num), s_fmtGutter, { s_r.gutter.left, y + 1, numR, y + s_lineH },
                           Brush(line == d->caret.line ? g_th.gutterTextActive : g_th.gutterText));
        }
        if (g_set.folding) {
            if (const np::FoldRegion* fr = RegionAt(d, line)) {
                bool folded = d->text.Flags(line) & np::LF_FOLDED;
                if (folded || s_hoverGutter)
                    DrawIcon(folded ? I_CHEV_RIGHT : I_CHEV_DOWN, s_r.gutter.right - 9, y + s_lineH / 2, 11, folded ? g_th.text : g_th.dim, 1.3f);
                (void)fr;
            }
        }
        y += h;
    }
    g_rt->PopAxisAlignedClip();
    if (lv) LiveAfterPaint(d);
    // scrollbars (overlay style)
    int total = RowCount(d);
    int vis = VisibleRowCount();
    if (total > 1) {
        float trackH = RH(s_r.vbar) - 4;
        float th = std::max(24.f, trackH * std::min(1.f, (float)vis / (total + vis - 1)));
        float ty = s_r.vbar.top + 2 + (trackH - th) * (total > 1 ? (float)d->topRow / (total - 1) : 0);
        float w = (s_hoverV || s_drag == 3) ? 7.f : 3.f;
        FillRR(RF(s_r.vbar.right - w - 2.5f, ty, w, th), w / 2, g_th.scrollThumb);
        // find markers on the track
        if (g_findActive && !g_findMatches.empty() && g_findMatches.size() < 5000) {
            int lastY = -1;
            for (auto& m : g_findMatches) {
                int row = RowOf(d, d->text.PosAt(m.off).line);
                int py = (int)(s_r.vbar.top + 2 + trackH * row / std::max(1, total));
                if (py == lastY) continue; lastY = py;
                FillR(RF(s_r.vbar.left + 1, (float)py, 4, 2), g_th.findCur);
            }
        }
    }
    if (!g_set.wordWrap) {
        float viewW = RW(s_r.text), contentW = s_maxW + s_spaceW * 4;
        if (contentW > viewW) {
            float trackW = RW(s_r.hbar) - 4;
            float tw = std::max(24.f, trackW * viewW / contentW);
            float tx = s_r.hbar.left + 2 + (trackW - tw) * std::min(1.f, d->scrollX / (contentW - viewW));
            float h = (s_hoverH || s_drag == 4) ? 7.f : 3.f;
            FillRR(RF(tx, s_r.hbar.bottom - h - 2.5f, tw, h), h / 2, g_th.scrollThumb);
        }
    }
}

// ------------------------------------------------------------------ positions
static float RowHeight(Doc* d, int row) { return GetLayout(d, RowLine(d, row)).height; }

Pos EditorHit(Doc* d, float x, float y) {
    if (d->loading) return {};
    EditorUpdateRows(d);
    if ((RowCount(d) == 0)) return {};
    float ry = s_r.text.top + 2;
    int row = d->topRow;
    if (y < ry) {   // above: walk upward
        while (row > 0 && y < ry) { --row; ry -= RowHeight(d, row); }
    } else {
        while (row < RowCount(d)) {
            float h = RowHeight(d, row);
            if (y < ry + h || row == RowCount(d) - 1) break;
            ry += h; ++row;
        }
    }
    row = std::min(row, RowCount(d) - 1);
    int line = RowLine(d, row);
    const LineLayout& ll = GetLayout(d, line);
    BOOL trailing, inside; DWRITE_HIT_TEST_METRICS m;
    ll.L->HitTestPoint(x - TextX0(d), std::max(0.f, std::min(y - ry, ll.height - 1)), &trailing, &inside, &m);
    int col = (int)m.textPosition + (trailing ? (int)m.length : 0);
    return d->text.Clamp({ line, col });
}
RectF EditorCaretRect(Doc* d) {
    if (d->loading) return RF(s_r.text.left, s_r.text.top, 1, s_lineH);
    EditorUpdateRows(d);
    int row = RowOf(d, d->caret.line);
    float y = s_r.text.top + 2;
    if (row < d->topRow) return RF(s_r.text.left, s_r.text.top, 1, s_lineH);
    for (int r = d->topRow; r < row && r < RowCount(d); ++r) { y += RowHeight(d, r); if (y > s_r.text.bottom) break; }
    const LineLayout& ll = GetLayout(d, d->caret.line);
    FLOAT px, py; DWRITE_HIT_TEST_METRICS m;
    ll.L->HitTestTextPosition(d->caret.col, FALSE, &px, &py, &m);
    return RF(TextX0(d) + px, y + py, 2, m.height);
}

// ---- accessibility helpers (UI Automation): geometry of the rows painted last frame
bool EditorVisibleLines(Doc* d, int& first, int& last) {
    if (s_rowLine.empty() || d != s_cacheDoc) return false;
    first = s_rowLine.front(); last = s_rowLine.back();
    for (size_t i = 0; i < s_rowY.size(); ++i) if (s_rowY[i] > s_r.text.bottom) { last = s_rowLine[i > 0 ? i - 1 : 0]; break; }
    return true;
}
void EditorRangeRects(Doc* d, Pos a, Pos b, std::vector<RectF>& out) {
    out.clear();
    if (s_rowLine.empty() || d != s_cacheDoc) return;
    float x0 = TextX0(d);
    for (size_t i = 0; i < s_rowLine.size(); ++i) {
        int line = s_rowLine[i];
        if (line < a.line || line > b.line) continue;
        float y = s_rowY[i];
        if (y > s_r.text.bottom) break;
        const LineLayout& ll = GetLayout(d, line);
        int ca = line == a.line ? a.col : 0, cb = line == b.line ? b.col : (int)d->text.Line(line).size();
        if (cb <= ca) {   // empty line / degenerate: a caret-wide rect
            FLOAT px, py; DWRITE_HIT_TEST_METRICS m; ll.L->HitTestTextPosition(ca, FALSE, &px, &py, &m);
            out.push_back(RF(x0 + px, y + py, 2, std::max(m.height, s_lineH)));
            continue;
        }
        std::vector<DWRITE_HIT_TEST_METRICS> hm; RangeRects(ll.L, (UINT32)ca, (UINT32)(cb - ca), hm);
        for (auto& m : hm) out.push_back(RF(x0 + m.left, y + m.top, m.width, m.height));
    }
    for (auto& r : out) { r.left = std::max(r.left, s_r.text.left); r.right = std::min(r.right, s_r.text.right); }
}
Pos EditorHitPublic(Doc* d, float x, float y) { return EditorHit(d, x, y); }

static bool IsLowSur(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
static Pos Left(Doc* d, Pos p, bool word) {
    if (p.col == 0) return p.line > 0 ? Pos{ p.line - 1, (int)d->text.Line(p.line - 1).size() } : p;
    const std::wstring& s = d->text.Line(p.line);
    if (!word) { --p.col; if (p.col > 0 && IsLowSur(s[p.col])) --p.col; return p; }
    while (p.col > 0 && iswspace(s[p.col - 1])) --p.col;
    if (p.col > 0 && np::IsWordChar(s[p.col - 1])) { while (p.col > 0 && np::IsWordChar(s[p.col - 1])) --p.col; }
    else if (p.col > 0) { while (p.col > 0 && !np::IsWordChar(s[p.col - 1]) && !iswspace(s[p.col - 1])) --p.col; }
    return p;
}
static Pos Right(Doc* d, Pos p, bool word) {
    const std::wstring& s = d->text.Line(p.line);
    if (p.col >= (int)s.size()) return p.line + 1 < d->text.LineCount() ? Pos{ p.line + 1, 0 } : p;
    if (!word) { ++p.col; if (p.col < (int)s.size() && IsLowSur(s[p.col])) ++p.col; return p; }
    int n = (int)s.size();
    if (np::IsWordChar(s[p.col])) { while (p.col < n && np::IsWordChar(s[p.col])) ++p.col; }
    else if (!iswspace(s[p.col])) { while (p.col < n && !np::IsWordChar(s[p.col]) && !iswspace(s[p.col])) ++p.col; }
    while (p.col < n && iswspace(s[p.col])) ++p.col;
    return p;
}
static Pos Vertical(Doc* d, Pos p, int dir) {
    EditorUpdateRows(d);
    const LineLayout& ll = GetLayout(d, p.line);
    FLOAT px, py; DWRITE_HIT_TEST_METRICS m;
    ll.L->HitTestTextPosition(p.col, FALSE, &px, &py, &m);
    if (d->desiredX < 0) d->desiredX = px;
    float ty = py + (dir > 0 ? m.height * 1.5f : -m.height * 0.5f);
    int line = p.line;
    const LineLayout* L = &ll;
    if (ty < 0) {
        int row = RowOf(d, line);
        if (row == 0) return { line, 0 };
        line = RowLine(d, row - 1); L = &GetLayout(d, line); ty = L->height - s_lineH * 0.5f;
    } else if (ty >= ll.height) {
        int row = RowOf(d, line);
        if (row + 1 >= RowCount(d)) return { line, (int)d->text.Line(line).size() };
        line = RowLine(d, row + 1); L = &GetLayout(d, line); ty = s_lineH * 0.5f;
    }
    BOOL tr, in; DWRITE_HIT_TEST_METRICS hm;
    L->L->HitTestPoint(d->desiredX, ty, &tr, &in, &hm);
    return d->text.Clamp({ line, (int)hm.textPosition + (tr ? (int)hm.length : 0) });
}

void EditorEnsureCaret(Doc* d) {
    if (d->loading) return;
    EditorUnfoldFor(d, d->caret.line);
    EditorUpdateRows(d);
    if (RH(s_r.text) <= 0) return;
    int row = RowOf(d, d->caret.line);
    int vis = VisibleRowCount();
    if (row < d->topRow) d->topRow = row;
    else {
        // far below the viewport: jump first so we only ever measure ~one screen of rows
        if (row - d->topRow > vis * 2) d->topRow = std::max(0, row - vis + 1);
        // ensure bottom of caret row fits
        for (int guard = 0; guard < 100000; ++guard) {
            float y = s_r.text.top + 2; bool fits = false;
            for (int r = d->topRow; r <= row; ++r) { y += RowHeight(d, r); }
            fits = y <= s_r.text.bottom || d->topRow >= row;
            if (fits) break;
            int jump = std::max(1, (row - d->topRow) - VisibleRowCount() + 1);
            d->topRow = std::min(row, d->topRow + (guard == 0 ? jump : 1));
        }
    }
    if (!g_set.wordWrap) {
        const LineLayout& ll = GetLayout(d, d->caret.line);
        FLOAT px, py; DWRITE_HIT_TEST_METRICS m;
        ll.L->HitTestTextPosition(d->caret.col, FALSE, &px, &py, &m);
        float viewW = RW(s_r.text) - 12;
        if (px < d->scrollX) d->scrollX = std::max(0.f, px - viewW / 4);
        else if (px > d->scrollX + viewW) d->scrollX = px - viewW * 3 / 4;
    } else d->scrollX = 0;
}

void EditorSetCaret(Doc* d, Pos p, bool extend) {
    p = LiveFixCaret(d, d->text.Clamp(p), d->caret);
    d->caret = d->text.Clamp(p);
    if (!extend) d->anchor = d->caret;
    g_caretOn = true;
    d->text.BreakCoalesce();
    MdSyncTo(d, (RowCount(d) == 0) ? 0 : RowLine(d, std::min(d->topRow, RowCount(d) - 1)));
}
void EditorSelect(Doc* d, Pos a, Pos b) { d->anchor = d->text.Clamp(a); d->caret = d->text.Clamp(b); EditorEnsureCaret(d); Invalidate(); }
bool EditorHasSel(Doc* d) { return d->caret != d->anchor; }
std::wstring EditorSelection(Doc* d) { return d->text.GetText(d->anchor, d->caret); }

// ------------------------------------------------------------------ editing
void EditorAfterEdit(Doc* d);
static void Edited(Doc* d) { EditorAfterEdit(d); }
void EditorAfterEdit(Doc* d) {
    int from = d->text.DirtyFrom();
    d->lex.Invalidate(from);
    d->text.ClearDirty();
    d->rowsDirty = true;
    s_lastEdit = GetTickCount64();
    g_caretOn = true;
    d->desiredX = -1;
    UpdateTitle();
    Invalidate();
}
static Pos ReplaceRange(Doc* d, Pos a, Pos b, const std::wstring& s, bool typing = false) {
    Pos e = d->text.Replace(a, b, s, d->caret, d->anchor, typing);
    d->caret = d->anchor = e;
    Edited(d);
    EditorEnsureCaret(d);
    return e;
}
void EditorDeleteSel(Doc* d) { if (EditorHasSel(d)) ReplaceRange(d, d->anchor, d->caret, L""); }

void EditorInsert(Doc* d, const std::wstring& s, bool typing) {
    Pos a = d->anchor, b = d->caret;
    if (b < a) std::swap(a, b);
    if (a == b && d->overwrite && typing && s.size() == 1 && b.col < (int)d->text.Line(b.line).size()) b.col++;
    ReplaceRange(d, a, b, s, typing && a == d->caret && !EditorHasSel(d));
}

static std::wstring LeadingWs(const std::wstring& s, int upto) {
    int i = 0; while (i < upto && i < (int)s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
    return s.substr(0, i);
}
static std::wstring IndentUnit() { return g_set.insertSpaces ? std::wstring(g_set.tabSize, L' ') : L"\t"; }

static void IndentLines(Doc* d, bool out) {
    Pos a = d->anchor, b = d->caret; bool caretFirst = b < a; if (b < a) std::swap(a, b);
    int l0 = a.line, l1 = (b.col == 0 && b.line > a.line) ? b.line - 1 : b.line;
    d->text.BeginGroup();
    for (int l = l0; l <= l1; ++l) {
        const std::wstring& s = d->text.Line(l);
        if (!out) { if (!s.empty()) d->text.Replace({ l, 0 }, { l, 0 }, IndentUnit(), d->caret, d->anchor); }
        else {
            int n = 0;
            if (!s.empty() && s[0] == L'\t') n = 1;
            else while (n < g_set.tabSize && n < (int)s.size() && s[n] == L' ') ++n;
            if (n) d->text.Replace({ l, 0 }, { l, n }, L"", d->caret, d->anchor);
        }
    }
    d->text.EndGroup();
    Pos na{ l0, 0 }, nb{ l1, (int)d->text.Line(l1).size() };
    if (caretFirst) { d->caret = na; d->anchor = nb; } else { d->anchor = na; d->caret = nb; }
    Edited(d);
}

static void ToggleComment(Doc* d) {
    const wchar_t* pre = np::LineCommentPrefix(d->lang);
    if (!pre) return;
    std::wstring p = pre, pt = p; while (!pt.empty() && pt.back() == L' ') pt.pop_back();
    Pos a = d->anchor, b = d->caret; if (b < a) std::swap(a, b);
    int l0 = a.line, l1 = (b.col == 0 && b.line > a.line) ? b.line - 1 : b.line;
    bool all = true; int minInd = 1 << 30;
    for (int l = l0; l <= l1; ++l) {
        const std::wstring& s = d->text.Line(l);
        size_t k = s.find_first_not_of(L" \t");
        if (k == std::wstring::npos) continue;
        minInd = std::min(minInd, (int)k);
        if (s.compare(k, pt.size(), pt) != 0) all = false;
    }
    if (minInd == (1 << 30)) return;
    d->text.BeginGroup();
    for (int l = l0; l <= l1; ++l) {
        const std::wstring& s = d->text.Line(l);
        size_t k = s.find_first_not_of(L" \t");
        if (k == std::wstring::npos) continue;
        if (all) {
            size_t len = s.compare(k, p.size(), p) == 0 ? p.size() : pt.size();
            d->text.Replace({ l, (int)k }, { l, (int)(k + len) }, L"", d->caret, d->anchor);
        } else d->text.Replace({ l, minInd }, { l, minInd }, p, d->caret, d->anchor);
    }
    d->text.EndGroup();
    d->anchor = { l0, 0 }; d->caret = { l1, (int)d->text.Line(l1).size() };
    Edited(d);
}

static void MoveLines(Doc* d, int dir) {
    Pos a = d->anchor, b = d->caret; if (b < a) std::swap(a, b);
    int l0 = a.line, l1 = (b.col == 0 && b.line > a.line) ? b.line - 1 : b.line;
    if ((dir < 0 && l0 == 0) || (dir > 0 && l1 + 1 >= d->text.LineCount())) return;
    std::wstring block = d->text.GetText({ l0, 0 }, { l1, (int)d->text.Line(l1).size() });
    d->text.BeginGroup();
    if (dir < 0) {
        std::wstring above = d->text.Line(l0 - 1);
        d->text.Replace({ l0 - 1, 0 }, { l1, (int)d->text.Line(l1).size() }, block + L"\n" + above, d->caret, d->anchor);
    } else {
        std::wstring below = d->text.Line(l1 + 1);
        d->text.Replace({ l0, 0 }, { l1 + 1, (int)below.size() }, below + L"\n" + block, d->caret, d->anchor);
    }
    d->text.EndGroup();
    d->anchor.line += dir; d->caret.line += dir;
    Edited(d); EditorEnsureCaret(d);
}

static void TransformSel(Doc* d, int mode) {
    Pos a = d->anchor, b = d->caret; if (b < a) std::swap(a, b);
    if (mode == C_TRIMWS || mode == C_SORTLINES) {
        if (a == b || mode == C_TRIMWS) { a = { 0, 0 }; b = d->text.End(); if (mode == C_SORTLINES) {} }
        a.col = 0; b.col = (int)d->text.Line(b.line).size();
        std::vector<std::wstring> ls;
        for (int l = a.line; l <= b.line; ++l) ls.push_back(d->text.Line(l));
        if (mode == C_TRIMWS) for (auto& s : ls) { size_t e = s.find_last_not_of(L" \t"); s.resize(e == std::wstring::npos ? 0 : e + 1); }
        else std::stable_sort(ls.begin(), ls.end(), [](const std::wstring& x, const std::wstring& y) { return _wcsicmp(x.c_str(), y.c_str()) < 0; });
        std::wstring out; for (size_t i = 0; i < ls.size(); ++i) { if (i) out += L'\n'; out += ls[i]; }
        if (out == d->text.GetText(a, b)) return;
        Pos keep = d->caret;
        ReplaceRange(d, a, b, out);
        d->caret = d->anchor = d->text.Clamp(keep);
        return;
    }
    if (a == b) return;
    std::wstring s = d->text.GetText(a, b);
    for (auto& c : s) c = (wchar_t)(mode == C_UPPER ? towupper(c) : towlower(c));
    bool caretFirst = d->caret < d->anchor;
    ReplaceRange(d, a, b, s);
    Pos e = d->caret;
    if (caretFirst) { d->caret = a; d->anchor = e; } else { d->anchor = a; d->caret = e; }
}

void EditorCommand(Doc* d, int cmd) {
    switch (cmd) {
    case C_UNDO: if (d->text.Undo(d->caret, d->anchor)) { d->caret = d->text.Clamp(d->caret); d->anchor = d->text.Clamp(d->anchor); Edited(d); EditorEnsureCaret(d); } break;
    case C_REDO: if (d->text.Redo(d->caret, d->anchor)) { d->caret = d->text.Clamp(d->caret); d->anchor = d->text.Clamp(d->anchor); Edited(d); EditorEnsureCaret(d); } break;
    case C_CUT:
        if (!EditorHasSel(d)) {   // cut whole line (VS-style)
            int l = d->caret.line;
            Pos a{ l, 0 }, b = l + 1 < d->text.LineCount() ? Pos{ l + 1, 0 } : Pos{ l, (int)d->text.Line(l).size() };
            ClipboardSet(d->text.GetText(a, b) + (l + 1 < d->text.LineCount() ? L"" : L"\n"));
            ReplaceRange(d, a, b, L"");
        } else { ClipboardSet(EditorSelection(d)); EditorDeleteSel(d); }
        break;
    case C_COPY: if (EditorHasSel(d)) ClipboardSet(EditorSelection(d)); break;
    case C_PASTE: { std::wstring s = ClipboardGet(); if (!s.empty()) { d->text.BreakCoalesce(); EditorInsert(d, s); } break; }
    case C_DELETE:
        if (EditorHasSel(d)) EditorDeleteSel(d);
        else { Pos n = Right(d, d->caret, false); if (n != d->caret) ReplaceRange(d, d->caret, n, L""); }
        break;
    case C_SELALL: d->anchor = { 0, 0 }; d->caret = d->text.End(); Invalidate(); break;
    case C_DUPLINE: {
        if (EditorHasSel(d)) { std::wstring s = EditorSelection(d); Pos a = std::max(d->caret, d->anchor); d->caret = d->anchor = a; EditorInsert(d, s); }
        else { int l = d->caret.line; std::wstring s = d->text.Line(l); Pos c = d->caret;
               ReplaceRange(d, { l, (int)s.size() }, { l, (int)s.size() }, L"\n" + s); d->caret = d->anchor = { c.line + 1, c.col }; }
        break; }
    case C_TOGGLECOMMENT: ToggleComment(d); break;
    case C_MOVEUP: MoveLines(d, -1); break;
    case C_MOVEDOWN: MoveLines(d, 1); break;
    case C_UPPER: case C_LOWER: case C_TRIMWS: case C_SORTLINES: TransformSel(d, cmd); break;
    case C_FOLDCUR: EditorToggleFold(d, d->caret.line, 1); break;
    case C_UNFOLDCUR: EditorToggleFold(d, d->caret.line, 0); break;
    case C_FOLDALL: EditorFoldAll(d, true); break;
    case C_UNFOLDALL: EditorFoldAll(d, false); break;
    }
    Invalidate();
}

bool EditorKey(Doc* d, WPARAM vk) {
    if (LiveKey(d, vk)) return true;
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
    Pos p = d->caret;
    bool keepX = false;
    switch (vk) {
    case VK_LEFT:
        if (!shift && EditorHasSel(d) && !ctrl) p = std::min(d->caret, d->anchor); else p = Left(d, p, ctrl);
        break;
    case VK_RIGHT:
        if (!shift && EditorHasSel(d) && !ctrl) p = std::max(d->caret, d->anchor); else p = Right(d, p, ctrl);
        break;
    case VK_UP:
        if (alt) { MoveLines(d, -1); return true; }
        if (ctrl) { EditorWheel(d, 120 / 3, false); return true; }
        p = Vertical(d, p, -1); keepX = true; break;
    case VK_DOWN:
        if (alt) { MoveLines(d, 1); return true; }
        if (ctrl) { EditorWheel(d, -120 / 3, false); return true; }
        p = Vertical(d, p, 1); keepX = true; break;
    case VK_HOME: {
        if (ctrl) { p = { 0, 0 }; break; }
        const std::wstring& s = d->text.Line(p.line);
        int fn = 0; while (fn < (int)s.size() && (s[fn] == L' ' || s[fn] == L'\t')) ++fn;
        p.col = (p.col == fn) ? 0 : fn; break; }
    case VK_END: p = ctrl ? d->text.End() : Pos{ p.line, (int)d->text.Line(p.line).size() }; break;
    case VK_PRIOR: case VK_NEXT: {
        int n = VisibleRowCount() - 1, dir = vk == VK_PRIOR ? -1 : 1;
        EditorUpdateRows(d);
        int row = RowOf(d, p.line), nr = std::max(0, std::min(RowCount(d) - 1, row + dir * n));
        d->topRow = std::max(0, std::min(RowCount(d) - 1, d->topRow + dir * n));
        const LineLayout& ll = GetLayout(d, p.line);
        FLOAT px, py; DWRITE_HIT_TEST_METRICS m; ll.L->HitTestTextPosition(p.col, FALSE, &px, &py, &m);
        if (d->desiredX < 0) d->desiredX = px;
        const LineLayout& nl = GetLayout(d, RowLine(d, nr));
        BOOL tr, in; DWRITE_HIT_TEST_METRICS hm; nl.L->HitTestPoint(d->desiredX, s_lineH / 2, &tr, &in, &hm);
        p = d->text.Clamp({ RowLine(d, nr), (int)hm.textPosition + (tr ? (int)hm.length : 0) });
        keepX = true; break; }
    case VK_BACK: {
        if (EditorHasSel(d)) { EditorDeleteSel(d); return true; }
        Pos a = Left(d, p, ctrl);
        if (!ctrl && g_set.insertSpaces && p.col > 0) {   // delete back to tab stop through spaces
            const std::wstring& s = d->text.Line(p.line);
            int stop = ((p.col - 1) / g_set.tabSize) * g_set.tabSize, c = p.col;
            while (c > stop && s[c - 1] == L' ') --c;
            if (c < p.col - 1) a = { p.line, c };
        }
        if (a != p) ReplaceRange(d, a, p, L"");
        return true; }
    case VK_DELETE: {
        if (shift && !EditorHasSel(d)) { EditorCommand(d, C_CUT); return true; }
        if (EditorHasSel(d)) { EditorDeleteSel(d); return true; }
        Pos b = Right(d, p, ctrl);
        if (b != p) ReplaceRange(d, p, b, L"");
        return true; }
    case VK_RETURN: {
        if (ctrl) return false;
        std::wstring ins = L"\n";
        if (g_set.autoIndent) {
            Pos a = std::min(d->caret, d->anchor);
            ins += LeadingWs(d->text.Line(a.line), a.col);
            const std::wstring& s = d->text.Line(a.line);
            if (a.col > 0 && (s[a.col - 1] == L'{' || s[a.col - 1] == L'[' || s[a.col - 1] == L'(' ||
                (d->lang == np::L_PY && s[a.col - 1] == L':'))) ins += IndentUnit();
        }
        d->text.BreakCoalesce();
        EditorInsert(d, ins);
        return true; }
    case VK_TAB: {
        if (ctrl) return false;
        Pos a = d->anchor, b = d->caret; if (b < a) std::swap(a, b);
        if (shift || (a.line != b.line)) { IndentLines(d, shift); return true; }
        if (g_set.insertSpaces) { int n = g_set.tabSize - (p.col % g_set.tabSize); EditorInsert(d, std::wstring(n, L' '), true); }
        else EditorInsert(d, L"\t", true);
        return true; }
    case VK_INSERT:
        if (ctrl) { EditorCommand(d, C_COPY); return true; }
        if (shift) { EditorCommand(d, C_PASTE); return true; }
        d->overwrite = !d->overwrite; Invalidate(); return true;
    case VK_ESCAPE:
        if (EditorHasSel(d)) { d->anchor = d->caret; Invalidate(); return true; }
        return false;
    default: return false;
    }
    if (!keepX) d->desiredX = -1;
    EditorSetCaret(d, p, shift);
    EditorEnsureCaret(d);
    Invalidate();
    return true;
}

void EditorChar(Doc* d, wchar_t ch) {
    if (ch < 0x20 && ch != L'\t') return;
    if (ch == 0x7F) return;
    if (ch == L'\t') return;   // handled in EditorKey
    if (LiveChar(d, ch)) return;
    EditorInsert(d, std::wstring(1, ch), true);
}

void EditorWheel(Doc* d, int delta, bool horizontal) {
    EditorUpdateRows(d);
    if (horizontal || (GetKeyState(VK_SHIFT) < 0 && !g_set.wordWrap)) {
        if (g_set.wordWrap) return;
        float maxX = std::max(0.f, s_maxW + s_spaceW * 4 - RW(s_r.text));
        d->scrollX = std::max(0.f, std::min(maxX, d->scrollX - delta * s_spaceW * 6 / 120.f));
    } else {
        UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        int n = (int)std::lround(-delta * (int)lines / 120.0);
        if (n == 0) n = delta > 0 ? -1 : 1;
        d->topRow = std::max(0, std::min(RowCount(d) - 1, d->topRow + n));
        MdSyncTo(d, (RowCount(d) == 0) ? 0 : RowLine(d, d->topRow));
    }
    Invalidate();
}

bool EditorWordAt(Doc* d, float x, float y, Pos& a, Pos& b) {
    Pos p = EditorHit(d, x, y);
    const std::wstring& s = d->text.Line(p.line);
    int s0 = p.col, e = p.col;
    if (s0 < (int)s.size() && !np::IsWordChar(s[s0]) && s0 > 0 && np::IsWordChar(s[s0 - 1])) --s0, --e;
    if (s0 >= (int)s.size() || !np::IsWordChar(s[s0])) { a = b = p; return false; }
    while (s0 > 0 && (np::IsWordChar(s[s0 - 1]) || s[s0 - 1] == L'\'')) --s0;
    while (e < (int)s.size() && (np::IsWordChar(s[e]) || (s[e] == L'\'' && e + 1 < (int)s.size() && np::IsWordChar(s[e + 1])))) ++e;
    a = { p.line, s0 }; b = { p.line, e };
    return true;
}

static void SetTopFromThumb(Doc* d, float y) {
    float trackH = RH(s_r.vbar) - 4;
    int total = RowCount(d), vis = VisibleRowCount();
    float th = std::max(24.f, trackH * std::min(1.f, (float)vis / (total + vis - 1)));
    float t = (y - s_dragGrab - s_r.vbar.top - 2) / std::max(1.f, trackH - th);
    d->topRow = std::max(0, std::min(total - 1, (int)std::lround(t * (total - 1))));
    MdSyncTo(d, (RowCount(d) == 0) ? 0 : RowLine(d, d->topRow));
}

bool EditorMouse(Doc* d, UINT msg, float x, float y, WPARAM wp) {
    if (d->loading) return In(s_r.area, x, y);
    s_lastMx = x; s_lastMy = y;
    EditorUpdateRows(d);
    switch (msg) {
    case WM_MOUSEMOVE: {
        bool hv = In(s_r.vbar, x, y), hh = In(s_r.hbar, x, y), hg = In(s_r.gutter, x, y);
        if (hv != s_hoverV || hh != s_hoverH || hg != s_hoverGutter) { s_hoverV = hv; s_hoverH = hh; s_hoverGutter = hg; Invalidate(); }
        if (!(wp & MK_LBUTTON)) {
            s_drag = 0;
            if (In(s_r.text, x, y) && LiveOn(d)) {
                if (LiveOverTable(x, y)) SetCursor(LoadCursor(nullptr, IDC_ARROW));
                else if ((wp & MK_CONTROL) && LiveHoverLink(d, EditorHit(d, x, y))) SetCursor(LoadCursor(nullptr, IDC_HAND));
            }
            return In(s_r.area, x, y);
        }
        if (s_drag == 1) {
            Pos p = EditorHit(d, x, y);
            if (s_clickCount == 2) {   // word-wise extension
                Pos a, b; EditorWordAt(d, x, y, a, b); if (a == b) a = b = p;
                if (p < s_dragOrigA) { d->anchor = s_dragOrigB; d->caret = a; } else { d->anchor = s_dragOrigA; d->caret = b; }
            } else if (s_clickCount >= 3) {
                Pos a{ p.line, 0 }, b = p.line + 1 < d->text.LineCount() ? Pos{ p.line + 1, 0 } : Pos{ p.line, (int)d->text.Line(p.line).size() };
                if (p < s_dragOrigA) { d->anchor = s_dragOrigB; d->caret = a; } else { d->anchor = s_dragOrigA; d->caret = b; }
            } else d->caret = p;
            EditorEnsureCaret(d); Invalidate();
        } else if (s_drag == 2) {
            Pos p = EditorHit(d, s_r.text.left + 1, y);
            Pos b = p.line + 1 < d->text.LineCount() ? Pos{ p.line + 1, 0 } : Pos{ p.line, (int)d->text.Line(p.line).size() };
            if (p.line < s_dragOrigA.line) { d->anchor = s_dragOrigB; d->caret = { p.line, 0 }; } else { d->anchor = s_dragOrigA; d->caret = b; }
            EditorEnsureCaret(d); Invalidate();
        } else if (s_drag == 3) { SetTopFromThumb(d, y); Invalidate(); }
        else if (s_drag == 4) {
            float trackW = RW(s_r.hbar) - 4, viewW = RW(s_r.text), contentW = s_maxW + s_spaceW * 4;
            float tw = std::max(24.f, trackW * viewW / contentW);
            float t = (x - s_dragGrab - s_r.hbar.left - 2) / std::max(1.f, trackW - tw);
            d->scrollX = std::max(0.f, std::min(contentW - viewW, t * (contentW - viewW)));
            Invalidate();
        }
        return true; }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        if (!In(s_r.area, x, y)) return false;
        SetCapture(g_hwnd);
        bool shift = (wp & MK_SHIFT) != 0;
        if (In(s_r.vbar, x, y)) {
            float trackH = RH(s_r.vbar) - 4; int total = RowCount(d), vis = VisibleRowCount();
            float th = std::max(24.f, trackH * std::min(1.f, (float)vis / (total + vis - 1)));
            float ty = s_r.vbar.top + 2 + (trackH - th) * (total > 1 ? (float)d->topRow / (total - 1) : 0);
            if (y >= ty && y < ty + th) s_dragGrab = y - ty;
            else { s_dragGrab = th / 2; SetTopFromThumb(d, y); }
            s_drag = 3; Invalidate(); return true;
        }
        if (In(s_r.hbar, x, y)) { s_drag = 4; s_dragGrab = 12; return true; }
        DWORD now = GetMessageTime();
        POINT pt{ (LONG)x, (LONG)y };
        if (now - s_lastClick <= GetDoubleClickTime() && std::abs(pt.x - s_lastClickPt.x) < 4 && std::abs(pt.y - s_lastClickPt.y) < 4) s_clickCount++;
        else s_clickCount = 1;
        s_lastClick = now; s_lastClickPt = pt;
        if (In(s_r.gutter, x, y)) {
            if (g_set.folding && x >= s_r.gutter.right - 16) {
                Pos p = EditorHit(d, s_r.text.left + 1, y);
                if (RegionAt(d, p.line)) { EditorToggleFold(d, p.line); ReleaseCapture(); return true; }
            }
            Pos p = EditorHit(d, s_r.text.left + 1, y);
            Pos a{ p.line, 0 }, b = p.line + 1 < d->text.LineCount() ? Pos{ p.line + 1, 0 } : Pos{ p.line, (int)d->text.Line(p.line).size() };
            d->anchor = a; d->caret = b; s_dragOrigA = a; s_dragOrigB = b; s_drag = 2;
            Invalidate(); return true;
        }
        Pos p = EditorHit(d, x, y);
        if (s_clickCount == 1 && LiveMouseDown(d, x, y, p, wp)) { ReleaseCapture(); s_drag = 0; Invalidate(); return true; }
        // clicked inside a folded marker -> unfold
        if (d->text.Flags(p.line) & np::LF_FOLDED && p.col == (int)d->text.Line(p.line).size()) {
            const LineLayout& ll = GetLayout(d, p.line);
            if (x > TextX0(d) + ll.width + 4) { EditorToggleFold(d, p.line, 0); ReleaseCapture(); return true; }
        }
        if (s_clickCount == 2) {
            Pos a, b; if (!EditorWordAt(d, x, y, a, b)) { a = b = p; }
            d->anchor = a; d->caret = b; s_dragOrigA = a; s_dragOrigB = b;
        } else if (s_clickCount >= 3) {
            Pos a{ p.line, 0 }, b = p.line + 1 < d->text.LineCount() ? Pos{ p.line + 1, 0 } : Pos{ p.line, (int)d->text.Line(p.line).size() };
            d->anchor = a; d->caret = b; s_dragOrigA = a; s_dragOrigB = b;
        } else EditorSetCaret(d, p, shift);
        d->desiredX = -1; d->text.BreakCoalesce();
        s_drag = 1; g_caretOn = true; Invalidate();
        return true; }
    case WM_LBUTTONUP:
        if (s_drag) { s_drag = 0; ReleaseCapture(); Invalidate(); return true; }
        return false;
    case WM_RBUTTONDOWN: {
        if (!In(s_r.text, x, y)) return false;
        Pos p = EditorHit(d, x, y);
        Pos a = std::min(d->caret, d->anchor), b = std::max(d->caret, d->anchor);
        if (!(a <= p && p <= b && a != b)) EditorSetCaret(d, p, false);
        Invalidate(); return true; }
    }
    return false;
}

void EditorTick() {
    // autoscroll while dragging outside the text area
    Doc* d = Cur();
    if (!d || s_drag != 1 || d->hex) return;
    if (s_lastMy < s_r.text.top) { d->topRow = std::max(0, d->topRow - 1); EditorMouse(d, WM_MOUSEMOVE, s_lastMx, s_lastMy, MK_LBUTTON); }
    else if (s_lastMy > s_r.text.bottom) { d->topRow = std::min(RowCount(d) - 1, d->topRow + 1); EditorMouse(d, WM_MOUSEMOVE, s_lastMx, s_lastMy, MK_LBUTTON); }
}

} // namespace app
