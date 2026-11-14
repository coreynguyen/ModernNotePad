// gridview.cpp - table view for CSV/TSV documents. The TextDoc remains the source of truth:
// single-cell edits are minimal range replacements; structural edits (sort, insert/delete
// column, multi-cell paste/clear) re-serialize the table as one undoable step.
#include "app.h"
#include <cmath>
#include <cwctype>

namespace app {

static RectF s_area, s_cells, s_vbar, s_hbar;
static float s_rowH = 24, s_hdrH = 26, s_rowHdrW = 48, s_cw = 7;
static HWND s_edit = nullptr; static WNDPROC s_editOrig = nullptr;
static int s_editR = -1, s_editC = -1;
static int s_drag = 0;   // 1 select cells, 2 resize column, 3 vscroll, 4 hscroll, 5 select rows, 6 select cols
static int s_resizeCol = -1; static float s_resizeX0 = 0, s_resizeW0 = 0;

static int HeadOff(Doc* d) { return d->gHeader ? 1 : 0; }
static int DataRows(Doc* d) { return d->table.Rows(); }
static int ViewRows(Doc* d) { return std::max(0, DataRows(d) - HeadOff(d)) + 1; }   // +1 blank row to append
static int ViewCols(Doc* d) { return d->table.Cols() + 1; }                           // +1 blank column
static std::wstring ColName(int c) { std::wstring s; ++c; while (c > 0) { int m = (c - 1) % 26; s.insert(s.begin(), (wchar_t)(L'A' + m)); c = (c - 1) / 26; } return s; }

static void Sync(Doc* d) {
    if (!d->delim) d->delim = np::DetectDelimiter(d->text);
    if (d->table.Stale(d->text) || d->table.delim != d->delim) d->table.Build(d->text, d->delim);
    int cols = ViewCols(d);
    if ((int)d->colW.size() < cols) {
        size_t old = d->colW.size();
        d->colW.resize(cols, 0);
        for (int c = (int)old; c < cols; ++c) {
            size_t mx = 4;
            int rows = d->table.Rows(), probe = std::min(rows, 300);
            for (int r = 0; r < probe; ++r) mx = std::max(mx, std::min<size_t>(d->table.Value(d->text, r, c).size(), 48));
            for (int r = std::max(probe, rows - 100); r < rows; ++r) mx = std::max(mx, std::min<size_t>(d->table.Value(d->text, r, c).size(), 48));
            d->colW[c] = std::max(64.f, std::min(380.f, mx * s_cw + 22));
        }
    }
    int lastRow = HeadOff(d) + ViewRows(d) - 1;
    d->gr = std::max(HeadOff(d) ? 0 : 0, std::min(d->gr, lastRow)); d->ga = std::max(0, std::min(d->ga, lastRow));
    d->gc = std::max(0, std::min(d->gc, cols - 1)); d->gca = std::max(0, std::min(d->gca, cols - 1));
}

void GridEnter(Doc* d) {
    d->grid = true; d->hex = false;
    d->delim = 0; d->colW.clear();
    d->gr = d->ga = d->gHeader ? 1 : 0; d->gc = d->gca = 0; d->gTop = 0; d->gScrollX = 0;
}

static float ColX(Doc* d, int c) { float x = s_cells.left - d->gScrollX; for (int i = 0; i < c && i < (int)d->colW.size(); ++i) x += d->colW[i]; return x; }
static float TotalW(Doc* d) { float w = 0; for (float x : d->colW) w += x; return w; }
static int VisRows() { return std::max(1, (int)(RH(s_cells) / s_rowH)); }

static void Ensure(Doc* d) {
    int vr = d->gr - HeadOff(d);
    if (vr >= 0) {
        if (vr < d->gTop) d->gTop = vr;
        if (vr >= d->gTop + VisRows()) d->gTop = vr - VisRows() + 1;
    }
    float x0 = ColX(d, d->gc) + d->gScrollX - s_cells.left, x1 = x0 + d->colW[d->gc];
    if (x0 < d->gScrollX) d->gScrollX = x0;
    if (x1 > d->gScrollX + RW(s_cells)) d->gScrollX = x1 - RW(s_cells);
    d->gScrollX = std::max(0.f, d->gScrollX);
}

void GridPaint(Doc* d, const RectF& area) {
    float z = g_set.zoom / 100.f * g_textScale;
    IDWriteTextFormat* f = UiFmt(13 * g_set.zoom / 100.f);
    IDWriteTextFormat* fb = UiFmt(13 * g_set.zoom / 100.f, true);
    IDWriteTextFormat* fs = UiFmt(11.5f * g_set.zoom / 100.f);
    s_cw = TextW(L"0000000000", f) / 10.f;
    s_rowH = std::round(26 * z); s_hdrH = std::round(28 * z);
    Sync(d);
    int digits = 1; for (int n = ViewRows(d); n >= 10; n /= 10) ++digits;
    s_rowHdrW = std::max(3, digits) * s_cw + 20;
    s_area = area;
    s_cells = { area.left + s_rowHdrW, area.top + s_hdrH, area.right - 12, area.bottom - 12 };
    s_vbar = { area.right - 12, area.top + s_hdrH, area.right, area.bottom - 12 };
    s_hbar = { area.left + s_rowHdrW, area.bottom - 12, area.right - 12, area.bottom };
    FillR(area, g_th.editor);
    Color hdrBg = g_th.dark ? Hex(0x2F2F2F) : Hex(0xF3F3F3), grid = g_th.dark ? Hex(0x3A3A3A) : Hex(0xE4E4E4);
    int r0 = std::min(d->ga, d->gr), r1 = std::max(d->ga, d->gr), c0 = std::min(d->gca, d->gc), c1 = std::max(d->gca, d->gc);
    int cols = ViewCols(d);
    // cells
    g_rt->PushAxisAlignedClip(s_cells, D2D1_ANTIALIAS_MODE_ALIASED);
    int vis = VisRows() + 1;
    for (int i = 0; i < vis; ++i) {
        int vr = d->gTop + i; if (vr >= ViewRows(d)) break;
        int dr = vr + HeadOff(d);
        float y = s_cells.top + i * s_rowH;
        const std::vector<np::CsvCell>* row = dr < d->table.Rows() ? &d->table.Row(d->text, dr) : nullptr;
        for (int c = 0; c < cols; ++c) {
            float x = ColX(d, c), w = d->colW[c];
            if (x > s_cells.right) break;
            if (x + w < s_cells.left) continue;
            RectF cr = RF(x, y, w, s_rowH);
            bool sel = dr >= r0 && dr <= r1 && c >= c0 && c <= c1;
            if (sel) FillR(cr, Alpha(g_th.accent, 0.16f));
            if (row && c < (int)row->size()) {
                const std::wstring& v = (*row)[c].v;
                std::wstring shown = v; for (auto& ch : shown) if (ch == L'\n' || ch == L'\r') ch = L'\u00B6';
                bool num = np::CsvIsNumber(v);
                Text(shown, RF(x + 7, y, w - 14, s_rowH), f, g_th.text, num ? 2 : 0);
            }
            Line(x + w - 0.5f, y, x + w - 0.5f, y + s_rowH, grid);
        }
        Line(s_cells.left, y + s_rowH - 0.5f, std::min(s_cells.right, ColX(d, cols)), y + s_rowH - 0.5f, grid);
    }
    // cursor
    if (d->gr >= HeadOff(d)) {
        int vr = d->gr - HeadOff(d) - d->gTop;
        if (vr >= 0 && vr < vis) {
            RectF cr = RF(ColX(d, d->gc), s_cells.top + vr * s_rowH, d->colW[d->gc], s_rowH);
            StrokeRR(RF(cr.left - 1, cr.top - 1, RW(cr) + 2, RH(cr) + 2), 2, g_th.accent, 2);
        }
    }
    g_rt->PopAxisAlignedClip();
    // column header bar
    FillR(RF(area.left, area.top, RW(area), s_hdrH), hdrBg);
    g_rt->PushAxisAlignedClip(RF(s_cells.left, area.top, RW(s_cells), s_hdrH), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int c = 0; c < cols; ++c) {
        float x = ColX(d, c), w = d->colW[c];
        if (x > s_cells.right) break;
        if (x + w < s_cells.left) continue;
        RectF hr = RF(x, area.top, w, s_hdrH);
        bool selC = c >= c0 && c <= c1;
        if (selC) FillR(hr, Alpha(g_th.accent, 0.22f));
        if (d->gHeader) {
            std::wstring name = d->table.Rows() ? d->table.Value(d->text, 0, c) : L"";
            if (name.empty() && c < cols - 1) name = ColName(c);
            Text(name, RF(x + 7, area.top, w - 14, s_hdrH), fb, g_th.text);
            if (d->gr == 0 && d->gc == c) StrokeRR(RF(x, area.top + 1, w, s_hdrH - 2), 2, g_th.accent, 2);
        } else Text(ColName(c), RF(x, area.top, w, s_hdrH), fs, g_th.dim, 1);
        Line(x + w - 0.5f, area.top + 4, x + w - 0.5f, area.top + s_hdrH - 4, grid);
    }
    g_rt->PopAxisAlignedClip();
    Line(area.left, area.top + s_hdrH - 0.5f, area.right, area.top + s_hdrH - 0.5f, grid);
    // row header column
    FillR(RF(area.left, area.top + s_hdrH, s_rowHdrW, RH(area) - s_hdrH), hdrBg);
    for (int i = 0; i < vis; ++i) {
        int vr = d->gTop + i; if (vr >= ViewRows(d)) break;
        int dr = vr + HeadOff(d);
        float y = s_cells.top + i * s_rowH;
        if (y > s_cells.bottom) break;
        if (dr >= r0 && dr <= r1) FillR(RF(area.left, y, s_rowHdrW, s_rowH), Alpha(g_th.accent, 0.22f));
        Text(vr == ViewRows(d) - 1 ? L"+" : std::to_wstring(vr + 1), RF(area.left, y, s_rowHdrW - 8, s_rowH), fs, g_th.dim, 2);
    }
    Line(s_cells.left - 0.5f, area.top, s_cells.left - 0.5f, area.bottom, grid);
    if (d->gHeader) DrawIcon(I_TABLE, area.left + s_rowHdrW / 2, area.top + s_hdrH / 2, 13, g_th.dim);
    // scrollbars
    int total = ViewRows(d), visr = VisRows();
    if (total > visr) {
        float trackH = RH(s_vbar) - 4, th = std::max(24.f, trackH * visr / total);
        float ty = s_vbar.top + 2 + (trackH - th) * d->gTop / std::max(1, total - visr);
        FillRR(RF(s_vbar.right - 6, ty, 4, th), 2, g_th.scrollThumb);
    }
    float tw = TotalW(d), vw = RW(s_cells);
    if (tw > vw) {
        float trackW = RW(s_hbar) - 4, w = std::max(24.f, trackW * vw / tw);
        float tx = s_hbar.left + 2 + (trackW - w) * std::min(1.f, d->gScrollX / (tw - vw));
        FillRR(RF(tx, s_hbar.bottom - 6, w, 4), 2, g_th.scrollThumb);
    }
    // inline editor position
    if (s_edit && s_editR >= 0) {
        float x, y;
        if (s_editR == 0 && d->gHeader) { x = ColX(d, s_editC); y = area.top; }
        else { x = ColX(d, s_editC); y = s_cells.top + (s_editR - HeadOff(d) - d->gTop) * s_rowH; }
        float w = std::max(d->colW[s_editC], 160.f);
        FillR(RF(x, y, w, s_rowH), g_th.editor); StrokeRR(RF(x, y, w, s_rowH), 2, g_th.accent, 2);
        SetWindowPos(s_edit, HWND_TOP, (int)((x + 6) * g_scale), (int)((y + (s_rowH - 18) / 2) * g_scale), (int)((w - 12) * g_scale), (int)(18 * g_scale), SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
}

// ------------------------------------------------------------------ editing
static void ApplyCell(Doc* d, int r, int c, const std::wstring& v) {
    Sync(d);
    if (r < d->table.Rows() && d->table.Value(d->text, r, c) == v) return;
    np::Pos a, b; std::wstring repl;
    d->table.CellEdit(d->text, r, c, v, a, b, repl);
    d->text.BreakCoalesce();
    d->text.Replace(a, b, repl, d->caret, d->anchor);
    EditorAfterEdit(d);
    Sync(d);
}
static void ReplaceAll(Doc* d, const std::vector<std::vector<std::wstring>>& rows) {
    std::wstring s = np::CsvTable::Serialize(rows, d->delim);
    if (!d->text.Line(d->text.LineCount() - 1).empty() || d->text.LineCount() == 1) {} 
    s += L"\n";
    d->text.BreakCoalesce();
    d->text.BeginGroup();
    d->text.Replace({ 0, 0 }, d->text.End(), s, d->caret, d->anchor);
    d->text.EndGroup();
    EditorAfterEdit(d);
    Sync(d);
}

static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp);
static void Commit(bool keep);
bool GridEditing() { return s_edit != nullptr; }
void GridCommit() { Commit(true); }
void GridCancelEdit() { if (!s_edit) return; HWND e = s_edit; s_edit = nullptr; s_editR = -1; DestroyWindow(e); SetFocus(g_hwnd); Invalidate(); }
static void BeginEdit(Doc* d, const std::wstring* initial) {
    Sync(d);
    if (s_edit) GridCancelEdit();
    s_editR = d->gr; s_editC = d->gc;
    std::wstring v = initial ? *initial : d->table.Value(d->text, d->gr, d->gc);
    s_edit = CreateWindowExW(0, L"EDIT", v.c_str(), WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)9010, g_inst, nullptr);
    SendMessageW(s_edit, WM_SETFONT, (WPARAM)UiEditFont(), TRUE);
    s_editOrig = (WNDPROC)SetWindowLongPtrW(s_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    SetFocus(s_edit);
    int n = GetWindowTextLengthW(s_edit);
    SendMessageW(s_edit, EM_SETSEL, initial ? n : 0, n);
    Invalidate();
}
static void Commit(bool keep) {
    if (!s_edit) return;
    Doc* d = Cur();
    int n = GetWindowTextLengthW(s_edit);
    std::wstring v(n + 1, 0); GetWindowTextW(s_edit, &v[0], n + 1); v.resize(n);
    int r = s_editR, c = s_editC;
    HWND e = s_edit; s_edit = nullptr; s_editR = -1;
    DestroyWindow(e);
    if (keep && d && d->grid) ApplyCell(d, r, c, v);
    SetFocus(g_hwnd);
    Invalidate();
}
static void Move(Doc* d, int dr, int dc, bool extend) {
    int lastRow = HeadOff(d) + ViewRows(d) - 1;
    d->gr = std::max(HeadOff(d), std::min(lastRow, d->gr + dr));   // header row is reached by clicking it
    d->gc = std::max(0, std::min(ViewCols(d) - 1, d->gc + dc));
    if (!extend) { d->ga = d->gr; d->gca = d->gc; }
    Ensure(d); Invalidate();
}
static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN) {
        Doc* d = Cur();
        bool shift = GetKeyState(VK_SHIFT) < 0;
        if (wp == VK_ESCAPE) { GridCancelEdit(); return 0; }
        if (wp == VK_RETURN) { Commit(true); if (d) Move(d, shift ? -1 : 1, 0, false); return 0; }
        if (wp == VK_TAB) { Commit(true); if (d) Move(d, 0, shift ? -1 : 1, false); return 0; }
        if (wp == VK_UP || wp == VK_DOWN) { Commit(true); if (d) Move(d, wp == VK_UP ? -1 : 1, 0, false); return 0; }
    }
    if (m == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) return 0;
    if (m == WM_KILLFOCUS && s_edit == h) { LRESULT r = CallWindowProcW(s_editOrig, h, m, wp, lp); PostMessageW(g_hwnd, WM_APP_GRIDCOMMIT, 0, 0); return r; }
    return CallWindowProcW(s_editOrig, h, m, wp, lp);
}

