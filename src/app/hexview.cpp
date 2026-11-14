// hexview.cpp - hex/binary editor view: offset | 16 bytes | ASCII. Overwrite or insert mode,
// nibble editing, ASCII-column typing, selection, hex copy/paste, own undo stack.
#include "app.h"
#include <cmath>
#include <cwctype>

namespace app {

static float s_cw = 8, s_lh = 18;
static RectF s_area, s_vbar;
static int s_dragHex = 0;
static IDWriteTextFormat* HexFmt() {
    float px = EditorFontPx();
    IDWriteTextFormat* f = Fmt(g_monoFont.c_str(), px);
    static float lastPx = 0;
    if (lastPx != px) {
        lastPx = px;
        s_cw = TextW(L"0000000000", f) / 10.f;
        s_lh = std::ceil(px * 1.45f);
    }
    return f;
}
static float ColHex(int i) { return 11 * s_cw + i * 3 * s_cw + (i >= 8 ? s_cw : 0); }
static float ColAsc(int i) { return 11 * s_cw + 16 * 3 * s_cw + s_cw * 3 + i * s_cw; }
static int Rows(Doc* d) { return (int)(d->bytes.size() / 16) + 1; }
static int VisRows() { return std::max(1, (int)((RH(s_area) - 8) / s_lh) - 1); }

void HexEnter(Doc* d) {
    if (!d->binaryOnly) d->bytes = np::EncodeText(d->text.Text(), d->text.enc, d->text.eol);
    d->hexUndo.clear(); d->hexRedo.clear(); d->hexSaved = 0;
    d->hexCaret = d->hexAnchor = 0; d->hexLow = false; d->hexTop = 0;
    d->hex = true;
}
bool HexLeave(Doc* d) {
    d->hex = false;
    if (d->binaryOnly || d->hexUndo.empty()) return false;
    np::Decoded dec = np::DecodeBytes(d->bytes);
    std::wstring t = np::NormalizeNewlines(np::DecodeAs(d->bytes, d->text.enc));
    (void)dec;
    d->text.Replace({ 0, 0 }, d->text.End(), t, d->caret, d->anchor);
    d->caret = d->anchor = d->text.Clamp(d->caret);
    d->lex.Invalidate(0); d->rowsDirty = true;
    d->hexUndo.clear(); d->hexRedo.clear();
    EditorClearCache();
    return true;
}

static void Apply(Doc* d, size_t off, size_t removeLen, const std::vector<uint8_t>& ins) {
    HexEdit e; e.off = off; e.caretBefore = d->hexCaret;
    removeLen = std::min(removeLen, d->bytes.size() - std::min(off, d->bytes.size()));
    e.oldB.assign(d->bytes.begin() + off, d->bytes.begin() + off + removeLen);
    e.newB = ins;
    d->bytes.erase(d->bytes.begin() + off, d->bytes.begin() + off + removeLen);
    d->bytes.insert(d->bytes.begin() + off, ins.begin(), ins.end());
    if (d->hexUndo.size() < d->hexSaved) d->hexSaved = SIZE_MAX;
    d->hexUndo.push_back(std::move(e)); d->hexRedo.clear();
    UpdateTitle();
}
void HexUndo(Doc* d, bool redo) {
    auto& from = redo ? d->hexRedo : d->hexUndo;
    auto& to = redo ? d->hexUndo : d->hexRedo;
    if (from.empty()) return;
    HexEdit e = std::move(from.back()); from.pop_back();
    const auto& cur = redo ? e.oldB : e.newB;
    const auto& put = redo ? e.newB : e.oldB;
    d->bytes.erase(d->bytes.begin() + e.off, d->bytes.begin() + e.off + cur.size());
    d->bytes.insert(d->bytes.begin() + e.off, put.begin(), put.end());
    d->hexCaret = d->hexAnchor = redo ? e.off + put.size() : e.caretBefore;
    d->hexLow = false;
    to.push_back(std::move(e));
    UpdateTitle(); Invalidate();
}

static void Ensure(Doc* d) {
    int row = (int)(d->hexCaret / 16);
    if (row < d->hexTop) d->hexTop = row;
    if (row >= d->hexTop + VisRows()) d->hexTop = row - VisRows() + 1;
}

void HexPaint(Doc* d, const RectF& area) {
    s_area = area;
    IDWriteTextFormat* f = HexFmt();
    FillR(area, g_th.editor);
    s_vbar = { area.right - 12, area.top, area.right, area.bottom };
    g_rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    float x0 = area.left + 12, y = area.top + 6;
    // header
    Text(L"Offset", RF(x0, y, 10 * s_cw, s_lh), f, g_th.dim);
    for (int i = 0; i < 16; ++i) { wchar_t h[4]; swprintf(h, 4, L"%02X", i); Text(h, RF(x0 + ColHex(i), y, 2 * s_cw + 4, s_lh), f, g_th.dim); }
    Text(L"Decoded text", RF(x0 + ColAsc(0), y, 16 * s_cw, s_lh), f, g_th.dim);
    y += s_lh;
    Line(x0, y - 2, x0 + ColAsc(16), y - 2, g_th.border);
    size_t sa = std::min(d->hexCaret, d->hexAnchor), sb = std::max(d->hexCaret, d->hexAnchor);
    bool focus = GetFocus() == g_hwnd;
    for (int r = d->hexTop; r < Rows(d) && y < area.bottom; ++r) {
        size_t base = (size_t)r * 16;
        wchar_t off[16]; swprintf(off, 16, L"%08zX", base);
        Text(off, RF(x0, y, 10 * s_cw + 4, s_lh), f, g_th.gutterText);
        for (int i = 0; i < 16; ++i) {
            size_t k = base + i;
            bool exists = k < d->bytes.size();
            if (!exists && k != d->bytes.size()) break;
            bool sel = sa != sb && k >= sa && k < sb;
            if (sel) {
                FillR(RF(x0 + ColHex(i) - s_cw * 0.5f, y, s_cw * 3, s_lh), g_th.selection);
                FillR(RF(x0 + ColAsc(i), y, s_cw, s_lh), g_th.selection);
            }
            if (k == d->hexCaret && focus && g_caretOn) {
                float cx = d->hexAscii ? x0 + ColAsc(i) : x0 + ColHex(i) + (d->hexLow ? s_cw : 0);
                if (d->hexInsert) FillR(RF(cx, y + 1, 1.6f, s_lh - 2), g_th.text);
                else FillR(RF(cx, y + 1, s_cw, s_lh - 2), Alpha(g_th.accent, 0.45f));
                float ox = d->hexAscii ? x0 + ColHex(i) : x0 + ColAsc(i);   // mirror cursor in the other column
                StrokeRR(RF(ox - 1, y + 1, d->hexAscii ? 2 * s_cw + 2 : s_cw + 2, s_lh - 2), 2, Alpha(g_th.accent, 0.6f));
            }
            if (!exists) break;
            uint8_t b = d->bytes[k];
            wchar_t hx[4]; swprintf(hx, 4, L"%02X", b);
            Color c = b == 0 ? g_th.faint : (b < 0x20 || b >= 0x7F) ? g_th.tok[np::T_NUMBER] : g_th.text;
            Text(hx, RF(x0 + ColHex(i), y, 2 * s_cw + 4, s_lh), f, c);
            wchar_t a[2] = { (b >= 0x20 && b < 0x7F) ? (wchar_t)b : L'·', 0 };
            Text(a, RF(x0 + ColAsc(i), y, s_cw + 3, s_lh), f, (b >= 0x20 && b < 0x7F) ? g_th.text : g_th.faint);
        }
        y += s_lh;
    }
    g_rt->PopAxisAlignedClip();
    int total = Rows(d), vis = VisRows();
    if (total > vis) {
        float trackH = RH(s_vbar) - 4, th = std::max(24.f, trackH * vis / total);
        float ty = s_vbar.top + 2 + (trackH - th) * d->hexTop / std::max(1, total - vis);
        FillRR(RF(s_vbar.right - 6, ty, 4, th), 2, g_th.scrollThumb);
    }
}

static bool HitByte(Doc* d, float x, float y, size_t& k, bool& low, bool& ascii) {
    float x0 = s_area.left + 12, y0 = s_area.top + 6 + s_lh;
    int r = d->hexTop + (int)std::floor((y - y0) / s_lh);
    if (r < 0) r = 0;
    float rx = x - x0;
    int i = -1;
    if (rx >= ColAsc(0) - s_cw) { ascii = true; i = (int)std::floor((rx - ColAsc(0)) / s_cw); low = false; }
    else {
        ascii = false;
        for (int c = 0; c < 16; ++c) if (rx < ColHex(c) + 2.5f * s_cw) { i = c; low = rx >= ColHex(c) + s_cw; break; }
        if (i < 0) i = 15;
    }
    i = std::max(0, std::min(15, i));
    k = std::min((size_t)r * 16 + i, d->bytes.size());
    if (k == d->bytes.size()) low = false;
    return true;
}

bool HexMouse(Doc* d, UINT msg, float x, float y, WPARAM wp) {
    if (msg == WM_LBUTTONDOWN && In(s_area, x, y)) {
        if (In(s_vbar, x, y)) { s_dragHex = 2; SetCapture(g_hwnd); }
        else {
            size_t k; bool low, asc; HitByte(d, x, y, k, low, asc);
            d->hexCaret = k; d->hexLow = low; d->hexAscii = asc;
            if (!(wp & MK_SHIFT)) d->hexAnchor = k;
            s_dragHex = 1; SetCapture(g_hwnd);
        }
        g_caretOn = true; Invalidate(); return true;
    }
    if (msg == WM_MOUSEMOVE && (wp & MK_LBUTTON) && s_dragHex) {
        if (s_dragHex == 2) {
            int total = Rows(d), vis = VisRows();
            float t = (y - s_vbar.top) / std::max(1.f, RH(s_vbar));
            d->hexTop = std::max(0, std::min(std::max(0, total - vis), (int)(t * total) - vis / 2));
        } else {
            size_t k; bool low, asc; HitByte(d, x, y, k, low, asc);
            d->hexCaret = std::min(k + (k >= d->hexAnchor ? 1 : 0), d->bytes.size()); d->hexLow = false;
        }
        Invalidate(); return true;
    }
    if (msg == WM_LBUTTONUP && s_dragHex) { s_dragHex = 0; ReleaseCapture(); return true; }
    return false;
}
void HexWheel(Doc* d, int delta) {
    int total = Rows(d), vis = VisRows();
    d->hexTop = std::max(0, std::min(std::max(0, total - vis), d->hexTop - delta / 40));
    Invalidate();
}

static void Move(Doc* d, long long delta, bool shift) {
    long long k = (long long)d->hexCaret + delta;
    k = std::max(0LL, std::min((long long)d->bytes.size(), k));
    d->hexCaret = (size_t)k; d->hexLow = false;
    if (!shift) d->hexAnchor = d->hexCaret;
    Ensure(d); g_caretOn = true; Invalidate();
}
static bool DeleteSel(Doc* d) {
    size_t a = std::min(d->hexCaret, d->hexAnchor), b = std::max(d->hexCaret, d->hexAnchor);
    if (a == b) return false;
    Apply(d, a, b - a, {});
    d->hexCaret = d->hexAnchor = a; d->hexLow = false;
    return true;
}

bool HexKey(Doc* d, WPARAM vk) {
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (vk) {
    case VK_LEFT:
        if (!d->hexAscii && d->hexLow && !shift) { d->hexLow = false; Invalidate(); return true; }
        Move(d, -1, shift); return true;
    case VK_RIGHT:
        if (!d->hexAscii && !d->hexLow && !shift && d->hexCaret < d->bytes.size()) { d->hexLow = true; Invalidate(); return true; }
        Move(d, 1, shift); return true;
    case VK_UP: Move(d, -16, shift); return true;
    case VK_DOWN: Move(d, 16, shift); return true;
    case VK_PRIOR: Move(d, -16LL * VisRows(), shift); return true;
    case VK_NEXT: Move(d, 16LL * VisRows(), shift); return true;
    case VK_HOME: Move(d, ctrl ? -(long long)d->hexCaret : -(long long)(d->hexCaret % 16), shift); return true;
    case VK_END: Move(d, ctrl ? (long long)(d->bytes.size() - d->hexCaret) : (long long)(15 - d->hexCaret % 16), shift); return true;
    case VK_TAB: d->hexAscii = !d->hexAscii; d->hexLow = false; Invalidate(); return true;
    case VK_INSERT: d->hexInsert = !d->hexInsert; Invalidate(); return true;
    case VK_DELETE:
        if (!DeleteSel(d) && d->hexCaret < d->bytes.size()) Apply(d, d->hexCaret, 1, {});
        d->hexLow = false; Invalidate(); return true;
    case VK_BACK:
        if (DeleteSel(d)) { Invalidate(); return true; }
        if (d->hexCaret > 0) {
            if (d->hexInsert) { Apply(d, d->hexCaret - 1, 1, {}); --d->hexCaret; d->hexAnchor = d->hexCaret; }
            else Move(d, -1, false);
        }
        Invalidate(); return true;
    }
    return false;
}

void HexChar(Doc* d, wchar_t c) {
    if (c < 0x20) return;
    DeleteSel(d);
    if (d->hexAscii) {
        if (c > 0xFF) { MessageBeep(MB_OK); return; }
        if (d->hexInsert || d->hexCaret >= d->bytes.size()) Apply(d, d->hexCaret, 0, { (uint8_t)c });
        else Apply(d, d->hexCaret, 1, { (uint8_t)c });
        Move(d, 1, false); return;
    }
    if (!iswxdigit(c)) { MessageBeep(MB_OK); return; }
    int v = iswdigit(c) ? c - L'0' : (towupper(c) - L'A' + 10);
    if (!d->hexLow) {
        if (d->hexInsert || d->hexCaret >= d->bytes.size()) Apply(d, d->hexCaret, 0, { (uint8_t)(v << 4) });
        else Apply(d, d->hexCaret, 1, { (uint8_t)((v << 4) | (d->bytes[d->hexCaret] & 0x0F)) });
        d->hexLow = true; d->hexAnchor = d->hexCaret;
    } else {
        Apply(d, d->hexCaret, 1, { (uint8_t)((d->bytes[d->hexCaret] & 0xF0) | v) });
        d->hexLow = false; Move(d, 1, false); return;
    }
    Ensure(d); Invalidate();
}

void HexCopy(Doc* d) {
    size_t a = std::min(d->hexCaret, d->hexAnchor), b = std::max(d->hexCaret, d->hexAnchor);
    if (a == b) return;
    std::wstring s;
    for (size_t k = a; k < b; ++k) { wchar_t h[4]; swprintf(h, 4, k + 1 < b ? L"%02X " : L"%02X", d->bytes[k]); s += h; }
    ClipboardSet(s);
}
void HexPaste(Doc* d) {
    std::wstring s = ClipboardGet();
    std::vector<uint8_t> v; int hi = -1;
    for (wchar_t c : s) {
        if (!iswxdigit(c)) continue;
        int x = iswdigit(c) ? c - L'0' : towupper(c) - L'A' + 10;
        if (hi < 0) hi = x; else { v.push_back((uint8_t)(hi << 4 | x)); hi = -1; }
    }
    if (v.empty()) { MessageBeep(MB_OK); return; }
    DeleteSel(d);
    size_t rem = d->hexInsert ? 0 : std::min(v.size(), d->bytes.size() - d->hexCaret);
    Apply(d, d->hexCaret, rem, v);
    d->hexCaret = d->hexAnchor = d->hexCaret + v.size(); d->hexLow = false;
    Ensure(d); Invalidate();
}

} // namespace app
