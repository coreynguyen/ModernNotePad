// gfx.cpp - Direct2D helpers, theme palette, and the original vector icon set.
// Icons are authored as SVG-style path strings on a 16x16 grid and turned into
// cached ID2D1PathGeometry objects (device-independent), then stroked/filled.
#include "app.h"
#include <cmath>
#include <cwchar>
#include <cmath>
#include <cwctype>

namespace app {

std::wstring g_uiFont = L"Segoe UI", g_monoFont = L"Consolas";
static std::map<ID2D1RenderTarget*, std::unordered_map<uint32_t, Com<ID2D1SolidColorBrush>>> s_brushSets;
float g_textScale = 1.f;
bool g_mica = false;
static std::map<std::wstring, Com<IDWriteTextFormat>> s_formats;
static Com<ID2D1StrokeStyle> s_round;

static uint32_t Key(Color c) {
    auto b = [](float v) { return (uint32_t)(std::min(1.f, std::max(0.f, v)) * 255.f + 0.5f); };
    return b(c.r) << 24 | b(c.g) << 16 | b(c.b) << 8 | b(c.a);
}
ID2D1SolidColorBrush* Brush(Color c) {
    uint32_t k = Key(c);
    auto& s_brushes = s_brushSets[g_rt];
    auto it = s_brushes.find(k);
    if (it != s_brushes.end()) return it->second;
    Com<ID2D1SolidColorBrush> b;
    g_rt->CreateSolidColorBrush(c, &b);
    if (s_brushes.size() > 4000) s_brushes.clear();
    s_brushes[k] = b;
    return b;
}
void ResetDeviceResources() { s_brushSets.erase(g_rt); }
void ReleaseBrushesFor(ID2D1RenderTarget* rt) { s_brushSets.erase(rt); }

void FillR(const RectF& r, Color c) { g_rt->FillRectangle(r, Brush(c)); }
void FillRR(const RectF& r, float rad, Color c) { g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), Brush(c)); }
void StrokeRR(const RectF& r, float rad, Color c, float w) {
    RectF q{ r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2 };
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(q, rad, rad), Brush(c), w);
}
void Line(float x0, float y0, float x1, float y1, Color c, float w) { g_rt->DrawLine({ x0, y0 }, { x1, y1 }, Brush(c), w); }

IDWriteTextFormat* Fmt(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w, DWRITE_FONT_STYLE s) {
    wchar_t key[160];
    swprintf(key, 160, L"%ls|%.2f|%d|%d", family, size, (int)w, (int)s);
    auto it = s_formats.find(key);
    if (it != s_formats.end()) return it->second;
    Com<IDWriteTextFormat> f;
    g_dw->CreateTextFormat(family, nullptr, w, s, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    s_formats[key] = f;
    return f;
}
IDWriteTextFormat* UiFmt(float size, bool bold) {
    return Fmt(g_uiFont.c_str(), size * g_textScale, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
}
float TextW(const std::wstring& s, IDWriteTextFormat* f) {
    Com<IDWriteTextLayout> l;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000.f, 1000.f, &l))) return 0;
    DWRITE_TEXT_METRICS m; l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}