// ------------------------------------------------------------------ hit testing / mouse
static bool HitCell(Doc* d, float x, float y, int& r, int& c) {
    c = ViewCols(d) - 1;
    for (int i = 0; i < ViewCols(d); ++i) if (x < ColX(d, i) + d->colW[i]) { c = i; break; }
    int vr = d->gTop + (int)std::floor((y - s_cells.top) / s_rowH);
    vr = std::max(0, std::min(ViewRows(d) - 1, vr));
    r = vr + HeadOff(d);
    return true;
}

bool GridMouse(Doc* d, UINT msg, float x, float y, WPARAM wp) {
    if (!In(s_area, x, y) && !s_drag) return false;
    Sync(d);
    bool shift = (wp & MK_SHIFT) != 0;
    if (msg == WM_MOUSEMOVE) {
        if (!s_drag) {
            bool border = false;
            if (y >= s_area.top && y < s_area.top + s_hdrH) for (int c = 0; c < ViewCols(d); ++c) if (std::fabs(x - (ColX(d, c) + d->colW[c])) < 4) border = true;
            SetCursor(LoadCursor(nullptr, border ? IDC_SIZEWE : IDC_ARROW));
            return true;
        }
        if (s_drag == 2) { d->colW[s_resizeCol] = std::max(28.f, s_resizeW0 + x - s_resizeX0); SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); Invalidate(); return true; }
        if (s_drag == 3) { int total = ViewRows(d); float t = (y - s_vbar.top) / std::max(1.f, RH(s_vbar)); d->gTop = std::max(0, std::min(std::max(0, total - VisRows()), (int)(t * total) - VisRows() / 2)); Invalidate(); return true; }
        if (s_drag == 4) { float t = (x - s_hbar.left) / std::max(1.f, RW(s_hbar)); d->gScrollX = std::max(0.f, std::min(TotalW(d) - RW(s_cells), t * TotalW(d) - RW(s_cells) / 2)); Invalidate(); return true; }
        int r, c; HitCell(d, x, y, r, c);
        if (s_drag == 1) { d->gr = r; d->gc = c; }
        if (s_drag == 5) d->gr = r;
        if (s_drag == 6) d->gc = c;
        if (y > s_cells.bottom) d->gTop = std::min(ViewRows(d) - 1, d->gTop + 1);
        if (y < s_cells.top && s_drag != 6) d->gTop = std::max(0, d->gTop - 1);
        Invalidate(); return true;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        if (s_edit) Commit(true);
        SetCapture(g_hwnd);
        if (In(s_vbar, x, y)) { s_drag = 3; return GridMouse(d, WM_MOUSEMOVE, x, y, wp); }
        if (In(s_hbar, x, y)) { s_drag = 4; return GridMouse(d, WM_MOUSEMOVE, x, y, wp); }
        if (y < s_area.top + s_hdrH) {
            for (int c = 0; c < ViewCols(d); ++c) if (std::fabs(x - (ColX(d, c) + d->colW[c])) < 4) {
                if (msg == WM_LBUTTONDBLCLK) { d->colW.clear(); Sync(d); ReleaseCapture(); Invalidate(); return true; }   // auto-fit all
                s_drag = 2; s_resizeCol = c; s_resizeX0 = x; s_resizeW0 = d->colW[c]; return true;
            }
            if (x < s_cells.left) { d->ga = HeadOff(d); d->gr = HeadOff(d) + ViewRows(d) - 1; d->gca = 0; d->gc = ViewCols(d) - 1; Invalidate(); ReleaseCapture(); return true; }
            int r, c; HitCell(d, x, s_cells.top, r, c);
            if (d->gHeader && msg == WM_LBUTTONDBLCLK) { d->gr = d->ga = 0; d->gc = d->gca = c; ReleaseCapture(); BeginEdit(d, nullptr); return true; }
            if (!shift) d->gca = c;
            d->gc = c; d->ga = HeadOff(d); d->gr = HeadOff(d) + std::max(0, ViewRows(d) - 2);
            s_drag = 6; Invalidate(); return true;
        }
        int r, c; HitCell(d, x, y, r, c);
        if (x < s_cells.left) { if (!shift) d->ga = r; d->gr = r; d->gca = 0; d->gc = ViewCols(d) - 1; s_drag = 5; Invalidate(); return true; }
        d->gr = r; d->gc = c;
        if (!shift) { d->ga = r; d->gca = c; }
        if (msg == WM_LBUTTONDBLCLK) { ReleaseCapture(); BeginEdit(d, nullptr); return true; }
        s_drag = 1; Invalidate(); return true;
    }
    if (msg == WM_LBUTTONUP) { if (s_drag) { s_drag = 0; ReleaseCapture(); } return true; }
    if (msg == WM_RBUTTONDOWN) {
        int r, c; HitCell(d, x, std::max(y, s_cells.top), r, c);
        int r0 = std::min(d->ga, d->gr), r1 = std::max(d->ga, d->gr), c0 = std::min(d->gca, d->gc), c1 = std::max(d->gca, d->gc);
        if (!(r >= r0 && r <= r1 && c >= c0 && c <= c1)) { d->gr = d->ga = r; d->gc = d->gca = c; }
        Invalidate(); return true;
    }
    return true;
}

