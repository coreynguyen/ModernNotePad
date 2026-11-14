// mdview.cpp - rendered Markdown preview (headings, emphasis, code, quotes, lists, task
// lists, tables, rules, clickable links) using DirectWrite layouts; scroll-synced to editor.
#include "app.h"
#include <cmath>
#include <shellapi.h>

namespace app {

struct LinkRange { UINT32 start, len; std::wstring url; };
struct MdCell { Com<IDWriteTextLayout> L; std::vector<LinkRange> links; float w = 0, h = 0; };
struct MdItem {
    np::MdKind kind; int srcLine = 0, level = 0, task = -1;
    bool chart = false; np::ChartSpec spec;
    float y = 0, h = 0, indent = 0;
    Com<IDWriteTextLayout> L;
    std::vector<LinkRange> links;
    std::wstring bullet;
    // table
    std::vector<std::vector<MdCell>> cells;
    std::vector<float> colW, rowH;
};
static std::vector<np::MdBlock> s_blocks;
static std::vector<MdItem> s_items;
static uint64_t s_ver = UINT64_MAX; static const Doc* s_doc = nullptr;
static float s_width = 0, s_total = 0, s_zoom = 0;
static int s_pendingLine = -1;
static RectF s_area;
static bool s_dark;

void MdClearCache() { s_ver = UINT64_MAX; s_items.clear(); }

static float Z() { return g_set.zoom / 100.f; }

static Com<IDWriteTextLayout> MakeLayout(const std::vector<np::MdRun>& runs, IDWriteTextFormat* f, float width, Color base,
                                         std::vector<LinkRange>& links, bool bold = false) {
    std::wstring t = np::RunsText(runs);
    Com<IDWriteTextLayout> L;
    g_dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, std::max(10.f, width), 1e6f, &L);
    L->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    L->SetDrawingEffect(Brush(base), { 0, (UINT32)t.size() });
    if (bold) L->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, { 0, (UINT32)t.size() });
    UINT32 pos = 0;
    for (auto& r : runs) {
        DWRITE_TEXT_RANGE rg{ pos, (UINT32)r.text.size() };
        if (r.style & np::MS_STRONG) L->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, rg);
        if (r.style & np::MS_EM) L->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, rg);
        if (r.style & np::MS_STRIKE) L->SetStrikethrough(TRUE, rg);
        if (r.style & np::MS_CODE) {
            L->SetFontFamilyName(g_monoFont.c_str(), rg);
            L->SetDrawingEffect(Brush(g_th.tok[np::T_CODE]), rg);
        }
        if (r.style & np::MS_LINK) {
            L->SetUnderline(TRUE, rg); L->SetDrawingEffect(Brush(g_th.link), rg);
            links.push_back({ pos, (UINT32)r.text.size(), r.url });
        }
        pos += (UINT32)r.text.size();
    }
    return L;
}

