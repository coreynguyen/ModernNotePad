// flyout.cpp - menus. One MenuModel, two presenters:
//  * modern: owner-drawn popup windows with the Windows 11 acrylic backdrop (DWMSBT_TRANSIENTWINDOW),
//    rounded corners, icon column, a command-bar row (Cut/Copy/Paste/...), submenus, full keyboard nav.
//  * classic: the model is converted to an HMENU and shown with TrackPopupMenu.
#include "app.h"
#include <dwmapi.h>
#include <cmath>
#include <cwctype>
#include <functional>

namespace app {

struct Fly {
    HWND h = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    const std::vector<MenuItem>* items = nullptr;
    const std::vector<MenuItem>* bar = nullptr;
    std::vector<RectF> rItems, rBar;
    float w = 0, hgt = 0;
    bool acrylic = false;
    std::vector<uint32_t> frost; int fw = 0, fh = 0;   // blurred snapshot of what is behind the flyout
    Com<ID2D1Bitmap> frostBmp;
    int hover = -1;          // >= 0 item, <= -2 bar index (-2 - i)
    int openSub = -1;        // item index whose submenu is open
    bool keyboard = false;
};
static std::vector<Fly*> s_flies;
static bool s_done = false, s_active = false;
static int s_result = 0;
static const float PAD = 5;

bool MenuActive() { return s_active; }

static float ItemH() { return std::round(30.f * g_textScale); }
static float SepH() { return 9.f; }
static float BarH() { return std::round(58.f * g_textScale); }

static bool IsFly(HWND h) { for (auto* f : s_flies) if (f->h == h) return true; return false; }
static Fly* FlyOf(HWND h) { for (auto* f : s_flies) if (f->h == h) return f; return nullptr; }

static void Measure(Fly* f) {
    IDWriteTextFormat* tf = UiFmt(13);
    float textW = 0, keysW = 0; bool anySub = false;
    for (auto& it : *f->items) {
        if (it.sep) continue;
        textW = std::max(textW, TextW(it.text, tf));
        if (!it.keys.empty()) keysW = std::max(keysW, TextW(it.keys, UiFmt(12)));
        if (!it.sub.empty()) anySub = true;
    }
    float w = 12 + 30 + textW + (keysW > 0 ? 28 + keysW : 0) + (anySub ? 26 : 0) + 18;
    float barW = f->bar && !f->bar->empty() ? f->bar->size() * 62.f * g_textScale + 2 * PAD : 0;
    f->w = std::max({ 200.f, w, barW });
    float y = PAD;
    f->rBar.clear(); f->rItems.clear();
    if (f->bar && !f->bar->empty()) {
        float bw = (f->w - 2 * PAD) / f->bar->size();
        for (size_t i = 0; i < f->bar->size(); ++i) f->rBar.push_back(RF(PAD + bw * i, y, bw, BarH()));
        y += BarH() + 1 + SepH() - 4;
    }
    for (auto& it : *f->items) {
        float h = it.sep ? SepH() : ItemH();
        f->rItems.push_back(RF(PAD, y, f->w - 2 * PAD, h));
        y += h;
    }
    f->hgt = y + PAD;
}

static Color MenuBg() { return g_th.dark ? Hex(0x2C2C2C) : Hex(0xF9F9F9); }

// Frosted glass. DWM backdrops (DWMSBT_TRANSIENTWINDOW) render their solid fallback for inactive windows and
// flyouts are never activated, and the undocumented accent-policy blur is unreliable across Windows builds - so
// both v1.1 and v1.2 menus came out opaque. Like MPP, blur it ourselves: snapshot the screen behind the flyout
// just before it is shown, downsample 4x, three box-blur passes (~gaussian), then the menu tint on top.
static bool FrostEnabled() {
    if (!g_set.transparency) return false;
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}
static void BoxBlur(std::vector<float>& c, int w, int h, int r) {
    std::vector<float> t(c.size());
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) for (int ch = 0; ch < 3; ++ch) {   // horizontal
            float acc = 0; int n = 0;
            for (int x = -r; x <= r; ++x) { int xx = std::max(0, std::min(w - 1, x)); acc += c[(y * w + xx) * 3 + ch]; ++n; }
            for (int x = 0; x < w; ++x) {
                t[(y * w + x) * 3 + ch] = acc / n;
                int xo = std::max(0, std::min(w - 1, x - r)), xi = std::max(0, std::min(w - 1, x + r + 1));
                acc += c[(y * w + xi) * 3 + ch] - c[(y * w + xo) * 3 + ch];
            }
        }
        for (int x = 0; x < w; ++x) for (int ch = 0; ch < 3; ++ch) {   // vertical
            float acc = 0; int n = 0;
            for (int y = -r; y <= r; ++y) { int yy = std::max(0, std::min(h - 1, y)); acc += t[(yy * w + x) * 3 + ch]; ++n; }
            for (int y = 0; y < h; ++y) {
                c[(y * w + x) * 3 + ch] = acc / n;
                int yo = std::max(0, std::min(h - 1, y - r)), yi = std::max(0, std::min(h - 1, y + r + 1));
                acc += t[(yi * w + x) * 3 + ch] - t[(yo * w + x) * 3 + ch];
            }
        }
    }
}
static void Snapshot(Fly* f, int x, int y, int w, int h) {
    const int k = 4;
    int sw = std::max(1, w / k), sh = std::max(1, h / k);
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        ok = BitBlt(mem, 0, 0, w, h, scr, x, y, SRCCOPY | CAPTUREBLT) != FALSE;
        SelectObject(mem, old);
    }
    if (ok) {
        const uint8_t* px = (const uint8_t*)bits;
        std::vector<float> c(sw * sh * 3, 0.f);
        for (int yy = 0; yy < sh; ++yy) for (int xx = 0; xx < sw; ++xx) {
            float a[3] = { 0, 0, 0 }; int n = 0;
            for (int dy = 0; dy < k; ++dy) for (int dx = 0; dx < k; ++dx) {
                int X = xx * k + dx, Y = yy * k + dy; if (X >= w || Y >= h) continue;
                const uint8_t* p = px + ((size_t)Y * w + X) * 4; a[0] += p[0]; a[1] += p[1]; a[2] += p[2]; ++n;
            }
            for (int ch = 0; ch < 3; ++ch) c[(yy * sw + xx) * 3 + ch] = n ? a[ch] / n : 0;
        }
        BoxBlur(c, sw, sh, std::max(2, (int)(5 * g_scale)));
        f->frost.resize(sw * sh); f->fw = sw; f->fh = sh;
        for (int i = 0; i < sw * sh; ++i) {
            // a little extra saturation keeps colours alive under the tint, like acrylic's luminosity layer
            float b = c[i * 3], g = c[i * 3 + 1], r = c[i * 3 + 2], l = 0.299f * r + 0.587f * g + 0.114f * b;
            auto sat = [&](float v) { return (uint32_t)std::max(0.f, std::min(255.f, l + (v - l) * 1.35f)); };
            f->frost[i] = 0xFF000000u | (sat(r) << 16) | (sat(g) << 8) | sat(b);
        }
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem); ReleaseDC(nullptr, scr);
}
static void PaintFrost(Fly* f) {
    if (!f->frostBmp && !f->frost.empty()) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        g_rt->CreateBitmap(D2D1::SizeU(f->fw, f->fh), f->frost.data(), f->fw * 4, bp, &f->frostBmp.p);
    }
    if (f->frostBmp) g_rt->DrawBitmap(f->frostBmp, RF(-2, -2, f->w + 4, f->hgt + 4), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    // subtle grain so it reads as frosted glass rather than a smear
    uint32_t seed = 0x9E3779B9u;
    for (float y = 0; y < f->hgt; y += 3) for (float x = ((int)y / 3 % 2) * 1.5f; x < f->w; x += 3) {
        seed = seed * 1664525u + 1013904223u;
        if ((seed >> 28) < 3) FillR(RF(x, y, 1, 1), g_th.dark ? Hex(0xFFFFFF, 0.035f) : Hex(0x000000, 0.03f));
    }
}