void GridWheel(Doc* d, int delta, bool horizontal) {
    if (horizontal || GetKeyState(VK_SHIFT) < 0) d->gScrollX = std::max(0.f, std::min(std::max(0.f, TotalW(d) - RW(s_cells)), d->gScrollX - delta * 0.8f));
    else d->gTop = std::max(0, std::min(std::max(0, ViewRows(d) - VisRows()), d->gTop - delta / 40));
    Invalidate();
}

bool GridKey(Doc* d, WPARAM vk) {
    if (s_edit) return false;
    Sync(d);
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    int lastRow = HeadOff(d) + ViewRows(d) - 1, lastCol = ViewCols(d) - 1;
    switch (vk) {
    case VK_UP: Move(d, ctrl ? -1000000 : -1, 0, shift); return true;
    case VK_DOWN: Move(d, ctrl ? lastRow : 1, 0, shift); return true;
    case VK_LEFT: Move(d, 0, ctrl ? -1000000 : -1, shift); return true;
    case VK_RIGHT: Move(d, 0, ctrl ? lastCol : 1, shift); return true;
    case VK_PRIOR: Move(d, -VisRows(), 0, shift); return true;
    case VK_NEXT: Move(d, VisRows(), 0, shift); return true;
    case VK_HOME: if (ctrl) Move(d, -1000000, -1000000, shift); else Move(d, 0, -1000000, shift); return true;
    case VK_END: if (ctrl) Move(d, lastRow, lastCol, shift); else Move(d, 0, std::max(0, d->table.Cols() - 1) - d->gc, shift); return true;
    case VK_TAB: Move(d, 0, shift ? -1 : 1, false); return true;
    case VK_RETURN: if (ctrl) return false; Move(d, shift ? -1 : 1, 0, false); return true;
    case VK_F2: BeginEdit(d, nullptr); return true;
    case VK_DELETE: case VK_BACK: {
        int r0 = std::min(d->ga, d->gr), r1 = std::max(d->ga, d->gr), c0 = std::min(d->gca, d->gc), c1 = std::max(d->gca, d->gc);
        if (r0 == r1 && c0 == c1) { if (vk == VK_BACK) { std::wstring e; BeginEdit(d, &e); } else ApplyCell(d, r0, c0, L""); return true; }
        auto all = d->table.All(d->text);
        for (int r = r0; r <= r1 && r < (int)all.size(); ++r) for (int c = c0; c <= c1 && c < (int)all[r].size(); ++c) all[r][c].clear();
        ReplaceAll(d, all); return true; }
    case VK_ESCAPE: d->ga = d->gr; d->gca = d->gc; Invalidate(); return true;
    }
    return false;
}
void GridChar(Doc* d, wchar_t ch) {
    if (s_edit || ch < 0x20 || ch == 0x7F) return;
    std::wstring s(1, ch);
    BeginEdit(d, &s);
}

