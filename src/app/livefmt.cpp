// livefmt.cpp - Formatted view: Markdown rendered in place while staying editable (Obsidian "live preview" /
// Typora style, which is also what Win11 Notepad's formatting does - its documents are Markdown).
//
// Every line keeps its own DirectWrite layout over the *source* text, so caret, selection, find and undo
// work unchanged. Formatting is applied as layout ranges:
//   * markers (**, #, [..](url), `) are shrunk to ~0 width and made transparent, except on the caret line
//   * bullets, task boxes, quote bars, rules, images and whole tables are IDWriteInlineObjects replacing
//     their source range
//   * table continuation lines are removed from the row map; the table is one grid drawn on its header line
//     and edited cell-by-cell (the Markdown is rewritten aligned on every commit, so it never gets messy).
#include "app.h"
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#include <shellapi.h>
#include <cmath>
#include <map>

namespace app {

using np::Pos;
using np::LiveKind;
using np::LiveSpanKind;

bool LiveOn(Doc* d) {
    return d && d->fmtView && !d->hex && !d->grid && !d->large && !d->loading && !d->binaryOnly &&
           (d->lang == np::L_MD || d->lang == np::L_TEXT) && d->text.LineCount() < 30000;
}
void LiveSync(Doc* d) {
    if (d->liveVer == d->text.Version() && (int)d->live.size() == d->text.LineCount()) return;
    np::ScanLive(d->text, d->live);
    d->liveTables = false;
    for (auto& l : d->live) if (l.kind == LiveKind::TableHidden) { d->liveTables = true; break; }
    d->liveVer = d->text.Version();
    d->rowsDirty = true;
}
static int TableHeadOf(Doc* d, int line) {
    while (line > 0 && d->live[line].kind == LiveKind::TableHidden) --line;
    return line;
}

// ------------------------------------------------------------------ inline objects
struct LiveObj : IDWriteInlineObject {
    LONG ref = 1; float w = 10, h = 10, base = 8;
    virtual ~LiveObj() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IDWriteInlineObject)) { *p = static_cast<IDWriteInlineObject*>(this); AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer*, FLOAT x, FLOAT y, BOOL, BOOL, IUnknown*) override { Paint(x, y); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* m) override { m->width = w; m->height = h; m->baseline = base; m->supportsSideways = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* o) override { *o = {}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* b, DWRITE_BREAK_CONDITION* a) override { *b = *a = DWRITE_BREAK_CONDITION_NEUTRAL; return S_OK; }
    virtual void Paint(float x, float y) = 0;
};

// objects drawn this frame, for hit testing (absolute DIPs)
struct LiveHit { RectF r; int kind; int line; int a; };   // kind 1 task, 2 image, 3 table
static std::vector<LiveHit> s_hits;

struct BulletObj : LiveObj {
    int depth = 0; float px = 14;
    void Paint(float x, float y) override {
        float cx = x + px * 0.55f, cy = y + base - px * 0.33f, r = px * 0.17f;
        if (g_th.pink) { DrawIcon(depth % 2 ? I_HEART : I_HEARTF, cx, cy, px * 0.62f, g_th.accent, 1.1f); return; }
        if (depth % 3 == 0) g_rt->FillEllipse(D2D1::Ellipse({ cx, cy }, r, r), Brush(g_th.text));
        else if (depth % 3 == 1) g_rt->DrawEllipse(D2D1::Ellipse({ cx, cy }, r, r), Brush(g_th.text), 1.1f);
        else FillR(RF(cx - r, cy - r, r * 2, r * 2), g_th.text);
    }
};
struct TaskObj : LiveObj {
    bool checked = false; float px = 14; int line = 0, at = 0;
    void Paint(float x, float y) override {
        float s = std::round(px * 0.95f), bx = x + px * 0.12f, by = y + base - s * 0.86f;
        RectF b = RF(bx, by, s, s);
        if (checked && g_th.pink) { DrawIcon(I_HEARTF, bx + s / 2, by + s / 2 + 0.5f, s * 1.1f, g_th.accent); s_hits.push_back({ b, 1, line, at }); return; }
        if (checked) { FillRR(b, 3, g_th.accent); DrawIcon(I_CHECK, bx + s / 2, by + s / 2, s * 0.75f, g_th.accentText, 1.6f); }
        else StrokeRR(RF(bx + 0.5f, by + 0.5f, s - 1, s - 1), 3, g_th.dim, 1.3f);
        s_hits.push_back({ b, 1, line, at });
    }
};
struct QuoteObj : LiveObj {
    void Paint(float x, float y) override { FillRR(RF(x + 2, y, 3.5f, h + 4), 1.5f, Alpha(g_th.accent, 0.7f)); }
};
struct RuleObj : LiveObj {
    void Paint(float x, float y) override { FillR(RF(x, y + h / 2 - 0.5f, w - 4, 1.2f), g_th.border); }
};