static void Paint(Fly* f) {
    if (!f->rt) {
        RECT rc; GetClientRect(f->h, &rc);
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * g_scale, 96.f * g_scale);
        g_d2d->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(f->h, D2D1::SizeU(rc.right, rc.bottom)), &f->rt);
        if (!f->rt) return;
        f->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    ID2D1HwndRenderTarget* saved = g_rt;
    g_rt = f->rt;
    g_rt->BeginDraw();
    if (f->acrylic) { g_rt->Clear(MenuBg()); PaintFrost(f); FillR(RF(0, 0, f->w, f->hgt), Alpha(MenuBg(), g_th.dark ? 0.66f : 0.62f));
                      StrokeRR(RF(0.5f, 0.5f, f->w - 1, f->hgt - 1), 7, Alpha(g_th.border, 0.9f)); }
    else { g_rt->Clear(MenuBg()); StrokeRR(RF(0, 0, f->w, f->hgt), 7, g_th.border); }
    Color txt = g_th.text, dim = g_th.dim, faint = Mix(g_th.dim, MenuBg(), 0.45f);
    // command bar
    if (f->bar) for (size_t i = 0; i < f->bar->size(); ++i) {
        const MenuItem& b = (*f->bar)[i];
        RectF r = f->rBar[i];
        bool bh = f->hover == -2 - (int)i && b.enabled;
        if (bh) FillRR(RF(r.left + 2, r.top + 2, RW(r) - 4, RH(r) - 4), 5, g_th.hc ? g_th.accent : g_th.menuHot);
        Color c = b.enabled ? (bh && g_th.hc ? g_th.accentText : txt) : faint;
        DrawIcon(b.icon, (r.left + r.right) / 2, r.top + RH(r) * 0.36f, 17 * g_textScale, c, 1.15f);
        Text(b.text, RF(r.left, r.top + RH(r) * 0.58f, RW(r), RH(r) * 0.36f), UiFmt(11.5f), c, 1);
    }
    if (f->bar && !f->bar->empty()) { float y = f->rBar[0].bottom + 4; Line(PAD + 6, y, f->w - PAD - 6, y, g_th.border); }
    for (size_t i = 0; i < f->items->size(); ++i) {
        const MenuItem& it = (*f->items)[i];
        RectF r = f->rItems[i];
        if (it.sep) { float y = (r.top + r.bottom) / 2; Line(r.left + 6, y, r.right - 6, y, g_th.border); continue; }
        bool hot = (f->hover == (int)i || f->openSub == (int)i) && it.enabled;
        // hover: accent-tinted fill + accent bar (a 4% grey wash vanished on 6-bit / low-gamut panels);
        // high contrast: solid accent with contrasting text
        bool hc = g_th.hc;
        if (hot) FillRR(RF(r.left, r.top + 1, RW(r), RH(r) - 2), 5, hc ? g_th.accent : g_th.menuHot);
        if (hot && !hc) FillRR(RF(r.left, r.top + RH(r) * 0.28f, 3, RH(r) * 0.44f), 1.5f, g_th.accent);
        Color c = it.enabled ? (hot && hc ? g_th.accentText : txt) : faint;
        float ix = r.left + 20;
        if (it.check && it.icon == I_NONE) DrawIcon(I_CHECK, ix, (r.top + r.bottom) / 2, 13, it.enabled ? g_th.accent : faint, 1.5f);
        else if (it.icon != I_NONE) {
            DrawIcon(it.icon, ix, (r.top + r.bottom) / 2, 15 * g_textScale, c, 1.15f);
            if (it.check) FillRR(RF(r.left + 2, r.top + RH(r) * 0.3f, 3, RH(r) * 0.4f), 1.5f, g_th.accent);
        }
        Text(it.text, RF(r.left + 40, r.top, RW(r) - 40, RH(r)), UiFmt(13), c);
        if (!it.keys.empty()) Text(it.keys, RF(r.left, r.top, RW(r) - (it.sub.empty() ? 12 : 30), RH(r)), UiFmt(12), it.enabled ? (hot && hc ? g_th.accentText : dim) : faint, 2);
        if (!it.sub.empty()) DrawIcon(I_CHEV_RIGHT, r.right - 14, (r.top + r.bottom) / 2, 11, c, 1.3f);
    }
    HRESULT hr = g_rt->EndDraw();
    g_rt = saved;
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { f->frostBmp.Reset(); ReleaseBrushesFor(f->rt); f->rt->Release(); f->rt = nullptr; }
}