// ------------------------------------------------------------------ clipboard
void GridCopy(Doc* d, bool cut) {
    Sync(d);
    int r0 = std::min(d->ga, d->gr), r1 = std::max(d->ga, d->gr), c0 = std::min(d->gca, d->gc), c1 = std::max(d->gca, d->gc);
    std::wstring s;
    for (int r = r0; r <= r1; ++r) {
        if (r > r0) s += L'\n';
        for (int c = c0; c <= c1; ++c) { if (c > c0) s += L'\t'; std::wstring v = d->table.Value(d->text, r, c); s += v.find_first_of(L"\t\n\"") != std::wstring::npos ? np::CsvQuote(v, L'\t') : v; }
    }
    ClipboardSet(s);
    if (cut) GridKey(d, VK_DELETE);
}
void GridPaste(Doc* d) {
    Sync(d);
    std::wstring s = np::NormalizeNewlines(ClipboardGet());
    if (s.empty()) return;
    while (!s.empty() && s.back() == L'\n') s.pop_back();
    np::TextDoc tmp; tmp.SetText(s);
    np::CsvTable t; t.Build(tmp, s.find(L'\t') != std::wstring::npos ? L'\t' : np::DetectDelimiter(tmp));
    auto src = t.All(tmp);
    if (src.size() == 1 && src[0].size() == 1) { ApplyCell(d, d->gr, d->gc, src[0][0]); return; }
    auto all = d->table.All(d->text);
    for (size_t i = 0; i < src.size(); ++i)
        for (size_t j = 0; j < src[i].size(); ++j) {
            size_t r = d->gr + i, c = d->gc + j;
            if (all.size() <= r) all.resize(r + 1);
            if (all[r].size() <= c) all[r].resize(c + 1);
            all[r][c] = src[i][j];
        }
    ReplaceAll(d, all);
    d->ga = d->gr; d->gca = d->gc; d->gr += (int)src.size() - 1; d->gc += (int)(src.empty() ? 0 : src[0].size()) - 1;
    Invalidate();
}