static void Build(Doc* d, float width) {
    bool reparse = s_ver != d->text.Version() || s_doc != d;
    if (!reparse && std::fabs(width - s_width) < 0.5f && s_zoom == Z() && s_dark == g_th.dark && !s_items.empty()) return;
    if (reparse) { s_blocks = np::ParseMarkdown(PreviewMarkdown(d)); s_ver = d->text.Version(); s_doc = d; }
    s_width = width; s_zoom = Z(); s_dark = g_th.dark;
    s_items.clear();
    float z = Z(), y = 16 * z, pad = 24 * z;
    float body = 15 * z;
    IDWriteTextFormat* bodyF = Fmt(g_uiFont.c_str(), body);
    for (auto& b : s_blocks) {
        MdItem it; it.kind = b.kind; it.srcLine = b.srcLine; it.level = b.level; it.task = b.task;
        float w = width - 2 * pad;
        switch (b.kind) {
        case np::MdKind::Heading: {
            static const float sz[7] = { 0, 28, 22, 18.5f, 16, 15, 14 };
            y += (b.level <= 2 ? 14 : 10) * z;
            it.L = MakeLayout(b.runs, Fmt(g_uiFont.c_str(), sz[std::min(6, b.level)] * z, DWRITE_FONT_WEIGHT_SEMI_BOLD), w, b.level == 6 ? g_th.dim : g_th.text, it.links, true);
            break; }
        case np::MdKind::Para: it.L = MakeLayout(b.runs, bodyF, w, g_th.text, it.links); break;
        case np::MdKind::Quote:
            it.indent = 16 * z * b.level;
            it.L = MakeLayout(b.runs, Fmt(g_uiFont.c_str(), body, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC), w - it.indent, g_th.dim, it.links);
            break;
        case np::MdKind::ListItem: {
            it.indent = (22 + 22 * b.level) * z;
            if (b.ordered) it.bullet = std::to_wstring(b.number) + L".";
            else if (b.task < 0) { static const wchar_t* bl[3] = { L"•", L"◦", L"▪" }; it.bullet = bl[b.level % 3]; }
            it.L = MakeLayout(b.runs, bodyF, w - it.indent, b.task == 1 ? g_th.dim : g_th.text, it.links);
            if (b.task == 1) { DWRITE_TEXT_METRICS m; (void)m; it.L->SetStrikethrough(TRUE, { 0, (UINT32)np::RunsText(b.runs).size() }); }
            break; }
        case np::MdKind::Code: {
            if ((b.lang == L"chart" || b.lang == L"Chart") && np::ParseChart(b.code, it.spec)) { it.chart = true; it.h = 270 * z; break; }
            std::wstring code = b.code.empty() ? L" " : b.code;
            IDWriteTextFormat* cf = Fmt(g_monoFont.c_str(), 13.5f * z);
            g_dw->CreateTextLayout(code.c_str(), (UINT32)code.size(), cf, 1e6f, 1e6f, &it.L);
            it.L->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            it.L->SetDrawingEffect(Brush(g_th.text), { 0, (UINT32)code.size() });
            it.indent = 12 * z;
            break; }
        case np::MdKind::Rule: it.h = 18 * z; break;
        case np::MdKind::Table: {
            size_t cols = b.align.size();
            it.colW.assign(cols, 0);
            for (size_t r = 0; r < b.rows.size(); ++r) {
                std::vector<MdCell> row(cols);
                for (size_t c = 0; c < cols; ++c) {
                    std::vector<np::MdRun> runs = c < b.rows[r].size() ? b.rows[r][c] : std::vector<np::MdRun>{};
                    row[c].L = MakeLayout(runs, bodyF, 1e5f, g_th.text, row[c].links, r == 0);
                    row[c].L->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    DWRITE_TEXT_METRICS m; row[c].L->GetMetrics(&m);
                    it.colW[c] = std::max(it.colW[c], m.widthIncludingTrailingWhitespace + 20 * z);
                }
                it.cells.push_back(std::move(row));
            }
            float tot = 0; for (float cw : it.colW) tot += cw;
            if (tot > w) for (auto& cw : it.colW) cw *= w / tot;
            for (auto& row : it.cells) {
                float rh = 0;
                for (size_t c = 0; c < cols; ++c) {
                    row[c].L->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                    row[c].L->SetMaxWidth(std::max(10.f, it.colW[c] - 20 * z));
                    DWRITE_TEXT_ALIGNMENT al = b.align[c] == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : b.align[c] == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
                    row[c].L->SetTextAlignment(al);
                    DWRITE_TEXT_METRICS m; row[c].L->GetMetrics(&m);
                    rh = std::max(rh, m.height + 12 * z);
                }
                it.rowH.push_back(rh);
                it.h += rh;
            }
            break; }
        }
        if (it.L && b.kind != np::MdKind::Table && !it.chart) {
            DWRITE_TEXT_METRICS m; it.L->GetMetrics(&m);
            it.h = m.height + (b.kind == np::MdKind::Code ? 20 * z : 0);
        }
        it.y = y;
        y += it.h;
        y += (b.kind == np::MdKind::ListItem ? 4 : b.kind == np::MdKind::Heading ? 8 : 12) * z;
        if (b.kind == np::MdKind::Heading && b.level <= 2) y += 6 * z;
        s_items.push_back(std::move(it));
    }
    s_total = y + 40 * z;
}