static void CloseFrom(size_t level) {
    while (s_flies.size() > level) {
        Fly* f = s_flies.back(); s_flies.pop_back();
        if (f->rt) { ReleaseBrushesFor(f->rt); f->rt->Release(); }
        DestroyWindow(f->h);
        delete f;
        if (!s_flies.empty()) s_flies.back()->openSub = -1;
    }
    for (auto* f : s_flies) InvalidateRect(f->h, nullptr, FALSE);
}

static LRESULT CALLBACK FlyProc(HWND h, UINT m, WPARAM wp, LPARAM lp);
static Fly* Open(const std::vector<MenuItem>* items, const std::vector<MenuItem>* bar, int sx, int sy, int flags, RECT avoid) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DROPSHADOW * 0; wc.lpfnWndProc = FlyProc; wc.hInstance = g_inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ModernNotepadFlyout";
        RegisterClassExW(&wc); reg = true;
    }
    Fly* f = new Fly; f->items = items; f->bar = bar;
    Measure(f);
    int w = (int)std::ceil(f->w * g_scale), hh = (int)std::ceil(f->hgt * g_scale);
    MONITORINFO mi{ sizeof(mi) }; POINT pt{ sx, sy };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    int x = sx, y = (flags & MF_ANCHOR_ABOVE) ? sy - hh : sy;
    if (avoid.right > avoid.left) {   // submenu: to the right of the parent, else left
        x = avoid.right - 4; y = sy;
        if (x + w > wa.right) x = avoid.left - w + 4;
    }
    if (x + w > wa.right) x = wa.right - w;
    if (x < wa.left) x = wa.left;
    if (y + hh > wa.bottom) y = (flags & MF_ANCHOR_ABOVE) ? wa.bottom - hh : std::max((LONG)wa.top, (LONG)(sy - hh));
    if (y < wa.top) y = wa.top;
    f->acrylic = FrostEnabled();
    if (f->acrylic) Snapshot(f, x, y, w, hh);
    if (f->frost.empty()) f->acrylic = false;
    f->h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"ModernNotepadFlyout", L"", WS_POPUP | (g_mica ? WS_THICKFRAME : 0),
                           x, y, w, hh, g_hwnd, nullptr, g_inst, nullptr);
    SetWindowLongPtrW(f->h, GWLP_USERDATA, (LONG_PTR)f);
    BOOL dark = g_th.dark; DwmSetWindowAttribute(f->h, 20, &dark, sizeof(dark));
    DWORD corner = 2; DwmSetWindowAttribute(f->h, 33, &corner, sizeof(corner));
    s_flies.push_back(f);
    SetWindowPos(f->h, HWND_TOPMOST, x, y, w, hh, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    return f;
}