void Text(const std::wstring& s, const RectF& r, IDWriteTextFormat* f, Color c, int halign, bool vcenter) {
    if (!f || s.empty()) return;
    f->SetTextAlignment(halign == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : halign == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    f->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    Com<IDWriteInlineObject> ell; g_dw->CreateEllipsisTrimmingSign(f, &ell);
    f->SetTrimming(&trim, ell);
    g_rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    f->SetTrimming(&trim, nullptr);
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

// ------------------------------------------------------------------ icons
struct IconDef { const wchar_t* path; bool fill; };
static std::wstring GearPath() {
    // 8 teeth, outer r 6.6, inner r 5.0, hub hole drawn as separate circle stroke
    std::wstring p; wchar_t b[64];
    const float PI = 3.14159265f;
    for (int i = 0; i < 16; ++i) {
        float a0 = (i * 22.5f - 6.f) * PI / 180.f, a1 = (i * 22.5f + 6.f) * PI / 180.f;
        float r = (i & 1) ? 5.1f : 6.7f;
        swprintf(b, 64, L"%ls%.2f %.2f L%.2f %.2f ", i ? L"L" : L"M", 8 + r * cosf(a0), 8 + r * sinf(a0), 8 + r * cosf(a1), 8 + r * sinf(a1));
        p += b;
    }
    p += L"Z M10.3 8A2.3 2.3 0 1 1 5.7 8A2.3 2.3 0 1 1 10.3 8Z";
    return p;
}
static const IconDef* Defs() {
    static std::wstring gear = GearPath();
    static IconDef d[I_COUNT] = {
        { L"M8 3V13M3 8H13", false },                                                       // add
        { L"M4 4L12 12M12 4L4 12", false },                                                 // close
        { L"M4 6L8 10L12 6", false },                                                       // chev down
        { L"M6 4L10 8L6 12", false },                                                       // chev right
        { L"M4 10L8 6L12 10", false },                                                      // chev up
        { L"M11.2 7A4.2 4.2 0 1 1 2.8 7A4.2 4.2 0 1 1 11.2 7Z M10.1 10.1L14 14", false },   // search
        { nullptr, false },                                                                 // gear (built)
        { L"M3 8H13", false },                                                              // minimize
        { L"M3.5 3.5H12.5V12.5H3.5Z", false },                                              // maximize
        { L"M3.5 5.5H10.5V12.5H3.5Z M5.5 5.5V3.5H12.5V10.5H10.5", false },                  // restore
        { L"M7 1.5C7.6 5 9.6 7 13 7.6C9.6 8.2 7.6 10.2 7 13.7C6.4 10.2 4.4 8.2 1 7.6C4.4 7 6.4 5 7 1.5Z "
          L"M12.8 10.2C13 11.4 13.6 12 14.8 12.2C13.6 12.4 13 13 12.8 14.2C12.6 13 12 12.4 10.8 12.2C12 12 12.6 11.4 12.8 10.2Z", true }, // sparkle
        { L"M2.5 4H13.5A1 1 0 0 1 14.5 5V11A1 1 0 0 1 13.5 12H2.5A1 1 0 0 1 1.5 11V5A1 1 0 0 1 2.5 4Z "
          L"M3.8 10V6L5.6 8.2L7.4 6V10 M11 6V10 M9.6 8.6L11 10L12.4 8.6", false },          // markdown
        { L"M5 3L2 8L5 13 M11 3L14 8L11 13 M7 5.5L9 10.5", false },                         // hex
        { L"M8 13V3M4 7L8 3L12 7", false },                                                 // arrow up
        { L"M8 3V13M4 9L8 13L12 9", false },                                                // arrow down
        { L"M13 8H3M7 4L3 8L7 12", false },                                                 // back
        { L"M11 8A3 3 0 1 1 5 8A3 3 0 1 1 11 8Z", true },                                   // dot
        { L"M2 4.5H10M7.5 2L10 4.5L7.5 7 M14 11.5H6M8.5 9L6 11.5L8.5 14", false },          // replace
        { L"M2 3.5H10M7.5 1.5L10 3.5L7.5 5.5 M2 7.5H10 M14 12.5H6M8.5 10.5L6 12.5L8.5 14.5", false }, // replace all
        { L"M5.5 5.5H13.5V14.5H5.5Z M3.5 11.5H2.5V1.5H10.5V3.5", false },                   // copy
        { L"M8 2V10M5 7L8 10L11 7M3 13.5H13", false },                                      // insert
        { L"M3 8.5L6.5 12L13 4.5", false },                                                 // check
        { L"M4 1.5H9.5L12.5 4.5V14.5H4Z M9.5 1.5V4.5H12.5", false },                        // file
        { L"M5.5 12.5A2 2 0 1 1 1.5 12.5A2 2 0 1 1 5.5 12.5Z M14.5 12.5A2 2 0 1 1 10.5 12.5A2 2 0 1 1 14.5 12.5Z M4.8 10.8L11 2 M11.2 10.8L5 2", false }, // cut
        { L"M4.5 3H3V14.5H13V3H11.5 M5.5 1.5H10.5V4.5H5.5Z", false },                       // paste
        { L"M2 2H4.5M6.8 2H9.2M11.5 2H14V4.5M14 6.8V9.2M14 11.5V14H11.5M9.2 14H6.8M4.5 14H2V11.5M2 9.2V6.8M2 4.5V2 M5.5 5.5H10.5V10.5H5.5Z", false }, // select all
        { L"M2.5 4H13.5 M6 4V2.5H10V4 M3.8 4L4.6 14.5H11.4L12.2 4 M6.5 7V12M9.5 7V12", false }, // delete
        { L"M5 2.5L2 5.5L5 8.5 M2 5.5H10A3.5 3.5 0 0 1 10 12.5H6", false },               // undo
        { L"M11 2.5L14 5.5L11 8.5 M14 5.5H6A3.5 3.5 0 0 0 6 12.5H10", false },            // redo
        { L"M2 2.5H14V13.5H2Z M2 6.2H14 M2 9.8H14 M6 2.5V13.5 M10 2.5V13.5", false },      // table
        { L"M6.8 9.2L9.2 6.8 M7.5 4.8L8.6 3.7A2.4 2.4 0 0 1 12.3 7.4L11.2 8.5 M8.5 11.2L7.4 12.3A2.4 2.4 0 0 1 3.7 8.6L4.8 7.5", false }, // link
        { L"M2.5 4H2.6M2.5 8H2.6M2.5 12H2.6M6 4H14M6 8H14M6 12H14", false },                 // bullet list
        { L"M2 2.8H3V6.5 M2 6.5H4 M2 9.5H4L2 13.2H4 M6.5 4.5H14M6.5 11.3H14", false },      // numbered list
        { L"M2 2.5H6V6.5H2Z M2.8 4.6L3.8 5.6L5.4 3.4 M2 9.5H6V13.5H2Z M8 4.5H14M8 11.5H14", false }, // task list
        { L"M2.5 4H6.5V8H4C4 10 5 11 6.5 11.5 M9.5 4H13.5V8H11C11 10 12 11 13.5 11.5", false }, // quote
        { L"M5.5 4L2 8L5.5 12 M10.5 4L14 8L10.5 12", false },                               // code
        { L"M2 2V14H14 M5 12V8.5 M8.2 12V5 M11.4 12V7", false },                            // chart
        { L"M2 3H14V13H2Z M2 11L6 7L9 10L11 8L14 11 M10.5 5.6H10.6", false },               // image
        { L"M2 8H14 M4 4.5H12 M4 11.5H12", false },                                         // hr
        { L"M2.5 13L6 3H7L10.5 13 M4 9H9 M10 9.5L14 13.5M14 9.5L10 13.5", false },          // clear formatting
        { L"M4 6V2H12V6 M2.5 6H13.5V11.5H2.5Z M4 9.5H12V14H4Z", false },                    // print
        { L"M2 2H11.5L14 4.5V14H2Z M5 2V5.5H10.5V2 M4.5 14V9.5H11.5V14", false },           // save
        { L"M1.5 13V3H6L7.5 4.5H12.5V7 M1.5 13L3.5 7H15L13 13Z", false },                    // open
        { L"M2 4.5H11.5V14H2Z M4.5 2H14V11.5 M6.8 7V11.5M4.5 9.2H9", false },                // new window
        { L"M14 8A6 6 0 1 1 2 8A6 6 0 1 1 14 8Z M8 4.5V8L10.5 9.5", false },                 // recent / clock
        { L"M9 2H3V14H9 M6.5 8H14.5 M12 5.5L14.5 8L12 10.5", false },                       // exit
        { L"M2 3.5H14 M2 8H11.5A2.5 2.5 0 0 1 11.5 13H8 M9.8 11.2L8 13L9.8 14.8 M2 12.5H5", false }, // wrap
        { L"M10.2 6.8A4.2 4.2 0 1 1 1.8 6.8A4.2 4.2 0 1 1 10.2 6.8Z M9.1 9.9L14 14.8 M4 6.8H8M6 4.8V8.8", false }, // zoom
        { L"M1.5 13L5 3L8.5 13 M2.8 9.5H7.2 M10.3 8.8C11 7.9 14 7.8 14 10V13 M14 10.8C12.4 10.6 10.4 10.9 10.5 12.2C10.7 13.4 13.4 13.3 14 11.8", false }, // font
        { L"M14 8A6 6 0 1 1 2 8A6 6 0 1 1 14 8Z M8 2V14 M8 5H12.5M8 8H14M8 11H12.5", false }, // theme
        { L"M2.5 3.2H3.5V7 M2.5 7H4.5 M2.5 9.8H4.5L2.5 13.5H4.5 M7 4.5H14M7 8.5H14M7 12.5H14", false }, // line numbers
        { L"M4.5 5.5L8 9L11.5 5.5 M2 2.5H14 M2 13.5H14", false },                           // fold
        { L"M1.5 8.5L3.5 2.5L5.5 8.5 M2.2 6.5H4.8 M7 2.5V8.5H8.8A1.5 1.5 0 0 0 8.8 5.5H7 M14.5 3.5A2.2 2.6 0 1 0 14.5 7.8 M6 12.5L8 14.5L13.5 10", false }, // spell
        { L"M1.5 8C3.5 4.2 12.5 4.2 14.5 8C12.5 11.8 3.5 11.8 1.5 8Z M10 8A2 2 0 1 1 6 8A2 2 0 1 1 10 8Z", false }, // eye / preview
        { L"M2.5 2.5H13.5V10.5H7.5L4.5 13.5V10.5H2.5Z", false },                            // comment
        { L"M4 2V14 M1.5 11.5L4 14L6.5 11.5 M8.5 4H14.5 M8.5 8H13 M8.5 12H11.5", false },  // sort
        { L"M13 3V8.5H3.5 M6 6L3.5 8.5L6 11", false },                                      // eol
        { L"M14 8A6 6 0 1 1 2 8A6 6 0 1 1 14 8Z M2 8H14 M8 2C5.6 5 5.6 11 8 14C10.4 11 10.4 5 8 2", false }, // globe
        { L"M2 2.5H14V13.5H2Z M4.5 5.5H11.5 M4.5 8H11.5 M4.5 10.5H9", false },              // menu style
        { L"M2 2H10V10H2Z M6 6H14V14H6Z", false },                                          // transparency
        { L"M2 3.5H14V8H2Z M4.5 5.75H4.6M7.5 5.75H7.6M10.5 5.75H10.6 M2 11H14 M2 13.5H10", false }, // toolbar
        { L"M2 5H14V13.5H2Z M2 5V2.5H7.5V5", false },                                       // tab
        { L"M10.5 6A3.5 3.5 0 1 1 3.5 6A3.5 3.5 0 1 1 10.5 6Z M9.3 8.8L14 13.5 M11.8 11.3L13.3 9.8", false }, // key
        { L"M2.5 2.5H13.5V7H2.5Z M2.5 9H13.5V13.5H2.5Z M4.8 4.75H4.9M4.8 11.25H4.9", false }, // server
        { L"M8 1.5L14 4.8V11.2L8 14.5L2 11.2V4.8Z M2 4.8L8 8L14 4.8 M8 8V14.5", false },    // model
        { L"M14 8A6 6 0 1 1 2 8A6 6 0 1 1 14 8Z M8 7.2V11.5 M8 4.8H8.1", false },            // info
        { L"M8 10V2 M5 5L8 2L11 5 M2.5 9V14H13.5V9", false },                               // export
        { L"M2 8H11 M7.5 4.5L11 8L7.5 11.5 M14 2.5V13.5", false },                          // goto
        { L"M3.5 13.5H6.5C4.5 12 3 10 3 7.5A5 5 0 0 1 13 7.5C13 10 11.5 12 9.5 13.5H12.5", false }, // unicode (omega)
        { L"M2.5 8A5.5 5.5 0 1 0 4.6 3.7 M2 1.8V4.8H5", false },                            // session / restore
        { L"M2.5 3V13M9 3V13M2.5 8H9 M11.5 9.3L13.5 8V13.5", false },                       // heading (H1)
        { L"M4 2.5H8.8A2.75 2.75 0 0 1 8.8 8H4Z M4 8H9.6A2.75 2.75 0 0 1 9.6 13.5H4Z", false }, // bold
        { L"M7 2.5H12 M4 13.5H9 M9.5 2.5L6.5 13.5", false },                                // italic
        { L"M11.2 4.5C10.8 3 9.6 2.5 8 2.5C5.6 2.5 4.6 3.8 4.6 5C4.6 6.2 5.6 6.8 7 7.2 M2 8.5H14 M5 11.4C5.4 12.9 6.7 13.5 8.2 13.5C10.4 13.5 11.5 12.5 11.5 11.2", false }, // strike
        { L"M4.5 2.5V8A3.5 3.5 0 0 0 11.5 8V2.5 M3.5 14H12.5", false },                     // underline
        { L"M2 2.5H14V13.5H2Z M2 10.5H14 M4 12H6", false },                                 // status bar
        { L"M2 3H14 M6 6.5H14 M6 10H14 M2 13.5H14 M2 6.2L4 8.2L2 10.2", false },            // indent
        { L"M5.5 4L2 8L5.5 12 M10.5 4L14 8L10.5 12 M9 3L7 13", false },                     // language
        { L"M2.5 13L5.5 3H6.5L9.5 13 M3.6 9.5H8.4 M10.5 5H14 M12.2 3V13 M10.5 13H14", false }, // encoding
        { L"M8 13.6C4.2 11 1.8 8.8 1.8 6A3.1 3.1 0 0 1 8 4.9A3.1 3.1 0 0 1 14.2 6C14.2 8.8 11.8 11 8 13.6Z", false }, // heart
        { L"M8 13.6C4.2 11 1.8 8.8 1.8 6A3.1 3.1 0 0 1 8 4.9A3.1 3.1 0 0 1 14.2 6C14.2 8.8 11.8 11 8 13.6Z", true }, // heart (filled)
        { L"M5.60 4.40A2.4 2.4 0 1 0 10.40 4.40A2.4 2.4 0 1 0 5.60 4.40Z M9.02 6.89A2.4 2.4 0 1 0 13.82 6.89A2.4 2.4 0 1 0 9.02 6.89Z M7.72 10.91A2.4 2.4 0 1 0 12.52 10.91A2.4 2.4 0 1 0 7.72 10.91Z M3.48 10.91A2.4 2.4 0 1 0 8.28 10.91A2.4 2.4 0 1 0 3.48 10.91Z M2.18 6.89A2.4 2.4 0 1 0 6.98 6.89A2.4 2.4 0 1 0 2.18 6.89Z M6.70 8.00A1.3 1.3 0 1 0 9.30 8.00A1.3 1.3 0 1 0 6.70 8.00Z", false }, // flower (pink settings)
        { L"M8 8L3.2 4.6C2.3 4 1.6 4.5 1.6 5.4V10.6C1.6 11.5 2.3 12 3.2 11.4Z M8 8L12.8 4.6C13.7 4 14.4 4.5 14.4 5.4V10.6C14.4 11.5 13.7 12 12.8 11.4Z M6.70 8.00A1.3 1.3 0 1 0 9.30 8.00A1.3 1.3 0 1 0 6.70 8.00Z M7.2 9.2L5.6 14 M8.8 9.2L10.4 14", false }, // bow
        { L"M3 1.5H13V14.5H3Z M5 3.5H11V6.3H5Z M5.6 8.6H5.7 M8 8.6H8.1 M10.4 8.6H10.5 M5.6 11.2H5.7 M8 11.2H8.1 M10.4 10.6V12.6", false }, // calculator / evaluate
    };
    d[I_GEAR].path = gear.c_str();
    return d;
}

static Com<ID2D1PathGeometry> BuildPath(const wchar_t* s) {
    Com<ID2D1PathGeometry> g;
    g_d2d->CreatePathGeometry(&g);
    Com<ID2D1GeometrySink> k;
    g->Open(&k);
    k->SetFillMode(D2D1_FILL_MODE_WINDING);
    bool open = false;
    D2D1_POINT_2F cur{ 0, 0 }, start{ 0, 0 };
    wchar_t cmd = 0;
    const wchar_t* p = s;
    auto num = [&]() -> float {
        while (*p == L' ' || *p == L',') ++p;
        wchar_t* e; float v = wcstof(p, &e); p = e; return v;
    };
    auto hasNum = [&]() { while (*p == L' ' || *p == L',') ++p; return (*p >= L'0' && *p <= L'9') || *p == L'-' || *p == L'.'; };
    auto begin = [&](D2D1_POINT_2F pt) { if (open) k->EndFigure(D2D1_FIGURE_END_OPEN); k->BeginFigure(pt, D2D1_FIGURE_BEGIN_FILLED); open = true; start = cur = pt; };
    while (*p) {
        while (*p == L' ') ++p;
        if (!*p) break;
        if (iswalpha(*p)) cmd = *p++;
        switch (cmd) {
        case L'M': { float x = num(), y = num(); begin({ x, y }); cmd = L'L'; break; }
        case L'L': { float x = num(), y = num(); cur = { x, y }; k->AddLine(cur); break; }
        case L'H': { cur.x = num(); k->AddLine(cur); break; }
        case L'V': { cur.y = num(); k->AddLine(cur); break; }
        case L'C': { float a = num(), b = num(), c = num(), d = num(), x = num(), y = num();
                     k->AddBezier(D2D1::BezierSegment({ a, b }, { c, d }, { x, y })); cur = { x, y }; break; }
        case L'Q': { float a = num(), b = num(), x = num(), y = num();
                     k->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ a, b }, { x, y })); cur = { x, y }; break; }
        case L'A': {   // SVG elliptical arc, flattened to segments (endpoint -> center parameterization, SVG 1.1 F.6.5)
            float rx = std::fabs(num()), ry = std::fabs(num()), rot = num(), large = num(), sweep = num(), x = num(), y = num();
            float phi = rot * 3.14159265f / 180.f, cp = cosf(phi), sp = sinf(phi);
            float dx = (cur.x - x) / 2, dy = (cur.y - y) / 2;
            float x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
            if (rx < 1e-4f || ry < 1e-4f) { k->AddLine({ x, y }); cur = { x, y }; break; }
            float lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
            if (lam > 1) { rx *= sqrtf(lam); ry *= sqrtf(lam); }
            float num2 = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
            float co = (den > 0 && num2 > 0) ? sqrtf(num2 / den) : 0.f;
            if ((large != 0) == (sweep != 0)) co = -co;
            float cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
            float cx = cp * cxp - sp * cyp + (cur.x + x) / 2, cy = sp * cxp + cp * cyp + (cur.y + y) / 2;
            auto ang = [](float ux, float uy, float vx, float vy) { return atan2f(ux * vy - uy * vx, ux * vx + uy * vy); };
            float t1 = ang(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
            float dt = ang((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
            if (!sweep && dt > 0) dt -= 6.2831853f; else if (sweep && dt < 0) dt += 6.2831853f;
            int segs = std::max(4, (int)(std::fabs(dt) / 0.12f));
            const wchar_t* q = p; while (*q == L' ') ++q;
            bool closesNext = (*q == L'Z' || *q == L'z') && std::fabs(x - start.x) < 1e-3f && std::fabs(y - start.y) < 1e-3f;
            for (int i = 1; i <= segs - (closesNext ? 1 : 0); ++i) {   // no duplicate end point before Z (avoids zero-length closing segment)
                float t = t1 + dt * i / segs, ex = rx * cosf(t), ey = ry * sinf(t);
                k->AddLine({ cp * ex - sp * ey + cx, sp * ex + cp * ey + cy });
            }
            cur = { x, y }; break; }
        case L'Z': case L'z': if (open) { k->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; cur = start; } break;
        default: ++p; break;
        }
        if (cmd == L'Z' || cmd == L'z') continue;
        if (!hasNum() && *p && !iswalpha(*p)) ++p;
    }
    if (open) k->EndFigure(D2D1_FIGURE_END_OPEN);
    k->Close();
    return g;
}

void DrawIcon(Icon i, float cx, float cy, float size, Color c, float stroke) {
    if (i < 0 || i >= I_COUNT) return;
    if (g_th.pink) {   // Pink theme swaps a few glyphs for cuter ones
        if (i == I_GEAR) i = I_FLOWER;
        else if (i == I_DOT) { i = I_HEARTF; size *= 1.15f; }
        else if (i == I_SPARKLE) i = I_HEART;
        else if (i == I_THEME) i = I_BOW;
    }
    static Com<ID2D1PathGeometry> cache[I_COUNT];
    if (!cache[i]) cache[i] = BuildPath(Defs()[i].path);
    if (!s_round) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        g_d2d->CreateStrokeStyle(sp, nullptr, 0, &s_round);
    }
    D2D1_MATRIX_3X2_F old; g_rt->GetTransform(&old);
    float k = size / 16.f;
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(cx - size / 2, cy - size / 2) * old);
    if (Defs()[i].fill) g_rt->FillGeometry(cache[i], Brush(c));
    else g_rt->DrawGeometry(cache[i], Brush(c), stroke / k, s_round);
    g_rt->SetTransform(old);
}

