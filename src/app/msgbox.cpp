// msgbox.cpp - themed replacement for MessageBoxW (Win11 content-dialog look, follows Light/Dark/Pink).
// Own popup + Direct2D, modal loop like the real thing: owner disabled, Enter = default button,
// Esc = Cancel/No/OK, Tab/arrows move focus, Y/N/C/O mnemonics, Ctrl+C copies the text.
#include "app.h"
#include <cmath>
#include <dwmapi.h>

namespace app {

namespace {
struct Box {
    HWND h = nullptr; ID2D1HwndRenderTarget* rt = nullptr;
    std::wstring caption, text; UINT flags = 0;
    std::vector<std::pair<int, std::wstring>> btns;   // id, label
    std::vector<RectF> rb; RectF rClose;
    int focus = 0, hot = -1, press = -1, result = 0; bool hotClose = false, done = false;
    float w = 400, hgt = 200, textH = 20;
    Com<IDWriteTextLayout> layout;
};
Box* s_box = nullptr;
const float TITLE_H = 40, FOOT_H = 72, PADX = 24;

int IconKind(UINT f) {
    switch (f & MB_ICONMASK) { case MB_ICONERROR: return 1; case MB_ICONWARNING: return 2; case MB_ICONQUESTION: return 3; case MB_ICONINFORMATION: return 4; }
    return 0;
}
void Close(Box* b, int id) { b->result = id; b->done = true; }
int CancelId(Box* b) {
    for (auto& x : b->btns) if (x.first == IDCANCEL) return IDCANCEL;
    if (b->btns.size() == 1) return b->btns[0].first;
    for (auto& x : b->btns) if (x.first == IDNO) return IDNO;
    return 0;
}

void Paint(Box* b) {
    if (!b->rt) {
        RECT rc; GetClientRect(b->h, &rc);
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * g_scale, 96.f * g_scale);
        g_d2d->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(b->h, D2D1::SizeU(rc.right, rc.bottom)), &b->rt);
        if (!b->rt) return;
        b->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    ID2D1HwndRenderTarget* saved = g_rt; g_rt = b->rt;
    g_rt->BeginDraw();
    g_rt->Clear(g_th.surface);
    float W = b->w, H = b->hgt, ts = std::min(g_textScale, 1.4f);
    bool active = GetActiveWindow() == b->h;
    // title band (tinted with the accent like Win11 dialogs on a coloured theme)
    FillR(RF(0, 0, W, TITLE_H * ts), Mix(g_th.surface, g_th.accent, g_th.pink ? 0.10f : 0.04f));
    Text(b->caption, RF(16, 0, W - 70, TITLE_H * ts), UiFmt(12.5f), active ? g_th.text : g_th.dim);
    b->rClose = RF(W - 46, 0, 46, TITLE_H * ts - 6);
    if (b->hotClose) FillR(b->rClose, Hex(0xC42B1C));
    DrawIcon(I_CLOSE, (b->rClose.left + b->rClose.right) / 2, (b->rClose.top + b->rClose.bottom) / 2, 10, b->hotClose ? Hex(0xFFFFFF) : g_th.dim, 1.f);
    // icon + text
    float y = TITLE_H * ts + 20, tx = PADX;
    int ik = IconKind(b->flags);
    if (ik) {
        Color c = ik == 1 ? Hex(0xD13438) : ik == 2 ? Hex(0xF7B500) : g_th.accent;
        float cx = PADX + 16, cy = y + 16;
        g_rt->FillEllipse(D2D1::Ellipse({ cx, cy }, 16, 16), Brush(c));
        const wchar_t* g = ik == 1 ? L"✕" : ik == 2 ? L"!" : ik == 3 ? L"?" : L"i";
        Text(g, RF(cx - 16, cy - 16, 32, 32), UiFmt(ik == 1 ? 15.f : 18.f, true), ik == 2 ? Hex(0x1A1A1A) : (ik == 1 ? Hex(0xFFFFFF) : g_th.accentText), 1);
        tx = PADX + 32 + 16;
    }
    if (b->layout) g_rt->DrawTextLayout({ tx, y + std::max(0.f, (32 - b->textH) / 2) }, b->layout, Brush(g_th.text));
    // footer + buttons
    RectF foot = RF(0, H - FOOT_H * ts, W, FOOT_H * ts);
    FillR(foot, g_th.chrome); Line(0, foot.top + 0.5f, W, foot.top + 0.5f, g_th.border);
    int n = (int)b->btns.size();
    float bw = std::min(150.f * ts, (W - PADX * 2 - 8 * (n - 1)) / n), bh = 32 * ts;
    float bx = W - PADX - n * bw - (n - 1) * 8;
    if (n <= 2) bx = W - PADX - n * bw - (n - 1) * 8;
    b->rb.clear();
    for (int i = 0; i < n; ++i) {
        RectF r = RF(bx + i * (bw + 8), foot.top + (RH(foot) - bh) / 2, bw, bh);
        b->rb.push_back(r);
        bool primary = i == 0;   // first button is the default, accent-filled like Win11
        Color fill = primary ? g_th.accent : g_th.editor;
        if (b->hot == i) fill = primary ? Mix(g_th.accent, g_th.text, 0.12f) : Mix(g_th.editor, g_th.text, 0.06f);
        if (b->press == i && b->hot == i) fill = primary ? Mix(g_th.accent, g_th.surface, 0.25f) : Mix(g_th.editor, g_th.text, 0.10f);
        FillRR(r, 5, fill);
        if (!primary) StrokeRR(RF(r.left + 0.5f, r.top + 0.5f, RW(r) - 1, RH(r) - 1), 5, g_th.border);
        if (b->focus == i && active) StrokeRR(RF(r.left - 2.5f, r.top - 2.5f, RW(r) + 5, RH(r) + 5), 7, g_th.text, 1.5f);
        Text(b->btns[i].second, r, UiFmt(13), primary ? g_th.accentText : g_th.text, 1);
    }
    StrokeRR(RF(0.5f, 0.5f, W - 1, H - 1), 8, Alpha(g_th.border, 0.9f));
    HRESULT hr = g_rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { ReleaseBrushesFor(b->rt); b->rt->Release(); b->rt = nullptr; }
    g_rt = saved;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Box* b = s_box;
    if (!b || b->h != h) return DefWindowProcW(h, m, wp, lp);
    auto pt = [&](LPARAM l) { return D2D1_POINT_2F{ (short)LOWORD(l) / g_scale, (short)HIWORD(l) / g_scale }; };
    switch (m) {
    case WM_NCCALCSIZE: if (wp) return 0; break;
    case WM_NCHITTEST: {
        POINT p{ (short)LOWORD(lp), (short)HIWORD(lp) }; ScreenToClient(h, &p);
        float x = p.x / g_scale, y = p.y / g_scale;
        if (y < TITLE_H * std::min(g_textScale, 1.4f) && !In(b->rClose, x, y)) return HTCAPTION;
        return HTCLIENT; }
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); Paint(b); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE: InvalidateRect(h, nullptr, FALSE); break;
    case WM_MOUSEMOVE: {
        auto p = pt(lp); int hot = -1;
        for (int i = 0; i < (int)b->rb.size(); ++i) if (In(b->rb[i], p.x, p.y)) hot = i;
        bool hc = In(b->rClose, p.x, p.y);
        if (hot != b->hot || hc != b->hotClose) { b->hot = hot; b->hotClose = hc; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0; }
    case WM_MOUSELEAVE: b->hot = -1; b->hotClose = false; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: { auto p = pt(lp); b->press = -1; for (int i = 0; i < (int)b->rb.size(); ++i) if (In(b->rb[i], p.x, p.y)) b->press = i;
                           if (b->press >= 0) { b->focus = b->press; SetCapture(h); } InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_LBUTTONUP: {
        auto p = pt(lp); ReleaseCapture();
        if (In(b->rClose, p.x, p.y)) { int c = CancelId(b); if (c) Close(b, c); else MessageBeep(MB_OK); return 0; }
        if (b->press >= 0 && In(b->rb[b->press], p.x, p.y)) Close(b, b->btns[b->press].first);
        b->press = -1; InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_KEYDOWN: {
        int n = (int)b->btns.size();
        if (wp == VK_RETURN || wp == VK_SPACE) { Close(b, b->btns[b->focus].first); return 0; }
        if (wp == VK_ESCAPE) { int c = CancelId(b); if (c) Close(b, c); return 0; }
        if (wp == VK_TAB || wp == VK_RIGHT || wp == VK_LEFT) {
            bool back = wp == VK_LEFT || (wp == VK_TAB && GetKeyState(VK_SHIFT) < 0);
            b->focus = (b->focus + (back ? n - 1 : 1)) % n; InvalidateRect(h, nullptr, FALSE); return 0;
        }
        if (wp == 'C' && GetKeyState(VK_CONTROL) < 0) { ClipboardSet(b->caption + L"\r\n\r\n" + b->text); return 0; }
        for (int i = 0; i < n; ++i) if (towupper(b->btns[i].second[0]) == (wchar_t)wp) { Close(b, b->btns[i].first); return 0; }
        return 0; }
    case WM_SYSCOMMAND: if ((wp & 0xFFF0) == SC_CLOSE) { int c = CancelId(b); if (c) Close(b, c); return 0; } break;
    case WM_CLOSE: { int c = CancelId(b); if (c) Close(b, c); return 0; }
    }
    return DefWindowProcW(h, m, wp, lp);
}
} // namespace

int ThemedMessageBox(HWND owner, LPCWSTR text, LPCWSTR caption, UINT flags) {
    if (!g_d2d || !g_dw || s_box) return MessageBoxW(owner, text, caption, flags);
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{ sizeof(wc) }; wc.lpfnWndProc = Proc; wc.hInstance = g_inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ModernNotepadMsgBox"; wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(101));
        RegisterClassExW(&wc); reg = true;
    }
    Box b; b.caption = caption ? caption : L"ModernNotepad"; b.text = text ? text : L""; b.flags = flags;
    switch (flags & MB_TYPEMASK) {
    case MB_OKCANCEL: b.btns = { { IDOK, L"OK" }, { IDCANCEL, L"Cancel" } }; break;
    case MB_YESNOCANCEL: b.btns = { { IDYES, L"Yes" }, { IDNO, L"No" }, { IDCANCEL, L"Cancel" } }; break;
    case MB_YESNO: b.btns = { { IDYES, L"Yes" }, { IDNO, L"No" } }; break;
    case MB_RETRYCANCEL: b.btns = { { IDRETRY, L"Retry" }, { IDCANCEL, L"Cancel" } }; break;
    default: b.btns = { { IDOK, L"OK" } };
    }
    if (b.btns.size() == 3 && b.btns[0].first == IDYES) b.btns[0].second = L"Save", b.btns[1].second = L"Don't save";   // Win11 Notepad wording
    if ((flags & MB_DEFMASK) == MB_DEFBUTTON2 && b.btns.size() > 1) b.focus = 1;
    // measure
    float ts = std::min(g_textScale, 1.4f);
    float iconW = IconKind(flags) ? 48.f : 0.f;
    float maxTextW = 440 * ts;
    g_dw->CreateTextLayout(b.text.c_str(), (UINT32)b.text.size(), UiFmt(13.5f), maxTextW, 2000, &b.layout.p);
    float tw = 200; b.textH = 20;
    if (b.layout) { b.layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); DWRITE_TEXT_METRICS tm; b.layout->GetMetrics(&tm); tw = tm.widthIncludingTrailingWhitespace; b.textH = tm.height;
                    b.layout->SetMaxWidth(std::ceil(tw) + 2); }
    b.w = std::ceil(std::max(360.f * ts, PADX * 2 + iconW + tw + 8));
    b.w = std::max(b.w, PADX * 2 + b.btns.size() * 118 * ts);
    b.hgt = std::ceil(TITLE_H * ts + 20 + std::max(32.f, b.textH) + 26 + FOOT_H * ts);
    int pw = (int)std::ceil(b.w * g_scale), ph = (int)std::ceil(b.hgt * g_scale);
    RECT orc{}; HWND own = owner && IsWindow(owner) ? owner : nullptr;
    if (own) GetWindowRect(own, &orc); else { orc.right = GetSystemMetrics(SM_CXSCREEN); orc.bottom = GetSystemMetrics(SM_CYSCREEN); }
    int x = (orc.left + orc.right - pw) / 2, y = (orc.top + orc.bottom - ph) / 2 - (int)(30 * g_scale);
    s_box = &b;
    b.h = CreateWindowExW(WS_EX_DLGMODALFRAME | (own ? 0 : WS_EX_TOPMOST), L"ModernNotepadMsgBox", b.caption.c_str(), WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
                          x, y, pw, ph, own, nullptr, g_inst, nullptr);
    if (!b.h) { s_box = nullptr; return MessageBoxW(owner, text, caption, flags); }
    BOOL dark = g_th.dark; DwmSetWindowAttribute(b.h, 20, &dark, sizeof(dark));
    DWORD corner = 2; DwmSetWindowAttribute(b.h, 33, &corner, sizeof(corner));
    MARGINS mg{ 0, 0, 1, 0 }; DwmExtendFrameIntoClientArea(b.h, &mg);   // keeps the DWM shadow
    SetWindowPos(b.h, nullptr, x, y, pw, ph, SWP_NOZORDER | SWP_FRAMECHANGED);
    int ik = IconKind(flags);
    if (ik == 1) MessageBeep(MB_ICONERROR); else if (ik == 2) MessageBeep(MB_ICONWARNING); else if (ik == 3) MessageBeep(MB_ICONQUESTION);
    if (own) EnableWindow(own, FALSE);
    ShowWindow(b.h, SW_SHOW); SetForegroundWindow(b.h); SetFocus(b.h);
    MSG msg;
    while (!b.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); break; }
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN || msg.message == WM_CHAR) && msg.hwnd != b.h) msg.hwnd = b.h;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (own) EnableWindow(own, TRUE);
    if (b.rt) { ReleaseBrushesFor(b.rt); b.rt->Release(); b.rt = nullptr; }
    DestroyWindow(b.h);
    s_box = nullptr;
    if (own) { SetActiveWindow(own); SetForegroundWindow(own); }
    return b.result ? b.result : IDCANCEL;
}

} // namespace app