// Native chart renderer for ```chart blocks (bar / line / pie); HTML export emits an equivalent SVG.
static void DrawChart(const np::ChartSpec& c, const RectF& r, float z) {
    static const uint32_t pal[] = { 0x4E79A7, 0xF28E2B, 0xE15759, 0x76B7B2, 0x59A14F, 0xEDC948, 0xB07AA1, 0xFF9DA7 };
    FillRR(r, 6 * z, g_th.codeBg);
    float top = r.top + 12 * z;
    if (!c.title.empty()) { Text(c.title, RF(r.left, top, RW(r), 22 * z), UiFmt(14 * z, true), g_th.text, 1); top += 28 * z; }
    float bottom = r.bottom - 30 * z, left = r.left + 52 * z, right = r.right - 16 * z;
    int n = (int)c.data.size();
    double mx = 0, sum = 0; for (auto& p : c.data) { mx = std::max(mx, p.second); sum += std::max(0.0, p.second); }
    if (mx <= 0) mx = 1;
    if (c.type == L"pie" && sum > 0) {
        float cx = (r.left + r.right) / 2 - 60 * z, cy = (top + r.bottom - 10 * z) / 2, rad = std::min((r.bottom - top - 20 * z) / 2, RW(r) / 3);
        double a = -1.5707963;
        for (int i = 0; i < n; ++i) {
            double frac = std::max(0.0, c.data[i].second) / sum, a2 = a + frac * 6.2831853;
            Com<ID2D1PathGeometry> g; g_d2d->CreatePathGeometry(&g);
            Com<ID2D1GeometrySink> k; g->Open(&k);
            k->BeginFigure({ cx, cy }, D2D1_FIGURE_BEGIN_FILLED);
            int segs = std::max(2, (int)(frac * 120));
            for (int sgi = 0; sgi <= segs; ++sgi) { double t = a + (a2 - a) * sgi / segs; k->AddLine({ cx + rad * (float)cos(t), cy + rad * (float)sin(t) }); }
            k->EndFigure(D2D1_FIGURE_END_CLOSED); k->Close();
            g_rt->FillGeometry(g, Brush(Hex(pal[i % 8])));
            g_rt->DrawGeometry(g, Brush(g_th.editor), 1.5f);
            float ly = top + 6 * z + i * 22 * z, lx = cx + rad + 30 * z;
            FillRR(RF(lx, ly + 3 * z, 12 * z, 12 * z), 2, Hex(pal[i % 8]));
            wchar_t b[96]; swprintf(b, 96, L"%ls  %.0f%%", c.data[i].first.c_str(), frac * 100);
            Text(b, RF(lx + 18 * z, ly, 200 * z, 18 * z), UiFmt(12 * z), g_th.text);
            a = a2;
        }
        return;
    }
    for (int k = 0; k <= 4; ++k) {   // grid + axis labels
        float gy = bottom - (bottom - top) * k / 4;
        Line(left, gy, right, gy, Alpha(g_th.border, k ? 0.6f : 1.f));
        wchar_t b[32]; swprintf(b, 32, L"%g", mx * k / 4);
        Text(b, RF(r.left, gy - 9 * z, left - r.left - 6 * z, 18 * z), UiFmt(10.5f * z), g_th.dim, 2);
    }
    float step = n ? (right - left) / n : 0;
    D2D1_POINT_2F prev{};
    for (int i = 0; i < n; ++i) {
        float v = (float)(std::max(0.0, c.data[i].second) / mx), x = left + step * i, h = (bottom - top) * v;
        if (c.type == L"line") {
            D2D1_POINT_2F p{ x + step / 2, bottom - h };
            if (i) g_rt->DrawLine(prev, p, Brush(Hex(pal[0])), 2.5f * z);
            g_rt->FillEllipse(D2D1::Ellipse(p, 3.5f * z, 3.5f * z), Brush(Hex(pal[0])));
            prev = p;
        } else FillRR(RF(x + step * 0.18f, bottom - h, step * 0.64f, h), 3 * z, Hex(pal[i % 8]));
        Text(c.data[i].first, RF(x, bottom + 4 * z, step, 18 * z), UiFmt(11 * z), g_th.dim, 1);
    }
}

void MdSyncTo(Doc* d, int topLine) { if (d && d->mdPreview) s_pendingLine = topLine; }