// ------------------------------------------------------------------ Pink theme graphics (vector, drawn live)
static void PinkAppIcon(float x, float y, float s) {
    Color rose = Hex(0xE0569B), deep = Hex(0xB0307A), paper = Hex(0xFFF7FB);
    RectF body = RF(x + s * 0.13f, y + s * 0.14f, s * 0.70f, s * 0.80f);
    FillRR(body, s * 0.10f, paper); StrokeRR(body, s * 0.10f, rose, std::max(1.f, s * 0.05f));
    FillRR(RF(body.left, body.top, RW(body), s * 0.20f), s * 0.08f, rose);
    for (int i = 0; i < 4; ++i) {   // spiral rings
        float cx = body.left + RW(body) * (0.2f + 0.2f * i);
        g_rt->FillEllipse(D2D1::Ellipse({ cx, body.top + s * 0.02f }, s * 0.045f, s * 0.065f), Brush(deep));
    }
    for (int i = 0; i < 3; ++i) Line(body.left + s * 0.10f, body.top + s * (0.36f + 0.13f * i), body.right - s * 0.10f, body.top + s * (0.36f + 0.13f * i), Hex(0xF2A7CB), std::max(0.8f, s * 0.035f));
    DrawIcon(I_HEARTF, x + s * 0.74f, y + s * 0.76f, s * 0.44f, Hex(0xFF3D8B));
    DrawIcon(I_HEART, x + s * 0.74f, y + s * 0.76f, s * 0.44f, Hex(0xFFFFFF), std::max(0.8f, s * 0.04f));
}
void PaintPinkEmpty(const RectF& area) {
    float cx = (area.left + area.right) / 2, cy = area.top + RH(area) * 0.42f;
    if (RH(area) < 220 || RW(area) < 260) return;
    DrawIcon(I_BOW, cx, cy - 18, 120, Alpha(g_th.accent, 0.16f), 1.6f);
    DrawIcon(I_HEARTF, cx - 86, cy + 38, 22, Alpha(g_th.accent, 0.14f));
    DrawIcon(I_HEARTF, cx + 92, cy - 62, 16, Alpha(g_th.accent, 0.12f));
    DrawIcon(I_HEARTF, cx + 70, cy + 48, 12, Alpha(g_th.accent, 0.12f));
    Text(L"Write something lovely \u2661", RF(area.left, cy + 52, RW(area), 28), UiFmt(15), Alpha(g_th.dim, 0.75f), 1);
}