// ---- images (local files only - nothing is ever downloaded)
struct ImgEntry { Com<ID2D1Bitmap> bmp; float w = 0, h = 0; bool failed = false; void* rt = nullptr; };
static std::map<std::wstring, ImgEntry> s_imgs;
static std::wstring UrlToPath(const std::wstring& url, const std::wstring& docPath) {
    std::wstring u = url;
    bool fileUrl = u.rfind(L"file:", 0) == 0;
    if (fileUrl) { u = u.substr(5); while (!u.empty() && u[0] == L'/') u.erase(0, 1); }
    if (u.rfind(L"http:", 0) == 0 || u.rfind(L"https:", 0) == 0 || u.rfind(L"data:", 0) == 0) return L"";
    std::wstring o;   // percent-decode (UTF-8)
    std::string bytes;
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == L'%' && i + 2 < u.size() && iswxdigit(u[i + 1]) && iswxdigit(u[i + 2])) { bytes += (char)wcstol(u.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; continue; }
        if (!bytes.empty()) { int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0); std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &w[0], n); o += w; bytes.clear(); }
        o += u[i] == L'/' ? L'\\' : u[i];
    }
    if (!bytes.empty()) { int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0); std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &w[0], n); o += w; }
    bool absolute = o.size() > 2 && (o[1] == L':' || (o[0] == L'\\' && o[1] == L'\\'));
    if (!absolute && !fileUrl) {
        if (docPath.empty()) return L"";
        size_t s = docPath.find_last_of(L"\\/");
        o = docPath.substr(0, s + 1) + o;
    }
    return o;
}
static ImgEntry* LoadImg(const std::wstring& path) {
    if (path.empty()) return nullptr;
    auto it = s_imgs.find(path);
    if (it != s_imgs.end() && (it->second.failed || it->second.rt == g_rt)) return &it->second;
    if (s_imgs.size() > 64) s_imgs.clear();
    ImgEntry& e = s_imgs[path];
    e = ImgEntry{}; e.rt = g_rt; e.failed = true;
    static Com<IWICImagingFactory> wic;
    if (!wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic.p));
    if (!wic || !g_rt) return &e;
    Com<IWICBitmapDecoder> dec; Com<IWICBitmapFrameDecode> fr; Com<IWICFormatConverter> cv;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec.p))) return &e;
    if (FAILED(dec->GetFrame(0, &fr.p)) || FAILED(wic->CreateFormatConverter(&cv.p))) return &e;
    if (FAILED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return &e;
    if (FAILED(g_rt->CreateBitmapFromWicBitmap(cv, nullptr, &e.bmp.p))) return &e;
    D2D1_SIZE_U sz = e.bmp->GetPixelSize();
    e.w = sz.width / g_scale; e.h = sz.height / g_scale; e.failed = false;
    return &e;
}
struct ImageObj : LiveObj {
    std::wstring alt, url; ImgEntry* img = nullptr; float px = 14; int line = 0;
    void Paint(float x, float y) override {
        RectF r = RF(x + 1, y + 2, w - 2, h - 4);
        if (img && !img->failed && img->bmp) g_rt->DrawBitmap(img->bmp, r, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else {
            FillRR(r, 6, g_th.codeBg); StrokeRR(r, 6, g_th.border);
            DrawIcon(I_IMAGE, r.left + 18, (r.top + r.bottom) / 2, 16, g_th.dim);
            bool web = url.rfind(L"http", 0) == 0;
            std::wstring t = (alt.empty() ? std::wstring(L"Image") : alt) + (web ? L"  ·  web image (not downloaded)" : L"  ·  not found");
            Text(t, RF(r.left + 34, r.top, RW(r) - 40, RH(r)), UiFmt(12), g_th.dim);
        }
        s_hits.push_back({ r, 2, line, 0 });
    }
};

// ---- tables
struct TableGeom { int first = -1, last = -1; RectF r; std::vector<float> colX; std::vector<float> rowY; int cols = 0, rows = 0; };
static std::map<int, TableGeom> s_tables;   // by first line, rebuilt each frame
struct TableObj : LiveObj {
    np::LiveTable t; std::vector<float> colW, rowH; std::vector<std::vector<Com<IDWriteTextLayout>>> cells;
    void Paint(float x, float y) override {
        TableGeom g; g.first = t.first; g.last = t.last; g.cols = t.Cols(); g.rows = (int)t.rows.size();
        float tw = 0; for (float c : colW) tw += c;
        RectF box = RF(x, y + 4, tw, h - 8);
        g.r = box;
        FillRR(box, 6, g_th.editor);
        float yy = box.top;
        for (size_t r = 0; r < t.rows.size(); ++r) {
            g.rowY.push_back(yy);
            if (r == 0) {   // header band (rounded top)
                g_rt->PushAxisAlignedClip(RF(box.left, yy, tw, rowH[0]), D2D1_ANTIALIAS_MODE_ALIASED);
                FillRR(RF(box.left, yy, tw, rowH[0] + 8), 6, g_th.codeBg);
                g_rt->PopAxisAlignedClip();
            }
            float xx = box.left;
            for (int c = 0; c < t.Cols(); ++c) {
                if (r == 0) g.colX.push_back(xx);
                if (cells[r][c]) g_rt->DrawTextLayout({ xx + 9, yy + 6 }, cells[r][c], Brush(g_th.text));
                xx += colW[c];
            }
            yy += rowH[r];
            if (r + 1 < t.rows.size()) Line(box.left, yy - 0.5f, box.right, yy - 0.5f, g_th.border);
        }
        g.rowY.push_back(yy);
        float xx = box.left;
        for (int c = 0; c + 1 < t.Cols(); ++c) { xx += colW[c]; Line(xx - 0.5f, box.top, xx - 0.5f, box.bottom, g_th.border); }
        g.colX.push_back(box.right);
        StrokeRR(RF(box.left + 0.5f, box.top + 0.5f, tw - 1, RH(box) - 1), 6, g_th.border);
        s_tables[t.first] = g;
        s_hits.push_back({ box, 3, t.first, 0 });
    }
};

static Com<IDWriteTextFormat> s_cellFmt; static float s_cellPx = 0;
static TableObj* MakeTable(Doc* d, int first, int last, IDWriteTextFormat* base, float px, float avail) {
    auto* o = new TableObj;
    np::ParseLiveTable(d->text, first, last, o->t);
    int cols = std::max(1, o->t.Cols());
    if (!s_cellFmt || std::fabs(s_cellPx - px) > 0.01f) {
        s_cellFmt.Reset(); s_cellPx = px;
        WCHAR fam[128] = L"Segoe UI"; base->GetFontFamilyName(fam, 128);
        g_dw->CreateTextFormat(fam, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", &s_cellFmt.p);
        if (s_cellFmt) s_cellFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    // natural widths
    std::vector<float> nat(cols, 0);
    std::vector<std::vector<std::wstring>> txt(o->t.rows.size());
    for (size_t r = 0; r < o->t.rows.size(); ++r)
        for (int c = 0; c < cols; ++c) {
            std::wstring v = c < (int)o->t.rows[r].size() ? np::StripInline(o->t.rows[r][c]) : L"";
            txt[r].push_back(v);
            nat[c] = std::max(nat[c], std::min(TextW(v, s_cellFmt), 420.f) + (r == 0 ? px * 0.6f : 0));
        }
    float minW = px * 4.5f, total = 0;
    o->colW.resize(cols);
    for (int c = 0; c < cols; ++c) { o->colW[c] = std::max(minW, nat[c] + 22); total += o->colW[c]; }
    float lim = std::max(minW * cols, avail - 8);
    if (total > lim) {   // shrink the wide columns, text wraps inside cells
        float excess = total - lim, shrinkable = 0;
        for (float w : o->colW) shrinkable += std::max(0.f, w - minW);
        for (auto& w : o->colW) if (shrinkable > 0) w -= excess * std::max(0.f, w - minW) / shrinkable;
        total = lim;
    }
    o->cells.resize(o->t.rows.size());
    o->rowH.assign(o->t.rows.size(), 0);
    for (size_t r = 0; r < o->t.rows.size(); ++r) {
        o->cells[r].resize(cols);
        float rh = px * 1.35f + 12;
        for (int c = 0; c < cols; ++c) {
            Com<IDWriteTextLayout> L;
            g_dw->CreateTextLayout(txt[r][c].c_str(), (UINT32)txt[r][c].size(), s_cellFmt, std::max(10.f, o->colW[c] - 18), 1e5f, &L.p);
            if (!L) continue;
            DWRITE_TEXT_RANGE all{ 0, (UINT32)txt[r][c].size() };
            if (r == 0) L->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
            int al = c < (int)o->t.align.size() ? o->t.align[c] : 0;
            L->SetTextAlignment(al == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : al == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
            DWRITE_TEXT_METRICS m; L->GetMetrics(&m);
            rh = std::max(rh, m.height + 12);
            o->cells[r][c] = L;
        }
        o->rowH[r] = std::ceil(rh);
    }
    float th = 8; for (float h : o->rowH) th += h;
    o->w = total + 2; o->h = th; o->base = th - 6;
    return o;
}

// ------------------------------------------------------------------ layout styling
static const float kHead[7] = { 1.f, 1.85f, 1.5f, 1.25f, 1.1f, 1.0f, 0.95f };

uint64_t LiveKey(Doc* d, int line, bool reveal, float avail) {
    LiveSync(d);
    const np::LiveLine& L = d->live[line];
    uint64_t k = 0x9E3779B97F4A7C15ull ^ ((uint64_t)L.kind << 1) ^ ((uint64_t)L.level << 8) ^ ((uint64_t)L.checked << 12) ^ ((uint64_t)reveal << 13) ^ ((uint64_t)L.prefix << 16);
    if (L.kind == LiveKind::TableHead) {
        for (int i = line; i <= L.tableEnd && i < d->text.LineCount(); ++i) k = (k ^ d->text.Stamp(i)) * 0x100000001B3ull;
        k ^= (uint64_t)(avail * 4) << 40;
    }
    if (L.kind == LiveKind::Rule || L.kind == LiveKind::Para || L.kind == LiveKind::Bullet || L.kind == LiveKind::Task) k ^= (uint64_t)(avail) << 44;
    return k | 1;   // never 0 (0 = not live)
}

// Applies formatting to a freshly created layout of `line`. Returns true if the layout needs natural line
// spacing (bigger fonts or inline objects), so the caller switches it off the uniform editor spacing.
bool LiveStyle(Doc* d, int line, bool reveal, IDWriteTextLayout* L, IDWriteTextFormat* base, float px, float avail) {
    LiveSync(d);
    const np::LiveLine& info = d->live[line];
    const std::wstring& s = d->text.Line(line);
    UINT32 n = (UINT32)s.size();
    bool natural = false;
    auto all = [&](UINT32 a, UINT32 len) { return DWRITE_TEXT_RANGE{ a, std::min(len, n > a ? n - a : 0) }; };
    if (info.kind == LiveKind::Code || info.kind == LiveKind::Fence) {
        if (info.kind == LiveKind::Fence && n) L->SetDrawingEffect(Brush(g_th.dim), all(0, n));
        return false;
    }
    if (info.kind == LiveKind::TableHead && n) {
        TableObj* o = MakeTable(d, line, info.tableEnd, base, px, avail);
        L->SetInlineObject(o, all(0, n)); o->Release();
        return true;
    }
    std::vector<np::LiveSpan> sp;
    np::LiveSpans(s, info, sp);
    if (info.kind == LiveKind::Heading && n) {
        float f = kHead[std::min<int>(info.level, 6)];
        L->SetFontSize(px * f, all(0, n));
        L->SetFontWeight(info.level <= 2 ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, all(0, n));
        natural = f > 1.01f;
    }
    if (info.kind == LiveKind::Quote && n > (UINT32)info.prefix) {
        L->SetDrawingEffect(Brush(Mix(g_th.text, g_th.dim, 0.5f)), all(info.prefix, n - info.prefix));
        L->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, all(info.prefix, n - info.prefix));
    }
    if (info.kind == LiveKind::Task && info.checked && n > (UINT32)info.prefix) {
        L->SetDrawingEffect(Brush(g_th.dim), all(info.prefix, n - info.prefix));
        L->SetStrikethrough(TRUE, all(info.prefix, n - info.prefix));
    }
    Color hidden = D2D1::ColorF(0, 0, 0, 0);
    for (auto& x : sp) {
        DWRITE_TEXT_RANGE r = all((UINT32)x.a, (UINT32)x.len);
        if (!r.length) continue;
        switch (x.kind) {
        case LiveSpanKind::Hide: case LiveSpanKind::Escape:
            if (reveal) L->SetDrawingEffect(Brush(g_th.dim), r);
            else { L->SetFontSize(0.05f, r); L->SetDrawingEffect(Brush(hidden), r); }
            break;
        case LiveSpanKind::Strong: L->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r); break;
        case LiveSpanKind::Em: L->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r); break;
        case LiveSpanKind::Strike: L->SetStrikethrough(TRUE, r); break;
        case LiveSpanKind::Code: L->SetDrawingEffect(Brush(g_th.tok[np::T_STRING]), r); break;
        case LiveSpanKind::Link: L->SetDrawingEffect(Brush(g_th.link), r); L->SetUnderline(TRUE, r); break;
        case LiveSpanKind::Bullet: {
            auto* o = new BulletObj; o->depth = x.arg; o->px = px; o->w = px * 1.15f; o->h = px * 1.2f; o->base = px * 0.95f;
            L->SetInlineObject(o, r); o->Release(); break; }
        case LiveSpanKind::Task: {
            auto* o = new TaskObj; o->checked = x.arg != 0; o->px = px; o->line = line; o->at = x.a;
            o->w = px * 1.55f; o->h = px * 1.2f; o->base = px * 0.95f;
            L->SetInlineObject(o, r); o->Release(); break; }
        case LiveSpanKind::Quote: {
            auto* o = new QuoteObj; o->w = px * 0.9f; o->h = px * 1.2f; o->base = px * 0.95f;
            L->SetInlineObject(o, r); o->Release(); break; }
        case LiveSpanKind::Rule:
            if (reveal) L->SetDrawingEffect(Brush(g_th.dim), r);
            else { auto* o = new RuleObj; o->w = std::max(40.f, avail - 8); o->h = px * 1.2f; o->base = px * 0.95f; L->SetInlineObject(o, r); o->Release(); }
            break;
        case LiveSpanKind::Image:
            if (reveal) { L->SetDrawingEffect(Brush(g_th.link), r); break; }
            else {
                auto* o = new ImageObj; o->alt = x.alt; o->url = x.url; o->px = px; o->line = line;
                o->img = LoadImg(UrlToPath(x.url, d->path));
                float maxW = std::min(std::max(80.f, avail - 12), 720.f);
                if (o->img && !o->img->failed) {
                    float sc = std::min(1.f, std::min(maxW / o->img->w, 420.f / o->img->h));
                    o->w = std::max(8.f, o->img->w * sc) + 2; o->h = std::max(8.f, o->img->h * sc) + 4;
                } else { o->w = std::min(maxW, 360.f); o->h = px * 2.6f; }
                o->base = o->h - 2;
                L->SetInlineObject(o, r); o->Release(); natural = true;
            }
            break;
        }
    }
    return natural;
}

// code-span backgrounds, code-block band, heading rule. Called before the line's text is drawn.
void LiveDecor(Doc* d, int line, IDWriteTextLayout* L, float x0, float y, float h, const RectF& textRect) {
    const np::LiveLine& info = d->live[line];
    if (info.kind == LiveKind::Code || info.kind == LiveKind::Fence) {
        bool top = info.kind == LiveKind::Fence && (line + 1 < (int)d->live.size() && d->live[line + 1].kind != LiveKind::Fence) && (line == 0 || d->live[line - 1].kind != LiveKind::Code);
        bool bottom = info.kind == LiveKind::Fence && !top;
        RectF r = RF(textRect.left + 2, y - (top ? -2 : 0), RW(textRect) - 8, h + (top || bottom ? -2 : 0));
        FillR(r, g_th.codeBg);
        return;
    }
    if (info.kind == LiveKind::Heading && info.level <= 2) Line(x0, y + h - 2.5f, textRect.right - 8, y + h - 2.5f, Alpha(g_th.border, 0.8f));
    std::vector<np::LiveSpan> sp; np::LiveSpans(d->text.Line(line), info, sp);
    for (auto& x : sp) if (x.kind == LiveSpanKind::Code) {
        UINT32 cnt = 0; DWRITE_HIT_TEST_METRICS hm[8];
        if (SUCCEEDED(L->HitTestTextRange(x.a, x.len, 0, 0, hm, 8, &cnt)))
            for (UINT32 i = 0; i < std::min(cnt, 8u); ++i) FillRR(RF(x0 + hm[i].left - 3, y + hm[i].top + 1, hm[i].width + 6, hm[i].height - 2), 3, g_th.codeBg);
    }
}

void LiveBeginFrame() { s_hits.clear(); s_tables.clear(); }

// ------------------------------------------------------------------ caret rules
Pos LiveFixCaret(Doc* d, Pos p, Pos old) {
    if (!LiveOn(d)) return p;
    LiveSync(d);
    if (p.line < 0 || p.line >= (int)d->live.size() || d->live[p.line].kind != LiveKind::TableHidden) return p;
    int first = TableHeadOf(d, p.line), last = d->live[p.line].tableEnd;
    if (old.line <= first && p.line > first) {   // moving forward past the table
        if (last + 1 < d->text.LineCount()) return { last + 1, 0 };
        return { first, (int)d->text.Line(first).size() };
    }
    return { first, 0 };
}

// ------------------------------------------------------------------ table cell editor
static HWND s_edit = nullptr; static WNDPROC s_editOrig = nullptr;
static int s_tFirst = -1, s_tR = 0, s_tC = 0;
static Doc* s_tDoc = nullptr;
bool LiveEditing() { return s_edit != nullptr; }

static bool CurTable(Doc* d, int first, np::LiveTable& t) {
    LiveSync(d);
    if (first < 0 || first >= (int)d->live.size() || d->live[first].kind != LiveKind::TableHead) return false;
    return np::ParseLiveTable(d->text, first, d->live[first].tableEnd, t);
}
static void WriteTable(Doc* d, const np::LiveTable& t) {
    std::wstring s = np::SerializeLiveTable(t);
    Pos a{ t.first, 0 }, b{ t.last, (int)d->text.Line(t.last).size() };
    if (d->text.GetText(a, b) == s) return;
    d->text.BreakCoalesce();
    d->text.BeginGroup();
    d->text.Replace(a, b, s, d->caret, d->anchor);
    d->text.EndGroup();
    d->caret = d->anchor = { t.first, 0 };
    EditorAfterEdit(d);
    LiveSync(d);
}
static std::wstring CellToEdit(const std::wstring& v) {
    std::wstring o; for (size_t i = 0; i < v.size(); ++i) { if (v[i] == L'\\' && i + 1 < v.size() && v[i + 1] == L'|') { o += L'|'; ++i; } else o += v[i]; }
    size_t p; while ((p = o.find(L"<br>")) != std::wstring::npos) o.replace(p, 4, L" ");
    return o;
}
static std::wstring EditToCell(const std::wstring& v) {
    std::wstring o; bool code = false;
    for (wchar_t c : v) {
        if (c == L'`') code = !code;
        if (c == L'|' && !code) o += L"\\|";
        else if (c == L'\r' || c == L'\n') o += L' ';
        else o += c;
    }
    return o;
}
static LRESULT CALLBACK CellProc(HWND h, UINT m, WPARAM wp, LPARAM lp);
static void Begin(Doc* d, int first, int r, int c);
static bool Commit(bool keep) {
    if (!s_edit) return false;
    int n = GetWindowTextLengthW(s_edit);
    std::wstring v(n + 1, 0); GetWindowTextW(s_edit, &v[0], n + 1); v.resize(n);
    HWND e = s_edit; s_edit = nullptr;
    DestroyWindow(e);
    Doc* d = s_tDoc;
    bool ok = false;
    if (keep && d && LiveOn(d)) {
        np::LiveTable t;
        if (CurTable(d, s_tFirst, t) && s_tR < (int)t.rows.size() && s_tC < t.Cols()) {
            std::wstring cell = EditToCell(v);
            while (!cell.empty() && cell.back() == L' ') cell.pop_back();
            while (!cell.empty() && cell[0] == L' ') cell.erase(0, 1);
            if (t.rows[s_tR][s_tC] != cell) { t.rows[s_tR][s_tC] = cell; WriteTable(d, t); }
            ok = true;
        }
    }
    SetFocus(g_hwnd); Invalidate();
    return ok;
}
void LiveCommitEdit() { Commit(true); }
void LiveCancelEdit() { Commit(false); }

static void MoveCell(int dr, int dc, bool tabWrap) {
    Doc* d = s_tDoc; int first = s_tFirst, r = s_tR, c = s_tC;
    Commit(true);
    if (!d) return;
    np::LiveTable t; if (!CurTable(d, first, t)) return;
    int rows = (int)t.rows.size(), cols = t.Cols();
    if (tabWrap) {
        c += dc;
        if (c >= cols) { c = 0; ++r; }
        if (c < 0) { c = cols - 1; --r; }
        if (r < 0) { r = 0; c = 0; }
        if (r >= rows) {   // Tab in the last cell adds a row (Word / Win11 Notepad behaviour)
            t.rows.push_back(std::vector<std::wstring>(cols)); WriteTable(d, t); r = rows; c = 0;
        }
    } else {
        r += dr;
        if (r >= rows) {   // Enter / Down past the last row: leave the table
            int last = d->live[first].tableEnd;
            if (last + 1 >= d->text.LineCount()) { Pos e{ last, (int)d->text.Line(last).size() }; d->text.Replace(e, e, L"\n", d->caret, d->anchor); EditorAfterEdit(d); }
            EditorSetCaret(d, { last + 1, 0 }, false); EditorEnsureCaret(d); Invalidate(); return;
        }
        if (r < 0) { EditorSetCaret(d, first > 0 ? Pos{ first - 1, (int)d->text.Line(first - 1).size() } : Pos{ first, 0 }, false); Invalidate(); return; }
    }
    Begin(d, first, r, std::max(0, std::min(c, cols - 1)));
}
static LRESULT CALLBACK CellProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN) {
        bool shift = GetKeyState(VK_SHIFT) < 0;
        if (wp == VK_ESCAPE) { Commit(false); return 0; }
        if (wp == VK_TAB) { MoveCell(0, shift ? -1 : 1, true); return 0; }
        if (wp == VK_RETURN) { MoveCell(shift ? -1 : 1, 0, false); return 0; }
        if (wp == VK_UP || wp == VK_DOWN) { MoveCell(wp == VK_UP ? -1 : 1, 0, false); return 0; }
        DWORD a = 0, b = 0; SendMessageW(h, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        int len = GetWindowTextLengthW(h);
        if (wp == VK_LEFT && a == 0 && b == 0 && !shift) { MoveCell(0, -1, true); return 0; }
        if (wp == VK_RIGHT && (int)a == len && (int)b == len && !shift) { MoveCell(0, 1, true); return 0; }
        if ((wp == 'Z' || wp == 'A') && GetKeyState(VK_CONTROL) < 0) { if (wp == 'A') SendMessageW(h, EM_SETSEL, 0, -1); else SendMessageW(h, EM_UNDO, 0, 0); return 0; }
    }
    if (m == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB || wp == 1 || wp == 26)) return 0;
    if (m == WM_KILLFOCUS && s_edit == h) { LRESULT r = CallWindowProcW(s_editOrig, h, m, wp, lp); PostMessageW(g_hwnd, WM_APP_LIVECOMMIT, 0, 0); return r; }
    return CallWindowProcW(s_editOrig, h, m, wp, lp);
}
static void Begin(Doc* d, int first, int r, int c) {
    if (s_edit) Commit(true);
    np::LiveTable t; if (!CurTable(d, first, t) || r >= (int)t.rows.size() || c >= t.Cols()) return;
    s_tDoc = d; s_tFirst = first; s_tR = r; s_tC = c;
    s_edit = CreateWindowExW(0, L"EDIT", CellToEdit(t.rows[r][c]).c_str(), WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)9011, g_inst, nullptr);
    SendMessageW(s_edit, WM_SETFONT, (WPARAM)UiEditFont(), TRUE);
    s_editOrig = (WNDPROC)SetWindowLongPtrW(s_edit, GWLP_WNDPROC, (LONG_PTR)CellProc);
    SetFocus(s_edit);
    SendMessageW(s_edit, EM_SETSEL, 0, -1);
    d->caret = d->anchor = { first, 0 };
    Invalidate();
}
// positions the cell editor over its cell; called after the editor painted (tables registered)
void LiveAfterPaint(Doc* d) {
    if (!s_edit) return;
    auto it = s_tables.find(s_tFirst);
    if (s_tDoc != d || it == s_tables.end() || s_tR + 1 >= (int)it->second.rowY.size() || s_tC + 1 >= (int)it->second.colX.size()) { ShowWindow(s_edit, SW_HIDE); return; }
    const TableGeom& g = it->second;
    RectF c = RF(g.colX[s_tC], g.rowY[s_tR], g.colX[s_tC + 1] - g.colX[s_tC], g.rowY[s_tR + 1] - g.rowY[s_tR]);
    FillR(RF(c.left + 1, c.top + 1, RW(c) - 2, RH(c) - 2), g_th.editor);
    StrokeRR(RF(c.left + 1, c.top + 1, RW(c) - 2, RH(c) - 2), 3, g_th.accent, 2);
    float eh = 18;
    SetWindowPos(s_edit, HWND_TOP, (int)((c.left + 8) * g_scale), (int)((c.top + (RH(c) - eh) / 2) * g_scale),
                 (int)(std::max(20.f, RW(c) - 14) * g_scale), (int)(eh * g_scale), SWP_SHOWWINDOW | SWP_NOACTIVATE);
}
void LiveEditTableNear(Doc* d, int line) {
    if (!LiveOn(d)) return;
    LiveSync(d);
    for (int i = std::max(0, line - 1); i < (int)d->live.size() && i < line + 40; ++i)
        if (d->live[i].kind == LiveKind::TableHead) { EditorSetCaret(d, { i, 0 }, false); EditorEnsureCaret(d); Begin(d, i, 0, 0); return; }
}