// ------------------------------------------------------------------ commands
void GridCommand(Doc* d, int cmd) {
    if (s_edit) Commit(true);
    Sync(d);
    if (cmd >= C_GRID_DELIM0 && cmd < C_GRID_DELIM0 + 4) {
        static const wchar_t dl[4] = { L',', L';', L'\t', L'|' };
        d->delim = dl[cmd - C_GRID_DELIM0]; d->colW.clear(); Sync(d); Invalidate(); return;
    }
    int r0 = std::min(d->ga, d->gr), r1 = std::max(d->ga, d->gr), c0 = std::min(d->gca, d->gc), c1 = std::max(d->gca, d->gc);
    switch (cmd) {
    case C_GRID_HEADER: d->gHeader = !d->gHeader; if (d->gr < HeadOff(d)) d->gr = d->ga = HeadOff(d); break;
    case C_GRID_EDIT: BeginEdit(d, nullptr); return;
    case C_GRID_AUTOFIT: d->colW.clear(); Sync(d); break;
    case C_GRID_SORTASC: case C_GRID_SORTDESC: {
        auto all = d->table.All(d->text);
        size_t from = d->gHeader ? 1 : 0;
        if (all.size() <= from + 1) break;
        int col = d->gc; bool asc = cmd == C_GRID_SORTASC;
        std::stable_sort(all.begin() + from, all.end(), [&](const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
            const std::wstring& x = col < (int)a.size() ? a[col] : L""; const std::wstring& y = col < (int)b.size() ? b[col] : L"";
            if (x.empty() != y.empty()) return y.empty();   // blanks last
            double nx, ny; bool bx = np::CsvIsNumber(x, &nx), by = np::CsvIsNumber(y, &ny);
            int cmpv = (bx && by) ? (nx < ny ? -1 : nx > ny ? 1 : 0) : (bx != by ? (bx ? -1 : 1) : _wcsicmp(x.c_str(), y.c_str()));
            return asc ? cmpv < 0 : cmpv > 0;
        });
        ReplaceAll(d, all);
        SetStatus(std::wstring(L"Sorted by ") + (d->gHeader ? d->table.Value(d->text, 0, col) : ColName(col)) + (asc ? L" (A→Z)" : L" (Z→A)"));
        break; }
    case C_GRID_INSROWABOVE: case C_GRID_INSROWBELOW: {
        int at = cmd == C_GRID_INSROWABOVE ? r0 : r1 + 1;
        auto all = d->table.All(d->text);
        at = std::min(at, (int)all.size());
        all.insert(all.begin() + at, std::vector<std::wstring>(std::max(1, d->table.Cols()), L""));
        ReplaceAll(d, all); d->gr = d->ga = at; break; }
    case C_GRID_DELROW: {
        auto all = d->table.All(d->text);
        if (r0 >= (int)all.size()) break;
        all.erase(all.begin() + r0, all.begin() + std::min((int)all.size(), r1 + 1));
        ReplaceAll(d, all); d->gr = d->ga = std::max(HeadOff(d), r0); break; }
    case C_GRID_INSCOLLEFT: case C_GRID_INSCOLRIGHT: {
        int at = cmd == C_GRID_INSCOLLEFT ? c0 : c1 + 1;
        auto all = d->table.All(d->text);
        for (auto& row : all) { if ((int)row.size() < at) row.resize(at); row.insert(row.begin() + at, L""); }
        if (d->gHeader && !all.empty()) all[0][at] = L"Column " + std::to_wstring(at + 1);
        ReplaceAll(d, all); d->colW.insert(d->colW.begin() + std::min((int)d->colW.size(), at), 90.f); d->gc = d->gca = at; break; }
    case C_GRID_DELCOL: {
        auto all = d->table.All(d->text);
        for (auto& row : all) if (c0 < (int)row.size()) row.erase(row.begin() + c0, row.begin() + std::min((int)row.size(), c1 + 1));
        ReplaceAll(d, all);
        if (c0 < (int)d->colW.size()) d->colW.erase(d->colW.begin() + c0, d->colW.begin() + std::min((int)d->colW.size(), c1 + 1));
        d->gc = d->gca = std::max(0, c0 - (c0 >= d->table.Cols() ? 1 : 0)); break; }
    }
    Sync(d); Ensure(d); Invalidate();
}