// ------------------------------------------------------------------ app icon bitmap (from the exe's own icon resource)
void DrawAppIcon(float x, float y, float size) {
    if (g_th.pink) { PinkAppIcon(x, y, size); return; }
    static std::map<ID2D1RenderTarget*, Com<ID2D1Bitmap>> cache;
    Com<ID2D1Bitmap>& bmp = cache[g_rt];
    if (!bmp) {
        int px = (int)(size * 2);   // supersample for crispness at any DPI
        HICON ic = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(101), IMAGE_ICON, px, px, 0);
        if (!ic) return;
        ICONINFO ii{}; GetIconInfo(ic, &ii);
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB };
        std::vector<uint32_t> pix((size_t)px * px);
        HDC dc = GetDC(nullptr);
        GetDIBits(dc, ii.hbmColor, 0, px, pix.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        bool anyAlpha = false; for (auto v : pix) if (v >> 24) { anyAlpha = true; break; }
        for (auto& v : pix) {   // premultiply
            uint32_t a = anyAlpha ? v >> 24 : 255;
            uint32_t r = ((v >> 16) & 255) * a / 255, g = ((v >> 8) & 255) * a / 255, b = (v & 255) * a / 255;
            v = a << 24 | r << 16 | g << 8 | b;
        }
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        g_rt->CreateBitmap(D2D1::SizeU(px, px), pix.data(), px * 4, bp, &bmp);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        DestroyIcon(ic);
    }
    if (bmp) g_rt->DrawBitmap(bmp, RF(x, y, size, size), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void ReadTextScale() {
    DWORD v = 100, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    g_textScale = std::max(1.f, std::min(2.25f, v / 100.f));
}
bool TransparencySupported() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static DWORD build = 0;
    if (!build) {
        OSVERSIONINFOW vi{ sizeof(vi) };
        if (auto f = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&vi);
        build = vi.dwBuildNumber ? vi.dwBuildNumber : 1;
    }
    if (build < 22621) return false;   // DWMWA_SYSTEMBACKDROP_TYPE needs Windows 11 22H2
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

// ------------------------------------------------------------------ theme
static bool SystemDark() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}
static Color Accent() {
    DWORD c = 0, sz = sizeof(c);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &c, &sz) == ERROR_SUCCESS)
        return Hex(((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF));
    return Hex(0x005FB8);
}
// First family that exists in the system collection (user-chosen fonts can be uninstalled;
// also keeps text visible on systems without Segoe/Consolas).
std::wstring ResolveFamily(const std::wstring& want, std::initializer_list<const wchar_t*> fallbacks) {
    static Com<IDWriteFontCollection> fc;
    if (!fc) g_dw->GetSystemFontCollection(&fc);
    auto has = [&](const wchar_t* n) { UINT32 idx; BOOL ex = FALSE; return fc && n && *n && SUCCEEDED(fc->FindFamilyName(n, &idx, &ex)) && ex; };
    if (has(want.c_str())) return want;
    for (auto* f : fallbacks) if (has(f)) return f;
    return want;
}
std::wstring UiFont() {
    return ResolveFamily(L"Segoe UI Variable Text", { L"Segoe UI", L"Tahoma", L"Arial", L"Liberation Sans", L"DejaVu Sans" });
}
std::wstring MonoFont(const std::wstring& want) {
    return ResolveFamily(want, { L"Consolas", L"Cascadia Mono", L"Courier New", L"Liberation Mono", L"DejaVu Sans Mono" });
}