void MdPaint(Doc* d, const RectF& area) {
    s_area = area;
    FillR(area, g_th.editor);
    float z = Z(), pad = 24 * z;
    Build(d, RW(area) - 12);
    if (s_pendingLine >= 0) {
        float target = 0;
        for (auto& it : s_items) { if (it.srcLine <= s_pendingLine) target = it.y - 12 * z; else break; }
        d->mdScroll = target; s_pendingLine = -1;
    }
    d->mdScroll = std::max(0.f, std::min(d->mdScroll, std::max(0.f, s_total - RH(area))));
    g_rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    float ox = area.left + pad, oy = area.top - d->mdScroll;
    for (auto& it : s_items) {
        float y = oy + it.y;
        if (y > area.bottom) break;
        if (y + it.h < area.top) continue;
        switch (it.kind) {
        case np::MdKind::Heading:
            g_rt->DrawTextLayout({ ox, y }, it.L, Brush(g_th.text));
            if (it.level <= 2) Line(ox, y + it.h + 6 * z, area.right - pad, y + it.h + 6 * z, g_th.border);
            break;
        case np::MdKind::Para: g_rt->DrawTextLayout({ ox, y }, it.L, Brush(g_th.text), (D2D1_DRAW_TEXT_OPTIONS)4); break;
        case np::MdKind::Quote:
            for (int q = 0; q < it.level; ++q) FillRR(RF(ox + q * 16 * z, y - 2, 3 * z, it.h + 4), 1.5f, Alpha(g_th.accent, 0.7f));
            g_rt->DrawTextLayout({ ox + it.indent, y }, it.L, Brush(g_th.dim));
            break;
        case np::MdKind::ListItem: {
            float bx = ox + it.indent - 18 * z;
            if (it.task >= 0) {
                RectF cb = RF(bx - 2 * z, y + 3 * z, 14 * z, 14 * z);
                if (it.task == 1) { FillRR(cb, 3 * z, g_th.accent); DrawIcon(I_CHECK, (cb.left + cb.right) / 2, (cb.top + cb.bottom) / 2, 11 * z, g_th.accentText, 1.8f); }
                else StrokeRR(cb, 3 * z, g_th.dim, 1.2f);
            } else Text(it.bullet, RF(bx - 10 * z, y, 26 * z, 20 * z), UiFmt(15 * z), g_th.dim, 2, false);
            g_rt->DrawTextLayout({ ox + it.indent, y }, it.L, Brush(g_th.text), (D2D1_DRAW_TEXT_OPTIONS)4);
            break; }
        case np::MdKind::Code: {
            if (it.chart) { DrawChart(it.spec, RF(ox, y, std::min(RW(area) - 2 * pad, 640 * z), it.h), z); break; }
            RectF box = RF(ox, y, RW(area) - 2 * pad, it.h);
            FillRR(box, 6 * z, g_th.codeBg);
            g_rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
            g_rt->DrawTextLayout({ ox + it.indent, y + 10 * z }, it.L, Brush(g_th.text));
            g_rt->PopAxisAlignedClip();
            break; }
        case np::MdKind::Rule: Line(ox, y + it.h / 2, area.right - pad, y + it.h / 2, g_th.border, 2); break;
        case np::MdKind::Table: {
            float ry = y;
            float tw = 0; for (float w : it.colW) tw += w;
            for (size_t r = 0; r < it.cells.size(); ++r) {
                if (r == 0) FillR(RF(ox, ry, tw, it.rowH[r]), g_th.codeBg);
                else if (r % 2 == 0) FillR(RF(ox, ry, tw, it.rowH[r]), Alpha(g_th.codeBg, 0.5f));
                float cx = ox;
                for (size_t c = 0; c < it.cells[r].size(); ++c) {
                    g_rt->DrawTextLayout({ cx + 10 * z, ry + 6 * z }, it.cells[r][c].L, Brush(g_th.text));
                    cx += it.colW[c];
                }
                ry += it.rowH[r];
                Line(ox, ry, ox + tw, ry, g_th.border);
            }
            StrokeRR(RF(ox, y, tw, it.h), 0, g_th.border);
            float cx = ox; for (size_t c = 0; c + 1 < it.colW.size(); ++c) { cx += it.colW[c]; Line(cx, y, cx, y + it.h, g_th.border); }
            break; }
        }
    }
    g_rt->PopAxisAlignedClip();
    if (s_total > RH(area)) {
        float th = std::max(24.f, RH(area) * RH(area) / s_total);
        float ty = area.top + (RH(area) - th) * d->mdScroll / (s_total - RH(area));
        FillRR(RF(area.right - 6, ty, 3, th), 1.5f, g_th.scrollThumb);
    }
}

void MdWheel(Doc* d, int delta) { d->mdScroll -= delta * 0.6f * Z(); Invalidate(); }

bool MdClick(Doc* d, float x, float y) {
    (void)d;
    if (!In(s_area, x, y)) return false;
    float z = Z(), pad = 24 * z, ox = s_area.left + pad, oy = s_area.top - d->mdScroll;
    auto test = [&](IDWriteTextLayout* L, const std::vector<LinkRange>& links, float lx, float ly) -> bool {
        if (!L || links.empty()) return false;
        BOOL tr, inside; DWRITE_HIT_TEST_METRICS m;
        L->HitTestPoint(x - lx, y - ly, &tr, &inside, &m);
        if (!inside) return false;
        for (auto& l : links) if (m.textPosition >= l.start && m.textPosition < l.start + l.len) {
            if (!l.url.empty()) ShellExecuteW(g_hwnd, L"open", l.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return true;
        }
        return false;
    };
    for (auto& it : s_items) {
        float iy = oy + it.y;
        if (y < iy || y > iy + it.h) continue;
        if (it.kind == np::MdKind::Table) {
            float ry = iy;
            for (size_t r = 0; r < it.cells.size(); ++r) {
                float cx = ox;
                for (size_t c = 0; c < it.cells[r].size(); ++c) { if (test(it.cells[r][c].L, it.cells[r][c].links, cx + 10 * z, ry + 6 * z)) return true; cx += it.colW[c]; }
                ry += it.rowH[r];
            }
        } else return test(it.L, it.links, ox + it.indent, iy);
    }
    return true;
}

} // namespace app