void GridContextMenu(Doc* d, int sx, int sy) {
    MenuModel m;
    m.bar = { MI(C_CUT, L"Cut", L"", I_CUT), MI(C_COPY, L"Copy", L"", I_COPY), MI(C_PASTE, L"Paste", L"", I_PASTE),
              MI(C_SELALL, L"Select all", L"", I_SELECTALL), MI(C_DELETE, L"Clear", L"", I_DELETE) };
    m.items = { MI(C_GRID_EDIT, L"Edit cell", L"F2", I_FONT), MSep(),
                MI(C_GRID_SORTASC, L"Sort A → Z", L"", I_SORT), MI(C_GRID_SORTDESC, L"Sort Z → A", L"", I_SORT), MSep(),
                MI(C_GRID_INSROWABOVE, L"Insert row above", L"", I_ARROW_UP), MI(C_GRID_INSROWBELOW, L"Insert row below", L"", I_ARROW_DOWN),
                MI(C_GRID_DELROW, L"Delete row", L"", I_DELETE), MSep(),
                MI(C_GRID_INSCOLLEFT, L"Insert column left", L"", I_BACK), MI(C_GRID_INSCOLRIGHT, L"Insert column right", L"", I_GOTO),
                MI(C_GRID_DELCOL, L"Delete column", L"", I_DELETE), MSep(),
                MI(C_GRID_HEADER, L"First row is header", L"", I_NONE, d->gHeader), MI(C_GRID_AUTOFIT, L"Auto-fit columns", L"", I_INDENT),
                MSub(L"Delimiter", I_CODE, { MI(C_GRID_DELIM0, L"Comma  ,", L"", I_NONE, d->delim == L','), MI(C_GRID_DELIM0 + 1, L"Semicolon  ;", L"", I_NONE, d->delim == L';'),
                                              MI(C_GRID_DELIM0 + 2, L"Tab", L"", I_NONE, d->delim == L'\t'), MI(C_GRID_DELIM0 + 3, L"Pipe  |", L"", I_NONE, d->delim == L'|') }),
                MSep(), MI(C_GRID, L"Show as text", L"Ctrl+Shift+T", I_FILE) };
    int cmd = RunMenu(m, sx, sy);
    if (cmd) RunCommand(cmd);
}