void BuildTheme() {
    bool pink = g_set.theme == 3;
    bool dark = !pink && (g_set.theme == 0 ? SystemDark() : g_set.theme == 2);
    Theme t; t.dark = dark; t.pink = pink;
    Color acc = Accent();
    using namespace np;
    if (dark) {
        t.chrome = Hex(0x202020); t.surface = Hex(0x2C2C2C); t.editor = Hex(0x272727);
        t.text = Hex(0xFFFFFF); t.dim = Hex(0xA8A8A8); t.faint = Hex(0x6E6E6E); t.border = Hex(0x3D3D3D);
        t.hover = Hex(0xFFFFFF, 0.06f); t.press = Hex(0xFFFFFF, 0.10f);
        t.accent = Mix(acc, Hex(0xFFFFFF), 0.35f); t.accentText = Hex(0x000000);
        t.selection = Alpha(Mix(acc, Hex(0xFFFFFF), 0.2f), 0.45f); t.selectionInactive = Hex(0xFFFFFF, 0.12f);
        t.lineHi = Hex(0xFFFFFF, 0.035f); t.gutterText = Hex(0x858585); t.gutterTextActive = Hex(0xE0E0E0);
        t.findHi = Hex(0xE2A33C, 0.35f); t.findCur = Hex(0xF5A623, 0.75f); t.danger = Hex(0xC42B1C);
        t.squiggle = Hex(0xFF6B6B); t.codeBg = Hex(0xFFFFFF, 0.06f); t.link = Hex(0x4FC1FF); t.scrollThumb = Hex(0xFFFFFF, 0.28f);
        Color* k = t.tok;
        k[T_TEXT] = t.text; k[T_KEYWORD] = Hex(0x569CD6); k[T_TYPE] = Hex(0x4EC9B0); k[T_STRING] = Hex(0xCE9178);
        k[T_NUMBER] = Hex(0xB5CEA8); k[T_COMMENT] = Hex(0x6A9955); k[T_PREPROC] = Hex(0xC586C0); k[T_OPERATOR] = Hex(0xD4D4D4);
        k[T_FUNC] = Hex(0xDCDCAA); k[T_TAG] = Hex(0x569CD6); k[T_ATTR] = Hex(0x9CDCFE); k[T_KEY] = Hex(0x9CDCFE);
        k[T_HEADING] = Hex(0x569CD6); k[T_EMPH] = Hex(0xD7BA7D); k[T_STRONG] = Hex(0xD7BA7D); k[T_CODE] = Hex(0xCE9178);
        k[T_LINK] = Hex(0x4FC1FF); k[T_QUOTE] = Hex(0x8A8A8A); k[T_LIST] = Hex(0x6796E6);
    } else {
        t.chrome = Hex(0xEEEEEE); t.surface = Hex(0xFAFAFA); t.editor = Hex(0xFFFFFF);
        t.text = Hex(0x1A1A1A); t.dim = Hex(0x5E5E5E); t.faint = Hex(0xA0A0A0); t.border = Hex(0xE3E3E3);
        t.hover = Hex(0x000000, 0.045f); t.press = Hex(0x000000, 0.08f);
        t.accent = acc; t.accentText = Hex(0xFFFFFF);
        t.selection = Alpha(acc, 0.28f); t.selectionInactive = Hex(0x000000, 0.08f);
        t.lineHi = Hex(0x000000, 0.03f); t.gutterText = Hex(0x9A9A9A); t.gutterTextActive = Hex(0x1A1A1A);
        t.findHi = Hex(0xF2C744, 0.40f); t.findCur = Hex(0xF59E0B, 0.70f); t.danger = Hex(0xC42B1C);
        t.squiggle = Hex(0xE81123); t.codeBg = Hex(0x000000, 0.045f); t.link = Hex(0x0064C8); t.scrollThumb = Hex(0x000000, 0.30f);
        Color* k = t.tok;
        k[T_TEXT] = t.text; k[T_KEYWORD] = Hex(0x0000FF); k[T_TYPE] = Hex(0x267F99); k[T_STRING] = Hex(0xA31515);
        k[T_NUMBER] = Hex(0x098658); k[T_COMMENT] = Hex(0x008000); k[T_PREPROC] = Hex(0xAF00DB); k[T_OPERATOR] = Hex(0x3B3B3B);
        k[T_FUNC] = Hex(0x795E26); k[T_TAG] = Hex(0x800000); k[T_ATTR] = Hex(0xE50000); k[T_KEY] = Hex(0x0451A5);
        k[T_HEADING] = Hex(0x0F4A85); k[T_EMPH] = Hex(0x6F42C1); k[T_STRONG] = Hex(0x6F42C1); k[T_CODE] = Hex(0xA31515);
        k[T_LINK] = Hex(0x0064C8); k[T_QUOTE] = Hex(0x6A737D); k[T_LIST] = Hex(0x0451A5);
    }
    if (pink) {   // soft blush chrome, rose accent, plum text
        Color rose = Hex(0xE0569B);
        t.chrome = Hex(0xFBE3EF); t.surface = Hex(0xFFF0F6); t.editor = Hex(0xFFF8FB);
        t.text = Hex(0x4A1C38); t.dim = Hex(0x8C5A77); t.faint = Hex(0xC39AB2); t.border = Hex(0xF2C4DA);
        t.hover = Hex(0xE0569B, 0.08f); t.press = Hex(0xE0569B, 0.15f);
        t.accent = rose; t.accentText = Hex(0xFFFFFF);
        t.selection = Hex(0xF29BC4, 0.45f); t.selectionInactive = Hex(0xE0569B, 0.12f);
        t.lineHi = Hex(0xE0569B, 0.05f); t.gutterText = Hex(0xD08AB0); t.gutterTextActive = Hex(0x7A2A5A);
        t.findHi = Hex(0xFFD36E, 0.55f); t.findCur = Hex(0xFF9F43, 0.75f);
        t.squiggle = Hex(0xD6336C); t.codeBg = Hex(0xE0569B, 0.08f); t.link = Hex(0xB0307A); t.scrollThumb = Hex(0xB0307A, 0.35f);
        Color* k = t.tok;
        k[T_TEXT] = t.text; k[T_KEYWORD] = Hex(0xC2185B); k[T_TYPE] = Hex(0x8E44AD); k[T_STRING] = Hex(0xD35400);
        k[T_NUMBER] = Hex(0x16A085); k[T_COMMENT] = Hex(0xB07A9C); k[T_PREPROC] = Hex(0x9B59B6); k[T_OPERATOR] = Hex(0x6D3A5A);
        k[T_FUNC] = Hex(0xAD1457); k[T_TAG] = Hex(0xC2185B); k[T_ATTR] = Hex(0x8E44AD); k[T_KEY] = Hex(0xAD1457);
        k[T_HEADING] = Hex(0xC2185B); k[T_EMPH] = Hex(0x8E44AD); k[T_STRONG] = Hex(0xAD1457); k[T_CODE] = Hex(0xD35400);
        k[T_LINK] = Hex(0xB0307A); k[T_QUOTE] = Hex(0xA07090); k[T_LIST] = Hex(0xC2185B);
    }
    // stronger hovers by default: the old 4-6% washes were invisible on 6-bit / low-contrast panels
    if (!dark) { t.hover = Alpha(t.pink ? t.accent : Hex(0x000000), t.pink ? 0.13f : 0.075f); t.press = Alpha(t.pink ? t.accent : Hex(0x000000), t.pink ? 0.22f : 0.13f); }
    else { t.hover = Hex(0xFFFFFF, 0.09f); t.press = Hex(0xFFFFFF, 0.14f); }
    t.menuHot = Alpha(t.accent, dark ? 0.26f : (t.pink ? 0.20f : 0.15f));
    HIGHCONTRASTW hcw{ sizeof(hcw) };
    bool sysHc = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hcw), &hcw, 0) && (hcw.dwFlags & HCF_HIGHCONTRASTON);
    if (g_set.highContrast || sysHc) {
        t.hc = true;
        t.hover = Alpha(t.accent, dark ? 0.30f : 0.22f); t.press = Alpha(t.accent, 0.36f);
        t.border = Mix(t.border, t.text, 0.45f);
        t.dim = Mix(t.dim, t.text, 0.45f); t.faint = Mix(t.faint, t.text, 0.3f);
        t.selection = Alpha(t.accent, 0.42f); t.selectionInactive = Alpha(t.accent, 0.22f);
        t.lineHi = Alpha(t.accent, dark ? 0.12f : 0.09f);
        t.scrollThumb = Alpha(t.text, 0.55f);
    }
    g_th = t;
    EditorClearCache();
    MdClearCache();
    ApplyDarkMenus();
}