static void OpenSub(Fly* f, int i) {
    size_t level = 0; for (size_t k = 0; k < s_flies.size(); ++k) if (s_flies[k] == f) level = k;
    CloseFrom(level + 1);
    const MenuItem& it = (*f->items)[i];
    if (it.sub.empty() || !it.enabled) return;
    f->openSub = i;
    RECT pr; GetWindowRect(f->h, &pr);
    int sy = pr.top + (int)((f->rItems[i].top - PAD) * g_scale);
    Fly* c = Open(&it.sub, nullptr, pr.right, sy, 0, pr);
    if (f->keyboard) { c->keyboard = true; for (size_t k = 0; k < it.sub.size(); ++k) if (!it.sub[k].sep && it.sub[k].enabled) { c->hover = (int)k; break; } }
    InvalidateRect(f->h, nullptr, FALSE);
}

static void Activate(Fly* f, int i) {
    if (i <= -2) {
        const MenuItem& b = (*f->bar)[-2 - i];
        if (b.enabled) { s_result = b.id; s_done = true; }
        return;
    }
    if (i < 0) return;
    const MenuItem& it = (*f->items)[i];
    if (it.sep || !it.enabled) return;
    if (!it.sub.empty()) { OpenSub(f, i); return; }
    s_result = it.id; s_done = true;
}