std::wstring GridStatus(Doc* d, std::wstring& stats) {
    Sync(d);
    int r0 = std::min(d->ga, d->gr), r1 = std::max(d->ga, d->gr), c0 = std::min(d->gca, d->gc), c1 = std::max(d->gca, d->gc);
    stats.clear();
    if (r0 != r1 || c0 != c1) {
        double sum = 0, mn = 1e300, mx = -1e300; long cnt = 0, nonEmpty = 0;
        long cells = 0;
        for (int r = r0; r <= r1 && r < d->table.Rows() && cells < 2000000; ++r)
            for (int c = c0; c <= c1; ++c, ++cells) {
                std::wstring v = d->table.Value(d->text, r, c);
                if (!v.empty()) ++nonEmpty;
                double x; if (np::CsvIsNumber(v, &x)) { sum += x; mn = std::min(mn, x); mx = std::max(mx, x); ++cnt; }
            }
        wchar_t b[200];
        if (cnt) swprintf(b, 200, L"Count %ld   Sum %.10g   Average %.6g   Min %.10g   Max %.10g", nonEmpty, sum, sum / cnt, mn, mx);
        else swprintf(b, 200, L"Count %ld", nonEmpty);
        stats = b;
    }
    // match the row header numbering (data rows from 1; the header row is "Header")
    std::wstring col = d->gHeader ? d->table.Value(d->text, 0, d->gc) : L"";
    if (col.empty()) col = ColName(d->gc);
    if (col.size() > 24) col = col.substr(0, 23) + L"\u2026";
    if (d->gHeader && d->gr == 0) return L"Header \u00B7 " + col;
    return L"Row " + std::to_wstring(d->gr - HeadOff(d) + 1) + L" \u00B7 " + col;
}

} // namespace app