// ------------------------------------------------------------------ background patterns
// One tile is rendered into a compatible target and tiled with a wrapping bitmap brush, so a full-window
// pattern costs one fill. The pattern scrolls with the text (scrollY) like printed paper.
static void HeartPath(ID2D1GeometrySink* k, float cx, float cy, float s) {
    k->BeginFigure({ cx, cy + s * 0.9f }, D2D1_FIGURE_BEGIN_FILLED);
    k->AddBezier(D2D1::BezierSegment({ cx - s * 1.3f, cy + s * 0.05f }, { cx - s * 0.75f, cy - s * 1.0f }, { cx, cy - s * 0.35f }));
    k->AddBezier(D2D1::BezierSegment({ cx + s * 0.75f, cy - s * 1.0f }, { cx + s * 1.3f, cy + s * 0.05f }, { cx, cy + s * 0.9f }));
    k->EndFigure(D2D1_FIGURE_END_CLOSED);
}
static void StarPath(ID2D1GeometrySink* k, float cx, float cy, float r) {
    for (int i = 0; i < 10; ++i) {
        float a = -3.14159265f / 2 + i * 3.14159265f / 5, rr = i % 2 ? r * 0.45f : r;
        D2D1_POINT_2F p{ cx + std::cos(a) * rr, cy + std::sin(a) * rr };
        if (i == 0) k->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED); else k->AddLine(p);
    }
    k->EndFigure(D2D1_FIGURE_END_CLOSED);
}
void PaintPattern(const RectF& area, float scrollY) {
    int pat = g_set.bgPattern == 0 ? (g_th.pink ? 3 : 1) : g_set.bgPattern;
    if (pat <= 1 || !g_rt) return;
    static ID2D1RenderTarget* rtFor = nullptr; static int patFor = -1; static uint32_t colFor = 0;
    static Com<ID2D1BitmapBrush> brush;
    Color base = g_th.pink ? Hex(0xE0569B) : (g_th.dark ? Hex(0xFFFFFF) : Hex(0x000000));
    float a = g_th.pink ? 0.09f : (g_th.dark ? 0.045f : 0.05f);
    uint32_t key = (uint32_t)(base.r * 255) << 16 | (uint32_t)(base.g * 255) << 8 | (uint32_t)(base.b * 255);
    if (!brush || rtFor != g_rt || patFor != pat || colFor != key) {
        brush.Reset();
        const float T = 56;
        Com<ID2D1BitmapRenderTarget> bt;
        if (FAILED(g_rt->CreateCompatibleRenderTarget(D2D1::SizeF(T, T), &bt.p)) || !bt) return;
        Com<ID2D1SolidColorBrush> b; bt->CreateSolidColorBrush(Alpha(base, a), &b.p);
        bt->BeginDraw(); bt->Clear(D2D1::ColorF(0, 0, 0, 0));
        if (pat == 2) { bt->FillEllipse(D2D1::Ellipse({ 14, 14 }, 1.6f, 1.6f), b); bt->FillEllipse(D2D1::Ellipse({ 42, 42 }, 1.6f, 1.6f), b); }
        else if (pat == 5) { bt->DrawLine({ 0, T - 0.5f }, { T, T - 0.5f }, b, 1); bt->DrawLine({ T - 0.5f, 0 }, { T - 0.5f, T }, b, 1);
                             bt->DrawLine({ 0, T / 2 - 0.5f }, { T, T / 2 - 0.5f }, b, 0.6f); bt->DrawLine({ T / 2 - 0.5f, 0 }, { T / 2 - 0.5f, T }, b, 0.6f); }
        else {
            ID2D1PathGeometry* g = nullptr; g_d2d->CreatePathGeometry(&g);
            if (g) {
                ID2D1GeometrySink* k = nullptr; g->Open(&k);
                if (pat == 3) { HeartPath(k, 14, 15, 6.5f); HeartPath(k, 42, 43, 4.5f); }
                else { StarPath(k, 14, 15, 6.5f); StarPath(k, 42, 42, 3.5f); }
                k->Close(); k->Release();
                bt->FillGeometry(g, b); g->Release();
            }
            if (pat == 4) bt->FillEllipse(D2D1::Ellipse({ 44, 13 }, 1.2f, 1.2f), b);
            else { bt->FillEllipse(D2D1::Ellipse({ 44, 14 }, 1.4f, 1.4f), b); bt->FillEllipse(D2D1::Ellipse({ 13, 44 }, 1.0f, 1.0f), b); }
        }
        bt->EndDraw();
        Com<ID2D1Bitmap> bmp; bt->GetBitmap(&bmp.p);
        if (!bmp) return;
        g_rt->CreateBitmapBrush(bmp, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &brush.p);
        rtFor = g_rt; patFor = pat; colFor = key;
    }
    if (!brush) return;
    brush->SetTransform(D2D1::Matrix3x2F::Translation(area.left, area.top - std::fmod(scrollY, 56.f)));
    g_rt->FillRectangle(area, brush);
}

} // namespace app