static bool CellAt(float x, float y, int& first, int& r, int& c) {
    for (auto& kv : s_tables) {
        const TableGeom& g = kv.second;
        if (!In(g.r, x, y)) continue;
        first = g.first; r = 0; c = 0;
        for (int i = 0; i + 1 < (int)g.rowY.size(); ++i) if (y >= g.rowY[i]) r = i;
        for (int i = 0; i + 1 < (int)g.colX.size(); ++i) if (x >= g.colX[i]) c = i;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ mouse / keys
static std::wstring LinkAt(Doc* d, Pos p) {
    if (p.line >= (int)d->live.size()) return L"";
    std::vector<np::LiveSpan> sp; np::LiveSpans(d->text.Line(p.line), d->live[p.line], sp);
    for (auto& x : sp) if ((x.kind == LiveSpanKind::Link || x.kind == LiveSpanKind::Image) && p.col >= x.a && p.col <= x.a + x.len) return x.url;
    return L"";
}
bool LiveMouseDown(Doc* d, float x, float y, Pos p, WPARAM wp) {
    if (!LiveOn(d)) return false;
    LiveSync(d);
    for (auto& h : s_hits) {
        if (!In(h.r, x, y)) continue;
        if (h.kind == 1 && h.line < d->text.LineCount()) {   // toggle task
            const std::wstring& s = d->text.Line(h.line);
            int at = h.a + 3;
            if (at < (int)s.size()) {
                std::wstring nv = s[at] == L' ' ? L"x" : L" ";
                d->text.BreakCoalesce();
                d->text.Replace({ h.line, at }, { h.line, at + 1 }, nv, d->caret, d->anchor);
                EditorAfterEdit(d);
            }
            return true;
        }
        if (h.kind == 3) { int first, r, c; if (CellAt(x, y, first, r, c)) { Begin(d, first, r, c); return true; } }
    }
    if (wp & MK_CONTROL) {
        std::wstring url = LinkAt(d, p);
        if (!url.empty()) {
            std::wstring target = url;
            if (url.find(L"://") == std::wstring::npos && url.rfind(L"mailto:", 0) != 0) { std::wstring pth = UrlToPath(url, d->path); if (!pth.empty()) target = pth; }
            ShellExecuteW(g_hwnd, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return true;
        }
    }
    return false;
}
bool LiveHoverLink(Doc* d, Pos p) { return LiveOn(d) && !LinkAt(d, p).empty(); }
bool LiveOverTable(float x, float y) { int f, r, c; return CellAt(x, y, f, r, c); }

// Enter continues lists / quotes; Enter on an empty item ends the list. Typing on a table line
// edits the table instead of the hidden source.
bool LiveKey(Doc* d, WPARAM vk) {
    if (!LiveOn(d) || EditorHasSel(d)) return false;
    LiveSync(d);
    Pos p = d->caret;
    const np::LiveLine& L = d->live[p.line];
    if (vk == VK_RETURN && GetKeyState(VK_CONTROL) >= 0) {
        if (L.kind == LiveKind::TableHead) {
            if (p.col == 0) { d->text.BreakCoalesce(); d->text.Replace({ p.line, 0 }, { p.line, 0 }, L"\n", d->caret, d->anchor); d->caret = d->anchor = { p.line, 0 }; EditorAfterEdit(d); return true; }
            int last = L.tableEnd; Pos e{ last, (int)d->text.Line(last).size() };
            d->text.BreakCoalesce(); d->text.Replace(e, e, L"\n", d->caret, d->anchor); d->caret = d->anchor = { last + 1, 0 }; EditorAfterEdit(d); EditorEnsureCaret(d);
            return true;
        }
        if (L.kind == LiveKind::Bullet || L.kind == LiveKind::Task || L.kind == LiveKind::Ordered || L.kind == LiveKind::Quote) {
            const std::wstring& s = d->text.Line(p.line);
            if (p.col < L.prefix) return false;
            bool empty = s.find_first_not_of(L" \t", L.prefix) == std::wstring::npos;
            if (empty) {   // end the list: drop the marker
                d->text.BreakCoalesce(); d->text.Replace({ p.line, 0 }, { p.line, (int)s.size() }, L"", d->caret, d->anchor);
                d->caret = d->anchor = { p.line, 0 }; EditorAfterEdit(d); return true;
            }
            std::wstring pre = s.substr(0, L.markerAt);
            if (L.kind == LiveKind::Bullet) pre += s.substr(L.markerAt, 2);
            else if (L.kind == LiveKind::Task) pre += s.substr(L.markerAt, 2) + L"[ ] ";
            else if (L.kind == LiveKind::Quote) pre += L"> ";
            else { int num = _wtoi(s.c_str() + L.markerAt); size_t e = s.find_first_of(L".)", L.markerAt); pre += std::to_wstring(num + 1) + s[e] + L' '; }
            d->text.BreakCoalesce();
            EditorInsert(d, L"\n" + pre);
            return true;
        }
    }
    if (L.kind == LiveKind::TableHead && (vk == VK_BACK || vk == VK_DELETE) && p.col > 0 && p.col < (int)d->text.Line(p.line).size()) return true;
    return false;
}
bool LiveChar(Doc* d, wchar_t ch) {
    if (!LiveOn(d) || EditorHasSel(d) || ch < 0x20) return false;
    LiveSync(d);
    Pos p = d->caret;
    if (d->live[p.line].kind != LiveKind::TableHead) return false;
    if (p.col == 0) {   // typing before a table starts a new paragraph above it
        d->text.BreakCoalesce(); d->text.Replace({ p.line, 0 }, { p.line, 0 }, std::wstring(1, ch) + L"\n", d->caret, d->anchor);
        d->caret = d->anchor = { p.line, 1 }; EditorAfterEdit(d); return true;
    }
    int last = d->live[p.line].tableEnd; Pos e{ last, (int)d->text.Line(last).size() };
    d->text.BreakCoalesce(); d->text.Replace(e, e, L"\n" + std::wstring(1, ch), d->caret, d->anchor);
    d->caret = d->anchor = { last + 1, 1 }; EditorAfterEdit(d); EditorEnsureCaret(d);
    return true;
}

// ------------------------------------------------------------------ table context menu
static int s_ctxFirst = -1, s_ctxR = 0, s_ctxC = 0;
bool LiveContextMenu(Doc* d, float x, float y, int sx, int sy) {
    if (!LiveOn(d)) return false;
    int first, r, c;
    if (!CellAt(x, y, first, r, c)) return false;
    if (s_edit) Commit(true);
    s_ctxFirst = first; s_ctxR = r; s_ctxC = c;
    np::LiveTable t; if (!CurTable(d, first, t)) return false;
    int al = c < (int)t.align.size() ? t.align[c] : 0;
    MenuModel m;
    m.items = { MI(C_LT_EDIT, L"Edit cell", L"", I_FONT), MSep(),
                MI(C_LT_ROWABOVE, L"Insert row above", L"", I_ARROW_UP, false, r > 0), MI(C_LT_ROWBELOW, L"Insert row below", L"", I_ARROW_DOWN),
                MI(C_LT_DELROW, L"Delete row", L"", I_DELETE, false, r > 0 && t.rows.size() > 2), MSep(),
                MI(C_LT_COLLEFT, L"Insert column left", L"", I_BACK), MI(C_LT_COLRIGHT, L"Insert column right", L"", I_GOTO),
                MI(C_LT_DELCOL, L"Delete column", L"", I_DELETE, false, t.Cols() > 1), MSep(),
                MSub(L"Align column", I_INDENT, { MI(C_LT_ALIGNL, L"Left", L"", I_NONE, al == 0 || al == 3), MI(C_LT_ALIGNC, L"Center", L"", I_NONE, al == 1), MI(C_LT_ALIGNR, L"Right", L"", I_NONE, al == 2) }),
                MI(C_LT_SORT, L"Sort by this column", L"", I_SORT, false, t.rows.size() > 2), MSep(),
                MI(C_LT_DELTABLE, L"Delete table", L"", I_DELETE) };
    int cmd = RunMenu(m, sx, sy);
    if (cmd) LiveTableCommand(d, cmd);
    return true;
}
void LiveTableCommand(Doc* d, int cmd) {
    np::LiveTable t; if (!CurTable(d, s_ctxFirst, t)) return;
    int r = s_ctxR, c = s_ctxC, cols = t.Cols();
    switch (cmd) {
    case C_LT_EDIT: Begin(d, s_ctxFirst, r, c); return;
    case C_LT_ROWABOVE: t.rows.insert(t.rows.begin() + std::max(1, r), std::vector<std::wstring>(cols)); break;
    case C_LT_ROWBELOW: t.rows.insert(t.rows.begin() + std::max(1, r + 1), std::vector<std::wstring>(cols)); break;
    case C_LT_DELROW: if (r > 0 && t.rows.size() > 2) t.rows.erase(t.rows.begin() + r); break;
    case C_LT_COLLEFT: case C_LT_COLRIGHT: {
        int at = cmd == C_LT_COLLEFT ? c : c + 1;
        for (size_t i = 0; i < t.rows.size(); ++i) t.rows[i].insert(t.rows[i].begin() + at, i == 0 ? L"Column " + std::to_wstring(cols + 1) : L"");
        t.align.insert(t.align.begin() + at, 0); break; }
    case C_LT_DELCOL: if (cols > 1) { for (auto& row : t.rows) row.erase(row.begin() + c); t.align.erase(t.align.begin() + c); } break;
    case C_LT_ALIGNL: t.align[c] = 3; break;
    case C_LT_ALIGNC: t.align[c] = 1; break;
    case C_LT_ALIGNR: t.align[c] = 2; break;
    case C_LT_SORT: {
        std::stable_sort(t.rows.begin() + 1, t.rows.end(), [&](const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
            std::wstring x = np::StripInline(a[c]), y = np::StripInline(b[c]); double nx, ny;
            if (np::CsvIsNumber(x, &nx) && np::CsvIsNumber(y, &ny)) return nx < ny;
            return _wcsicmp(x.c_str(), y.c_str()) < 0; });
        break; }
    case C_LT_DELTABLE: {
        Pos a{ t.first, 0 }, b = t.last + 1 < d->text.LineCount() ? Pos{ t.last + 1, 0 } : Pos{ t.last, (int)d->text.Line(t.last).size() };
        d->text.BreakCoalesce(); d->text.Replace(a, b, L"", d->caret, d->anchor); d->caret = d->anchor = a; EditorAfterEdit(d); return; }
    }
    WriteTable(d, t);
}

} // namespace app