static int HitItem(Fly* f, float x, float y) {
    for (size_t i = 0; i < f->rBar.size(); ++i) if (In(f->rBar[i], x, y)) return -2 - (int)i;
    for (size_t i = 0; i < f->rItems.size(); ++i) if (In(f->rItems[i], x, y) && !(*f->items)[i].sep) return (int)i;
    return -1;
}

static LRESULT CALLBACK FlyProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Fly* f = (Fly*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCALCSIZE: if (wp) return 0; break;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); if (f) Paint(f); EndPaint(h, &ps); return 0; }
    case WM_MOUSEMOVE: {
        if (!f) break;
        float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
        int hit = HitItem(f, x, y);
        if (hit != f->hover || f->keyboard) {
            f->hover = hit; f->keyboard = false;
            KillTimer(h, 1);
            if (hit >= 0 && !(*f->items)[hit].sub.empty()) SetTimer(h, 1, 280, nullptr);
            else if (hit >= 0 || hit <= -2) SetTimer(h, 2, 380, nullptr);   // close stray child after a moment
            InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0; }
    case WM_MOUSELEAVE: if (f && f->openSub < 0) { f->hover = -1; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_TIMER:
        KillTimer(h, (UINT_PTR)wp);
        if (!f) return 0;
        if (wp == 1 && f->hover >= 0) OpenSub(f, f->hover);
        if (wp == 2 && f->hover != f->openSub && f->openSub >= 0) {
            size_t level = 0; for (size_t k = 0; k < s_flies.size(); ++k) if (s_flies[k] == f) level = k;
            CloseFrom(level + 1);
        }
        return 0;
    case WM_LBUTTONUP: {
        if (!f) break;
        int hit = HitItem(f, GET_X_LPARAM(lp) / g_scale, GET_Y_LPARAM(lp) / g_scale);
        Activate(f, hit);
        return 0; }
    case WM_RBUTTONUP: return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void MoveHover(Fly* f, int dir) {
    int n = (int)f->items->size(), barN = f->bar ? (int)f->bar->size() : 0;
    // linear order: bar items (as -2-i) then items
    std::vector<int> order;
    for (int i = 0; i < barN; ++i) if ((*f->bar)[i].enabled) order.push_back(-2 - i);
    for (int i = 0; i < n; ++i) if (!(*f->items)[i].sep && (*f->items)[i].enabled) order.push_back(i);
    if (order.empty()) return;
    int pos = -1;
    for (size_t k = 0; k < order.size(); ++k) if (order[k] == f->hover) pos = (int)k;
    pos = pos < 0 ? (dir > 0 ? 0 : (int)order.size() - 1) : (pos + dir + (int)order.size()) % (int)order.size();
    f->hover = order[pos]; f->keyboard = true;
    InvalidateRect(f->h, nullptr, FALSE);
}

static int Classic(const MenuModel& m, int sx, int sy, int flags) {
    std::function<HMENU(const std::vector<MenuItem>&)> build = [&](const std::vector<MenuItem>& items) {
        HMENU h = CreatePopupMenu();
        for (auto& it : items) {
            if (it.sep) { AppendMenuW(h, MF_SEPARATOR, 0, nullptr); continue; }
            std::wstring t = it.text; if (!it.keys.empty()) t += L"\t" + it.keys;
            UINT fl = MF_STRING | (it.check ? MF_CHECKED : 0) | (it.enabled ? 0 : MF_GRAYED);
            if (!it.sub.empty()) AppendMenuW(h, fl | MF_POPUP, (UINT_PTR)build(it.sub), t.c_str());
            else AppendMenuW(h, fl, it.id, t.c_str());
        }
        return h;
    };
    std::vector<MenuItem> all = m.bar;
    if (!all.empty()) all.push_back(MSep());
    all.insert(all.end(), m.items.begin(), m.items.end());
    HMENU h = build(all);
    int cmd = TrackPopupMenu(h, TPM_RETURNCMD | TPM_LEFTALIGN | ((flags & MF_ANCHOR_ABOVE) ? TPM_BOTTOMALIGN : TPM_TOPALIGN), sx, sy, 0, g_hwnd, nullptr);
    DestroyMenu(h);
    return cmd;
}

int RunMenu(const MenuModel& m, int sx, int sy, int flags) {
    if (!g_set.modernMenus) return Classic(m, sx, sy, flags);
    if (s_active) return 0;
    s_active = true; s_done = false; s_result = 0;
    RECT none{};
    Open(&m.items, &m.bar, sx, sy, flags, none);
    MSG msg;
    while (!s_done) {
        if (!GetMessageW(&msg, nullptr, 0, 0)) { PostQuitMessage((int)msg.wParam); break; }
        bool eat = false;
        switch (msg.message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            Fly* f = s_flies.back();
            switch (msg.wParam) {
            case VK_DOWN: MoveHover(f, 1); break;
            case VK_UP: MoveHover(f, -1); break;
            case VK_RIGHT:
                if (f->hover >= 0 && !(*f->items)[f->hover].sub.empty()) { f->keyboard = true; OpenSub(f, f->hover); }
                else if (f->hover <= -2) MoveHover(f, 1);
                break;
            case VK_LEFT:
                if (s_flies.size() > 1) CloseFrom(s_flies.size() - 1);
                else if (f->hover <= -2) MoveHover(f, -1);
                break;
            case VK_RETURN: case VK_SPACE: Activate(f, f->hover); break;
            case VK_ESCAPE: if (s_flies.size() > 1) CloseFrom(s_flies.size() - 1); else s_done = true; break;
            case VK_MENU: case VK_F10: s_done = true; break;
            }
            eat = true; break; }
        case WM_CHAR: case WM_SYSCHAR: {
            Fly* f = s_flies.back();
            wchar_t c = (wchar_t)towlower((wchar_t)msg.wParam);
            for (size_t i = 0; i < f->items->size(); ++i) {
                const MenuItem& it = (*f->items)[i];
                if (!it.sep && it.enabled && !it.text.empty() && towlower(it.text[0]) == c) { f->hover = (int)i; f->keyboard = true; Activate(f, (int)i); break; }
            }
            eat = true; break; }
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN:
            if (!IsFly(msg.hwnd)) { s_done = true; eat = true; }
            break;
        case WM_MOUSEWHEEL: eat = true; break;
        }
        if (!eat) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        HWND fg = GetForegroundWindow();
        if (fg != g_hwnd && !IsFly(fg)) s_done = true;
    }
    CloseFrom(0);
    s_active = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return s_result;
}

} // namespace app
