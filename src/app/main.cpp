// main.cpp - ModernNotepad shell: borderless Win11-style window (tabs in the title bar,
// Snap Layouts on the maximize button), menu bar, floating find/replace card, status bar,
// settings page, prompt overlay, file I/O, printing, accelerators.
#include "app.h"
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <imm.h>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <thread>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace app {

HWND g_hwnd;
HINSTANCE g_inst;
float g_scale = 1.f;
Theme g_th;
Settings g_set;
std::vector<std::unique_ptr<Doc>> g_docs;
int g_cur = -1;
ID2D1Factory* g_d2d;
IDWriteFactory* g_dw;
ID2D1HwndRenderTarget* g_rt;

// ------------------------------------------------------------------ ui state
static float CAP_H = 40, MENU_H = 36, STATUS_H = 26;
static const float CAPBTN_W = 46;
static void UpdateMetrics() { CAP_H = std::round(40 * std::min(g_textScale, 1.4f)); MENU_H = std::round(36 * g_textScale); STATUS_H = std::round(26 * g_textScale); }
enum Hot { HT_NONE, HT_TAB, HT_TABCLOSE, HT_NEWTAB, HT_MIN, HT_MAX, HT_CLOSE, HT_MENU, HT_TOOL, HT_STATUS, HT_FIND, HT_SETTINGS, HT_SPLIT, HT_FMT };
struct HitItem { RectF r; int kind; int idx; };
static std::vector<HitItem> s_hits;
static int s_hotKind = 0, s_hotIdx = -1, s_pressKind = 0, s_pressIdx = -1;
static bool s_settingsOpen = false;
static float s_settingsScroll = 0, s_settingsH = 0;
static std::wstring s_status; static ULONGLONG s_statusUntil = 0;
static bool s_sessionOwner = false, s_noSession = false;
static int s_dragTab = -1; static float s_dragTabX = 0;
static bool s_splitDrag = false;
static RectF s_editorArea, s_mdArea, s_aiArea, s_splitR, s_contentR;
static HFONT s_editFont = nullptr; static HBRUSH s_editBrush = nullptr;
HFONT UiEditFont() { return s_editFont; }
#undef SetCursor
static HCURSOR s_appCursor = LoadCursor(nullptr, IDC_ARROW);
HCURSOR AppSetCursor(HCURSOR c) {   // only touch the system cursor when it actually changes
    if (c == s_appCursor && ::GetCursor() == c) return c;
    s_appCursor = c; return ::SetCursor(c);
}
HCURSOR AppCursor() { return s_appCursor; }   // may be null (game hides it)
static void SetCursorNative(HCURSOR c) {
#undef SetCursor
    ::SetCursor(c);
#define SetCursor AppSetCursor
}
#define SetCursor AppSetCursor
// find
static bool s_findOpen = false, s_replaceOpen = false, s_optCase = false, s_optWord = false, s_optRegex = false;
static HWND s_findEdit = nullptr, s_replEdit = nullptr;
static RectF s_findCard, s_findBox, s_replBox;
static std::wstring s_findMsg; static uint64_t s_findVer = UINT64_MAX; static Doc* s_findDoc = nullptr; static std::wstring s_findText;
// prompt
struct PromptState { bool on = false; std::wstring title; void (*done)(const std::wstring&) = nullptr; HWND edit = nullptr; RectF card, box, ok, cancel; };
static PromptState s_prompt;
static WNDPROC s_editProcOrig = nullptr;
// context menu state
static np::Pos s_ctxA, s_ctxB; static std::vector<std::wstring> s_ctxSug;

float EditorFontPx() { return g_set.fontSize * 96.f / 72.f * g_set.zoom / 100.f * g_textScale; }
void Invalidate() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }
void SetStatus(const std::wstring& s) { s_status = s; s_statusUntil = GetTickCount64() + 3500; Invalidate(); }

// ------------------------------------------------------------------ dark popup menus
void ApplyDarkMenus() {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int); using Flush = void(WINAPI*)(); using Allow = bool(WINAPI*)(HWND, bool);
    auto sm = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto fl = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    auto aw = (Allow)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    if (sm) sm(g_th.dark ? 2 : 3);
    if (aw && g_hwnd) aw(g_hwnd, g_th.dark);
    if (fl) fl();
    if (g_hwnd) {
        BOOL dk = g_th.dark;
        DwmSetWindowAttribute(g_hwnd, 20, &dk, sizeof(dk));
        if (s_editBrush) DeleteObject(s_editBrush);
        Color c = g_th.surface;
        s_editBrush = CreateSolidBrush(RGB((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255)));
        if (s_findEdit) InvalidateRect(s_findEdit, nullptr, TRUE);
        if (s_replEdit) InvalidateRect(s_replEdit, nullptr, TRUE);
    }
}

// ------------------------------------------------------------------ clipboard
std::wstring ClipboardGet() {
    std::wstring s;
    if (!OpenClipboard(g_hwnd)) return s;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) { if (auto p = (const wchar_t*)GlobalLock(h)) { s = p; GlobalUnlock(h); } }
    CloseClipboard();
    return s;
}
void ClipboardSet(const std::wstring& t) {
    std::wstring s;
    for (wchar_t c : t) { if (c == L'\n') s += L"\r\n"; else s += c; }
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

// ------------------------------------------------------------------ documents
void UpdateTitle() {
    Doc* d = Cur();
    std::wstring t = d ? d->Name() + L" - ModernNotepad" : L"ModernNotepad";
    SetWindowTextW(g_hwnd, t.c_str());
    Invalidate();
}
Doc* NewDoc() {
    static uint64_t nextId = 1;
    g_docs.push_back(std::make_unique<Doc>());
    g_docs.back()->id = nextId++;
    g_docs.back()->fmtView = g_set.formatted;
    g_cur = (int)g_docs.size() - 1;
    UpdateTitle();
    return g_docs.back().get();
}
void SelectTab(int i) {
    if (i < 0 || i >= (int)g_docs.size()) return;
    g_cur = i;
    s_settingsOpen = false;
    s_findVer = UINT64_MAX;
    EditorClearCache(); MdClearCache();
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 14, 0, 0);
    UpdateTitle();
}
static void AddRecent(const std::wstring& p) {
    auto& r = g_set.recent;
    r.erase(std::remove_if(r.begin(), r.end(), [&](const std::wstring& x) { return _wcsicmp(x.c_str(), p.c_str()) == 0; }), r.end());
    r.insert(r.begin(), p);
    if (r.size() > 10) r.resize(10);
}
static void DetectLang(Doc* d) {
    if (!d->langAuto) return;
    d->lang = np::LangFromPath(d->path);
    if (d->lang == np::L_TEXT && d->text.LineCount() > 0) d->lang = np::LangFromContent(d->text.Line(0));
    d->lex.Reset(); d->foldsVer = UINT64_MAX; d->rowsDirty = true;
}
static bool s_quietSave = false;
static bool DiskStamp(const std::wstring& path, uint64_t& time, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
    time = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return true;
}
// Window activated / tab switched: has the current file changed on disk? Unmodified tabs reload
// silently (like VS Code); tabs with unsaved edits ask first.
static bool s_checkingExt = false;
void CheckExternalChange() {
    Doc* d = Cur();
    if (s_checkingExt || !d || d->path.empty() || d->loading || d->diskSize == UINT64_MAX) return;
    uint64_t t, sz;
    if (!DiskStamp(d->path, t, sz)) return;
    if (t == d->diskTime && sz == d->diskSize) return;
    s_checkingExt = true;
    d->diskTime = t; d->diskSize = sz;   // ask once per change
    bool reload = !d->Modified();
    if (!reload) {
        std::wstring m = d->Name() + L" was changed by another program.\n\nReload it and lose your unsaved changes here?";
        reload = ThemedMessageBox(g_hwnd, m.c_str(), L"ModernNotepad", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    }
    if (reload) { np::Pos c = d->caret; int top = d->topRow; OpenPath(d->path, false, (int)d->text.enc); if (Cur()) { Cur()->restoreCaret = c; Cur()->restoreTop = top; Cur()->caret = Cur()->anchor = Cur()->text.Clamp(c); Cur()->topRow = top; } SetStatus(d->Name() + L" was reloaded (changed on disk)"); }
    s_checkingExt = false;
}
bool SaveDocQuiet(Doc* d) { s_quietSave = true; bool ok = SaveDoc(d, false); s_quietSave = false; return ok; }
static bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = L"Cannot open the file."; return false; }
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    if (sz.QuadPart > (1LL << 30)) { CloseHandle(h); err = L"The file is too large (over 1 GB)."; return false; }
    out.resize((size_t)sz.QuadPart);
    size_t done = 0;
    bool fail = false;
    while (done < out.size()) {
        DWORD r = 0;
        if (!ReadFile(h, out.data() + done, (DWORD)std::min<size_t>(out.size() - done, 1 << 26), &r, nullptr)) { fail = true; break; }
        if (!r) break;
        done += r;
    }
    CloseHandle(h);
    // a short read (error, file truncated underneath us, network drop) must never open as a complete document
    if (fail || done != out.size()) { out.clear(); err = L"The file could not be read completely (read error or it changed while loading)."; return false; }
    return true;
}
struct LoadResult {
    uint64_t docId = 0; std::wstring path, err; int forceEnc = -1; bool ok = false, binary = false, mixed = false;
    std::vector<uint8_t> bytes; np::TextDoc text; np::Enc enc = np::Enc::UTF8; np::Eol eol = np::Eol::CRLF; size_t fileSize = 0;
};
static void DoLoad(LoadResult* r) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(r->path, bytes, r->err)) return;
    r->fileSize = bytes.size();
    if (r->forceEnc >= 0) {
        std::wstring t = np::DecodeAs(bytes, (np::Enc)r->forceEnc);
        std::vector<uint8_t>().swap(bytes);
        r->text.SetText(std::move(t)); r->enc = (np::Enc)r->forceEnc;
    } else {
        np::Decoded dec = np::DecodeBytes(bytes);
        if (dec.binary) { r->binary = true; r->bytes = std::move(bytes); }
        else {
            std::vector<uint8_t>().swap(bytes);   // free the raw file before building lines
            r->text.SetText(std::move(dec.text)); r->enc = dec.enc; r->eol = dec.eol; r->mixed = dec.mixedEol;
        }
    }
    r->ok = true;
}
static void ApplyLoaded(Doc* d, LoadResult* r) {
    d->loading = false;
    if (!r->ok) { ThemedMessageBox(g_hwnd, (r->path + L"\n\n" + r->err).c_str(), L"ModernNotepad", MB_ICONWARNING); return; }
    d->hex = false; d->binaryOnly = false; d->forceDirty = false;
    if (r->binary) {
        d->binaryOnly = true; d->bytes = std::move(r->bytes);
        d->text.SetText(L""); HexEnter(d); d->hexSaved = 0;
    } else {
        d->text = std::move(r->text);
        d->text.enc = r->enc; d->text.eol = r->eol;
        if (r->mixed) SetStatus(L"Mixed line endings were normalized to " + std::wstring(np::EolName(r->eol)) + L" on save.");
    }
    d->text.MarkSaved();
    if (!DiskStamp(d->path, d->diskTime, d->diskSize)) d->diskSize = UINT64_MAX;
    d->large = r->fileSize > (48u << 20) || d->text.LineCount() > 1000000;
    d->grid = false;
    d->caret = d->anchor = d->text.Clamp(d->restoreCaret); d->topRow = std::max(0, d->restoreTop); d->scrollX = 0;
    d->restoreTop = -1; d->restoreCaret = {};
    d->langAuto = true; DetectLang(d);
    d->lex.Reset(); d->folds.clear(); d->foldsVer = UINT64_MAX; d->rowsDirty = true; d->anyFolded = false;
    if (d->large) SetStatus(L"Large file mode: syntax colouring, folding and spell check are off for this tab");
    else if (g_set.csvAsTable && !d->binaryOnly && d->lang == np::L_CSV) GridEnter(d);
    d->fmtView = g_set.formatted && d->lang == np::L_MD && !d->large && !d->binaryOnly;
    d->liveVer = UINT64_MAX;
    EditorClearCache(); MdClearCache();
    UpdateTitle();
}
void OnLoaded(LPARAM lp) {
    std::unique_ptr<LoadResult> r((LoadResult*)lp);
    for (auto& d : g_docs) if (d->id == r->docId) { ApplyLoaded(d.get(), r.get()); if (Cur() == d.get()) EditorEnsureCaret(d.get()); Invalidate(); return; }
}
bool OpenPath(const std::wstring& pathIn, bool newTab, int forceEnc) {
    wchar_t full[MAX_PATH * 4];
    GetFullPathNameW(pathIn.c_str(), MAX_PATH * 4, full, nullptr);
    std::wstring path = full;
    if (forceEnc < 0)
        for (int i = 0; i < (int)g_docs.size(); ++i) if (_wcsicmp(g_docs[i]->path.c_str(), path.c_str()) == 0) { SelectTab(i); return true; }
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) { ThemedMessageBox(g_hwnd, (path + L"\n\nCannot open the file.").c_str(), L"ModernNotepad", MB_ICONWARNING); return false; }
    unsigned long long size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (size > (1ull << 30)) { ThemedMessageBox(g_hwnd, (path + L"\n\nThe file is too large (over 1 GB).").c_str(), L"ModernNotepad", MB_ICONWARNING); return false; }
    Doc* d = Cur();
    bool reuse = forceEnc >= 0 || (d && d->path.empty() && !d->Modified() && d->text.Length() == 0 && !d->binaryOnly && !d->loading);
    (void)newTab;
    if (!reuse) d = NewDoc();
    d->path = path; d->mdPreview = false;
    auto* r = new LoadResult; r->docId = d->id; r->path = path; r->forceEnc = forceEnc;
    AddRecent(path);
    if (size > (8u << 20)) {   // big file: decode + split lines on a worker, keep the UI responsive
        d->loading = true; d->loadMB = size / 1048576.0;
        d->text.SetText(L"");
        std::thread([r] { DoLoad(r); PostMessageW(g_hwnd, WM_APP_LOADED, 0, (LPARAM)r); }).detach();
        SelectTab(g_cur);
        return true;
    }
    DoLoad(r);
    bool ok = r->ok;
    ApplyLoaded(d, r);
    delete r;
    SelectTab(g_cur);
    return ok;
}

static bool WriteAllChecked(HANDLE h, const std::vector<uint8_t>& b) {
    size_t done = 0;
    while (done < b.size()) {
        DWORD w = 0, want = (DWORD)std::min<size_t>(b.size() - done, 1 << 24);
        if (!WriteFile(h, b.data() + done, want, &w, nullptr) || w == 0) return false;
        done += w;
    }
    return FlushFileBuffers(h) != FALSE;
}
// Safe save: write a temp file next to the target, verify every byte, then swap it in with ReplaceFileW
// (keeps the original's ACLs/attributes/streams). If the folder does not allow creating files but the file
// itself is writable, the original is first copied to the backup folder, then rewritten in place WITHOUT
// truncation; any failure restores the copy. There is no path that truncates the original before the new
// bytes are known to be on disk.
static bool WriteFileAtomic(const std::wstring& path, const std::vector<uint8_t>& b, std::wstring& err) {
    std::wstring tmp = path + L".mnp~";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!exists) { err = L"Cannot create the file in this folder (access denied)."; return false; }
        wchar_t tdir[MAX_PATH], copy[MAX_PATH];
        GetTempPathW(MAX_PATH, tdir);
        if (!GetTempFileNameW(tdir, L"mnp", 0, copy) || !CopyFileW(path.c_str(), copy, FALSE)) {
            err = L"This folder does not allow a temporary file and a safety copy of the original could not be made. Nothing was changed.";
            return false;
        }
        h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);   // no truncation yet
        bool ok = h != INVALID_HANDLE_VALUE && WriteAllChecked(h, b) && SetEndOfFile(h);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!ok) {
            bool restored = CopyFileW(copy, path.c_str(), FALSE) != FALSE;
            err = restored ? L"Write failed; the original file was restored." : (L"Write failed and the original could not be put back. A copy is at:\n" + std::wstring(copy));
            if (restored) DeleteFileW(copy);
            return false;
        }
        DeleteFileW(copy);
        return true;
    }
    BOOL ok = WriteAllChecked(h, b);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); err = L"Write failed (disk full?)."; return false; }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (ReplaceFileW(path.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) return true;
    }
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(tmp.c_str());
    err = L"Could not replace the file.";
    return false;
}

static std::wstring SaveDialog(Doc* d) {
    wchar_t file[MAX_PATH * 4] = {};
    std::wstring name = d->Name();
    if (d->path.empty()) {
        for (auto& c : name) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
        bool md = d->lang == np::L_MD;
        if (!md && d->fmtView && d->lang == np::L_TEXT) {   // used formatting -> Markdown, like Win11 Notepad
            LiveSync(d);
            for (auto& l : d->live) if (l.kind != np::LiveKind::Para && l.kind != np::LiveKind::Blank) { md = true; break; }
            if (!md) for (int i = 0; i < d->text.LineCount() && !md; ++i) { std::vector<np::LiveSpan> sp; np::LiveSpans(d->text.Line(i), d->live[i], sp); md = !sp.empty(); }
        }
        const wchar_t* ext = md ? L".md" : d->lang == np::L_HTML ? L".html" : d->lang == np::L_BBCODE ? L".bbcode" : L".txt";
        name += ext;
    }
    wcsncpy_s(file, name.c_str(), _TRUNCATE);
    OPENFILENAMEW of{ sizeof(of) };
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"Text documents (*.txt)\0*.txt\0Markdown (*.md)\0*.md;*.markdown\0HTML (*.html)\0*.html;*.htm\0BBCode (*.bbcode)\0*.bbcode;*.bb\0"
                     L"CSV / TSV (*.csv;*.tsv)\0*.csv;*.tsv;*.tab\0All files (*.*)\0*.*\0";
    { std::wstring nm = file; size_t dot = nm.find_last_of(L'.'); std::wstring e = dot == std::wstring::npos ? L"" : nm.substr(dot);
      of.nFilterIndex = e == L".md" ? 2 : (e == L".html" || e == L".htm") ? 3 : e == L".bbcode" ? 4 : (e == L".csv" || e == L".tsv") ? 5 : e == L".txt" ? 1 : 6; }
    of.lpstrFile = file; of.nMaxFile = MAX_PATH * 4;
    of.lpstrDefExt = L"txt";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ENABLESIZING;
    if (!GetSaveFileNameW(&of)) return L"";
    return file;
}

bool SaveDoc(Doc* d, bool saveAs, int forceEnc) {
    if (!d) return false;
    std::wstring path = d->path;
    if (saveAs || path.empty()) { path = SaveDialog(d); if (path.empty()) return false; }
    if (forceEnc >= 0) d->text.enc = (np::Enc)forceEnc;
    // another program changed the file since we loaded/saved it?
    if (!saveAs && path == d->path && d->diskSize != UINT64_MAX) {
        uint64_t t, sz;
        if (DiskStamp(path, t, sz) && (t != d->diskTime || sz != d->diskSize)) {
            if (s_quietSave) { SetStatus(L"Auto-save skipped: " + d->Name() + L" was changed by another program"); return false; }
            std::wstring m = d->Name() + L" was changed by another program since it was opened.\n\nOverwrite those changes with this version?";
            if (ThemedMessageBox(g_hwnd, m.c_str(), L"ModernNotepad", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return false;
        }
    }
    std::vector<uint8_t> bytes;
    if (d->binaryOnly || d->hex) bytes = d->bytes;
    else {
        std::wstring txt = d->text.Text();
        if (d->text.enc == np::Enc::ANSI && !np::AnsiCanEncode(txt)) {
            if (s_quietSave) { SetStatus(L"Auto-save skipped: " + d->Name() + L" has characters ANSI cannot store"); return false; }
            int r = ThemedMessageBox(g_hwnd, (d->Name() + L" contains characters that the ANSI code page cannot represent; they would be replaced by '?'.\n\nSave as UTF-8 instead?").c_str(),
                                     L"ModernNotepad", MB_YESNOCANCEL | MB_ICONWARNING);
            if (r == IDCANCEL) return false;
            if (r == IDYES) d->text.enc = np::Enc::UTF8;
        }
        bytes = np::EncodeText(txt, d->text.enc, d->text.eol);
    }
    std::wstring err;
    if (!WriteFileAtomic(path, bytes, err)) {
        if (s_quietSave) SetStatus(L"Auto-save failed: " + err);
        else ThemedMessageBox(g_hwnd, (L"Could not save " + path + L"\n\n" + err).c_str(), L"ModernNotepad", MB_ICONERROR);
        return false;
    }
    if (d->hex && !d->binaryOnly) { HexLeave(d); HexEnter(d); }   // only after the bytes are safely on disk
    DiskStamp(path, d->diskTime, d->diskSize);
    bool pathChanged = path != d->path;
    d->path = path;
    d->text.MarkSaved(); d->forceDirty = false; d->hexSaved = d->hexUndo.size();
    d->savedDiskVer = d->text.Version();
    SessionDeleteBackup(d);
    if (pathChanged) { DetectLang(d); EditorClearCache(); }
    AddRecent(path);
    SetStatus(L"Saved " + d->Name());
    UpdateTitle();
    return true;
}

static bool ConfirmClose(Doc* d) {
    if (!d->Modified()) return true;
    std::wstring m = L"Do you want to save changes to " + d->Name() + L"?";
    int r = ThemedMessageBox(g_hwnd, m.c_str(), L"ModernNotepad", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return SaveDoc(d, false);
    return true;
}
static void CloseTab(int i, bool ask = true) {
    if (i < 0 || i >= (int)g_docs.size()) return;
    Doc* d = g_docs[i].get();
    if (ask && !ConfirmClose(d)) return;
    SessionDeleteBackup(d);
    g_docs.erase(g_docs.begin() + i);
    if (g_docs.empty()) { NewDoc(); }
    if (g_cur >= (int)g_docs.size()) g_cur = (int)g_docs.size() - 1;
    else if (i < g_cur) --g_cur;
    SelectTab(g_cur);
}

static void OpenDialog() {
    std::vector<wchar_t> buf(32768, 0);
    OPENFILENAMEW of{ sizeof(of) };
    of.hwndOwner = g_hwnd;
    of.lpstrFilter =
        L"All supported files\0*.txt;*.md;*.markdown;*.html;*.htm;*.xml;*.json;*.ini;*.cfg;*.conf;*.yaml;*.yml;*.csv;*.tsv;*.bbcode;*.log;"
        L"*.c;*.h;*.cpp;*.hpp;*.cs;*.java;*.js;*.ts;*.rs;*.go;*.py;*.css;*.sh;*.ps1;*.bat;*.cmd;*.sql;*.lua;*.asm\0"
        L"Text documents (*.txt;*.log)\0*.txt;*.log\0Markdown (*.md)\0*.md;*.markdown\0HTML / XML (*.html;*.xml)\0*.html;*.htm;*.xml;*.xaml;*.svg\0"
        L"JSON (*.json)\0*.json\0INI / config (*.ini;*.cfg;*.conf)\0*.ini;*.cfg;*.conf;*.inf\0YAML (*.yaml;*.yml)\0*.yaml;*.yml\0"
        L"CSV / TSV (*.csv;*.tsv)\0*.csv;*.tsv;*.tab\0BBCode (*.bbcode)\0*.bbcode;*.bb\0"
        L"Source code\0*.c;*.h;*.cpp;*.hpp;*.cc;*.cs;*.java;*.js;*.ts;*.rs;*.go;*.py;*.css;*.sh;*.ps1;*.bat;*.cmd;*.sql;*.lua;*.asm\0"
        L"All files (*.*)\0*.*\0";
    of.nFilterIndex = 1;
    of.lpstrFile = buf.data(); of.nMaxFile = (DWORD)buf.size();
    of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_ENABLESIZING;
    if (!GetOpenFileNameW(&of)) return;
    std::wstring dir = buf.data();
    const wchar_t* p = buf.data() + dir.size() + 1;
    if (!*p) { OpenPath(dir); return; }
    while (*p) { OpenPath(dir + L"\\" + p); p += wcslen(p) + 1; }
}

// ------------------------------------------------------------------ printing (GDI)
static void PrintDoc(Doc* d) {
    PRINTDLGW pd{ sizeof(pd) };
    pd.hwndOwner = g_hwnd;
    pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS | PD_USEDEVMODECOPIESANDCOLLATE;
    if (!PrintDlgW(&pd)) return;
    HDC dc = pd.hDC;
    int dpiY = GetDeviceCaps(dc, LOGPIXELSY), dpiX = GetDeviceCaps(dc, LOGPIXELSX);
    int pw = GetDeviceCaps(dc, HORZRES), ph = GetDeviceCaps(dc, VERTRES);
    int mx = dpiX * 3 / 4, my = dpiY * 3 / 4;
    HFONT f = CreateFontW(-MulDiv((int)g_set.fontSize, dpiY, 72), 0, 0, 0, g_set.fontBold ? FW_BOLD : FW_NORMAL, g_set.fontItalic, 0, 0,
                          DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_set.fontFace.c_str());
    HFONT hf = CreateFontW(-MulDiv(9, dpiY, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    DOCINFOW di{ sizeof(di) }; std::wstring name = d->Name(); di.lpszDocName = name.c_str();
    if (StartDocW(dc, &di) > 0) {
        int page = 0, line = 0, n = d->text.LineCount();
        DRAWTEXTPARAMS dtp{ sizeof(dtp), g_set.tabSize, 0, 0, 0 };
        while (line < n) {
            StartPage(dc); ++page;
            SelectObject(dc, hf);
            RECT hr{ mx, my / 2, pw - mx, my };
            DrawTextW(dc, name.c_str(), -1, &hr, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            std::wstring pn = L"Page " + std::to_wstring(page);
            RECT fr{ mx, ph - my, pw - mx, ph - my / 2 };
            DrawTextW(dc, pn.c_str(), -1, &fr, DT_CENTER | DT_SINGLELINE);
            SelectObject(dc, f);
            int y = my + dpiY / 8;
            while (line < n) {
                std::wstring s = d->text.Line(line); if (s.empty()) s = L" ";
                RECT r{ mx, y, pw - mx, ph - my };
                DrawTextExW(dc, &s[0], (int)s.size(), &r, DT_CALCRECT | DT_WORDBREAK | DT_EXPANDTABS | DT_TABSTOP | DT_NOPREFIX, &dtp);
                int h = r.bottom - r.top;
                if (y + h > ph - my && y > my + dpiY / 8) break;
                r = { mx, y, pw - mx, y + h };
                DrawTextExW(dc, &s[0], (int)s.size(), &r, DT_WORDBREAK | DT_EXPANDTABS | DT_TABSTOP | DT_NOPREFIX, &dtp);
                y += h; ++line;
            }
            EndPage(dc);
        }
        EndDoc(dc);
    }
    DeleteObject(f); DeleteObject(hf); DeleteDC(dc);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
}

// ------------------------------------------------------------------ prompt overlay
static LRESULT CALLBACK EditSubProc(HWND h, UINT m, WPARAM wp, LPARAM lp);
static void PromptClose(bool accept) {
    if (!s_prompt.on) return;
    std::wstring v;
    if (accept) { int n = GetWindowTextLengthW(s_prompt.edit); v.resize(n + 1); GetWindowTextW(s_prompt.edit, &v[0], n + 1); v.resize(n); }
    DestroyWindow(s_prompt.edit);
    auto done = s_prompt.done;
    s_prompt = PromptState{};
    SetFocus(g_hwnd);
    if (accept && done) done(v);
    Invalidate();
}
void Prompt(const std::wstring& title, const std::wstring& initial, void (*done)(const std::wstring&), bool password) {
    if (s_prompt.on) PromptClose(false);
    s_prompt.on = true; s_prompt.title = title; s_prompt.done = done;
    s_prompt.edit = CreateWindowExW(0, L"EDIT", initial.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0),
                                    0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)9001, g_inst, nullptr);
    SendMessageW(s_prompt.edit, WM_SETFONT, (WPARAM)s_editFont, TRUE);
    s_editProcOrig = (WNDPROC)SetWindowLongPtrW(s_prompt.edit, GWLP_WNDPROC, (LONG_PTR)EditSubProc);
    SendMessageW(s_prompt.edit, EM_SETSEL, 0, -1);
    SetFocus(s_prompt.edit);
    Invalidate();
}


// ------------------------------------------------------------------ two-field form (Link / Image dialogs, Win11 Notepad style)
struct FormState {
    bool on = false; std::wstring title, okText;
    struct Field { std::wstring label, cue; HWND edit = nullptr; RectF box; } f[2];
    bool browse = false; RectF card, ok, cancel, brw;
    void (*done)(const std::wstring&, const std::wstring&) = nullptr;
};
static FormState s_form;
static WNDPROC s_formOrig = nullptr;
static std::wstring FormText(int i) { HWND e = s_form.f[i].edit; int n = GetWindowTextLengthW(e); std::wstring v(n + 1, 0); GetWindowTextW(e, &v[0], n + 1); v.resize(n); return v; }
static void FormClose(bool accept) {
    if (!s_form.on) return;
    std::wstring a = FormText(0), b = FormText(1);
    if (accept && b.find_first_not_of(L" \t") == std::wstring::npos) return;   // address required
    for (auto& f : s_form.f) DestroyWindow(f.edit);
    auto done = s_form.done;
    s_form = FormState{};
    SetFocus(g_hwnd);
    if (accept && done) done(a, b);
    Invalidate();
}
static std::wstring FileUrl(const std::wstring& path) {
    std::string u8; int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0, nullptr, nullptr);
    u8.resize(n); WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(), &u8[0], n, nullptr, nullptr);
    std::wstring o = L"file:///";
    for (unsigned char c : u8) {
        if (c == '\\') o += L'/';
        else if (isalnum(c) || strchr("/:-_.~!$&'()*+,;=@", c)) o += (wchar_t)c;
        else { wchar_t b[4]; swprintf(b, 4, L"%%%02X", c); o += b; }
    }
    return o;
}
static void FormBrowse() {
    wchar_t file[MAX_PATH * 4] = {};
    OPENFILENAMEW of{ sizeof(of) };
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.ico;*.tif)\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.ico;*.tif;*.tiff\0All files (*.*)\0*.*\0";
    of.lpstrFile = file; of.nMaxFile = MAX_PATH * 4;
    of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ENABLESIZING;
    if (!GetOpenFileNameW(&of)) return;
    std::wstring pth = file;
    SetWindowTextW(s_form.f[1].edit, FileUrl(pth).c_str());
    if (GetWindowTextLengthW(s_form.f[0].edit) == 0) {
        size_t sl = pth.find_last_of(L"\\/"), dot = pth.find_last_of(L'.');
        std::wstring stem = pth.substr(sl + 1, dot != std::wstring::npos && dot > sl ? dot - sl - 1 : std::wstring::npos);
        SetWindowTextW(s_form.f[0].edit, stem.c_str());
    }
    SetFocus(s_form.f[1].edit); Invalidate();
}
static LRESULT CALLBACK FormProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN) {
        if (wp == VK_RETURN) { FormClose(true); return 0; }
        if (wp == VK_ESCAPE) { FormClose(false); return 0; }
        if (wp == VK_TAB) { SetFocus(h == s_form.f[0].edit ? s_form.f[1].edit : s_form.f[0].edit); SendMessageW(GetFocus(), EM_SETSEL, 0, -1); Invalidate(); return 0; }
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    }
    if (m == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB || wp == 1)) return 0;
    LRESULT r = CallWindowProcW(s_formOrig, h, m, wp, lp);
    if (m == WM_CHAR || m == WM_SETFOCUS || m == WM_KILLFOCUS || m == WM_PASTE || m == WM_CUT || m == WM_KEYUP) Invalidate();
    return r;
}
static void Form(const std::wstring& title, const std::wstring& okText, const std::wstring& l0, const std::wstring& v0, const std::wstring& c0,
                 const std::wstring& l1, const std::wstring& v1, const std::wstring& c1, bool browse, void (*done)(const std::wstring&, const std::wstring&), int focus) {
    if (s_form.on) FormClose(false);
    s_form.on = true; s_form.title = title; s_form.okText = okText; s_form.browse = browse; s_form.done = done;
    const std::wstring* lab[2] = { &l0, &l1 }; const std::wstring* val[2] = { &v0, &v1 }; const std::wstring* cue[2] = { &c0, &c1 };
    for (int i = 0; i < 2; ++i) {
        s_form.f[i].label = *lab[i]; s_form.f[i].cue = *cue[i];
        s_form.f[i].edit = CreateWindowExW(0, L"EDIT", val[i]->c_str(), WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)(9020 + i), g_inst, nullptr);
        SendMessageW(s_form.f[i].edit, WM_SETFONT, (WPARAM)s_editFont, TRUE);
        SendMessageW(s_form.f[i].edit, 0x1501 /*EM_SETCUEBANNER*/, TRUE, (LPARAM)cue[i]->c_str());
        s_formOrig = (WNDPROC)SetWindowLongPtrW(s_form.f[i].edit, GWLP_WNDPROC, (LONG_PTR)FormProc);
    }
    SetFocus(s_form.f[focus].edit); SendMessageW(s_form.f[focus].edit, EM_SETSEL, 0, -1);
    Invalidate();
}
static void PaintForm(float W, float H) {
    if (!s_form.on) return;
    FillR(RF(0, 0, W, H), Hex(0x000000, 0.35f));
    float ts = std::min(g_textScale, 1.4f);
    float cw = std::min(400.f * ts, W - 40), ch = 290 * ts;
    s_form.card = RF(std::round((W - cw) / 2), std::round((H - ch) / 2 - 30), cw, ch);
    RectF c = s_form.card;
    FillRR(c, 8, g_th.surface); StrokeRR(c, 8, g_th.border);
    float x = c.left + 24, y = c.top + 18, fw = cw - 48;
    Text(s_form.title, RF(x, y, fw, 30 * ts), UiFmt(19, true), g_th.text);
    y += 44 * ts;
    for (int i = 0; i < 2; ++i) {
        Text(s_form.f[i].label, RF(x, y, fw, 20 * ts), UiFmt(13), g_th.text);
        y += 24 * ts;
        float bw = fw - (i == 1 && s_form.browse ? 92 * ts : 0);
        RectF b = RF(x, y, bw, 32 * ts); s_form.f[i].box = b;
        bool f = GetFocus() == s_form.f[i].edit;
        FillRR(b, 4, f ? g_th.editor : Alpha(g_th.editor, 0.7f)); StrokeRR(b, 4, g_th.border);
        if (f) FillRR(RF(b.left + 1, b.bottom - 2, RW(b) - 2, 2), 1, g_th.accent);
        float eh = 18;
        SetWindowPos(s_form.f[i].edit, HWND_TOP, (int)((b.left + 9) * g_scale), (int)((b.top + (RH(b) - eh) / 2) * g_scale),
                     (int)((RW(b) - 18) * g_scale), (int)(eh * g_scale), SWP_SHOWWINDOW | SWP_NOACTIVATE);
        if (i == 1 && s_form.browse) {
            s_form.brw = RF(b.right + 8 * ts, y, 84 * ts, 32 * ts);
            FillRR(s_form.brw, 4, g_th.editor); StrokeRR(s_form.brw, 4, g_th.border);
            Text(L"Browse…", s_form.brw, UiFmt(13), g_th.text, 1);
        }
        y += 44 * ts;
    }
    // footer band like the Win11 dialog
    RectF foot = RF(c.left + 1, c.bottom - 72 * ts, cw - 2, 72 * ts - 1);
    g_rt->PushAxisAlignedClip(foot, D2D1_ANTIALIAS_MODE_ALIASED);
    FillRR(RF(foot.left, foot.top - 10, RW(foot), RH(foot) + 10), 7, g_th.chrome);
    g_rt->PopAxisAlignedClip();
    float bw = (fw - 8) / 2;
    s_form.ok = RF(x, foot.top + 20 * ts, bw, 32 * ts);
    s_form.cancel = RF(x + bw + 8, foot.top + 20 * ts, bw, 32 * ts);
    bool can = FormText(1).find_first_not_of(L" \t") != std::wstring::npos;
    FillRR(s_form.ok, 4, can ? g_th.accent : Alpha(g_th.border, 0.9f));
    Text(s_form.okText, s_form.ok, UiFmt(13), can ? g_th.accentText : g_th.dim, 1);
    FillRR(s_form.cancel, 4, g_th.editor); StrokeRR(s_form.cancel, 4, g_th.border); Text(L"Cancel", s_form.cancel, UiFmt(13), g_th.text, 1);
}
static std::wstring NormalizeAddress(std::wstring u) {
    while (!u.empty() && (u.back() == L' ' || u.back() == L'"')) u.pop_back();
    while (!u.empty() && (u[0] == L' ' || u[0] == L'"')) u.erase(0, 1);
    if (u.size() > 2 && ((iswalpha(u[0]) && u[1] == L':' && (u[2] == L'\\' || u[2] == L'/')) || (u[0] == L'\\' && u[1] == L'\\'))) return FileUrl(u);
    std::wstring host = u.substr(0, u.find(L'/'));
    if (u.find(L"://") == std::wstring::npos && host.find(L'.') != std::wstring::npos && u.find(L' ') == std::wstring::npos &&
        u.find(L'\\') == std::wstring::npos && u.rfind(L"mailto:", 0) != 0 && host[0] != L'.') {
        // bare host like example.org/docs -> https; a relative file (pic.png, ./img/a.png) stays relative
        size_t dot = host.find_last_of(L'.'); std::wstring ext = host.substr(dot + 1);
        static const wchar_t* fileExt[] = { L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"md", L"txt", L"html", L"htm", L"pdf", L"svg" };
        bool isFile = false; for (auto e : fileExt) if (_wcsicmp(ext.c_str(), e) == 0) isFile = true;
        if (!isFile) return L"https://" + u;
    }
    return u;
}
static void LinkDone(const std::wstring& text, const std::wstring& addr) {
    Doc* d = Cur(); if (!d || d->hex || d->loading) return;
    std::wstring url = NormalizeAddress(addr);
    std::wstring shown = text.empty() ? url : text;
    np::Dialect dl = DocDialect(d);
    std::wstring ins;
    if (dl == np::Dialect::Markdown) {
        std::wstring u = url; if (u.find(L' ') != std::wstring::npos || u.find(L')') != std::wstring::npos) u = L"<" + u + L">";
        ins = text.empty() && url.find(L"://") != std::wstring::npos ? L"<" + url + L">" : L"[" + shown + L"](" + u + L")";
    } else ins = np::WrapInline(dl, np::Inline::Link, shown, url).text;
    d->text.BreakCoalesce();
    EditorInsert(d, ins);
}
static void LinkDialog() {
    Doc* d = Cur(); if (!d || d->hex || d->loading || d->grid) return;
    std::wstring sel = EditorHasSel(d) && d->caret.line == d->anchor.line ? EditorSelection(d) : L"";
    std::wstring addr;
    if (sel.rfind(L"http", 0) == 0 || sel.rfind(L"www.", 0) == 0) { addr = sel; sel.clear(); }
    Form(L"Link", L"Insert", L"Display text", sel, L"Text for the link (optional)", L"Address", addr, L"Link to an existing webpage or file", false, LinkDone, sel.empty() && addr.empty() ? 0 : 1);
}
static void ImageDone(const std::wstring& alt, const std::wstring& addr) {
    Doc* d = Cur(); if (!d || d->hex || d->loading) return;
    std::wstring url = NormalizeAddress(addr);
    if (DocDialect(d) == np::Dialect::Markdown && (url.find(L' ') != std::wstring::npos || url.find(L')') != std::wstring::npos)) url = L"<" + url + L">";
    std::wstring s = np::MakeImage(DocDialect(d), url, alt.empty() ? L"image" : alt);
    d->text.BreakCoalesce();
    EditorInsert(d, s);
}
static void ImageDialog() {
    Doc* d = Cur(); if (!d || d->hex || d->loading || d->grid) return;
    Form(L"Image", L"Insert", L"Description", L"", L"Alternative text (optional)", L"Address", L"", L"File path, file:/// or https:// address", true, ImageDone, 1);
}

// ------------------------------------------------------------------ find
static std::wstring EditText(HWND e) { int n = GetWindowTextLengthW(e); std::wstring s(n + 1, 0); GetWindowTextW(e, &s[0], n + 1); s.resize(n); return s; }
static void FindRefresh(bool force = false) {
    Doc* d = Cur();
    if (!d || !s_findOpen) { g_findActive = false; return; }
    std::wstring t = EditText(s_findEdit);
    static uint64_t verSeen = UINT64_MAX; static ULONGLONG verSeenAt = 0;
    if (d->text.Version() != verSeen) { verSeen = d->text.Version(); verSeenAt = GetTickCount64(); }
    if (!force && d->text.Length() > (4u << 20) && t == s_findText && s_findDoc == d && GetTickCount64() - verSeenAt < 400) return;   // debounce on big docs
    if (!force && t == s_findText && s_findDoc == d && s_findVer == d->text.Version() && g_searcher.Opts().matchCase == s_optCase &&
        g_searcher.Opts().wholeWord == s_optWord && g_searcher.Opts().regex == s_optRegex) return;
    s_findText = t; s_findDoc = d; s_findVer = d->text.Version();
    np::SearchOpts o; o.pattern = t; o.matchCase = s_optCase; o.wholeWord = s_optWord; o.regex = s_optRegex;
    std::wstring err;
    g_findMatches.clear(); s_findMsg.clear();
    g_findActive = !t.empty() && !d->hex;
    if (!g_findActive) return;
    if (!g_searcher.Prepare(o, &err)) {
        s_findMsg = err.empty() ? L"Invalid pattern" : L"Invalid pattern";
        if (!err.empty()) SetStatus(err);
        g_findActive = false; return;
    }
    g_searcher.budgetMs = 700;   // typing in the find box must stay responsive
    g_searcher.FindAll(d->text, g_findMatches);
    if (g_searcher.TimedOut()) { s_findMsg = std::to_wstring(g_findMatches.size()) + L"+ (stopped)"; SetStatus(L"Regex highlighting stopped after 0.7 s on this document; Find next still works."); }
    g_searcher.budgetMs = 3000;
}
static void FindNext(bool forward) {
    Doc* d = Cur();
    if (!d || d->hex) return;
    FindRefresh();
    if (!g_searcher.Ready() || s_findText.empty()) return;
    size_t a = d->text.Offset(std::min(d->caret, d->anchor)), b = d->text.Offset(std::max(d->caret, d->anchor));
    np::Match m;
    if (g_searcher.Find(d->text, forward ? b : a, forward, m)) {
        np::Pos pa = d->text.PosAt(m.off), pb = d->text.PosAt(m.off + m.len);
        EditorUnfoldFor(d, pa.line);
        EditorSelect(d, pa, pb);
    } else if (g_searcher.TimedOut()) { SetStatus(L"Regex search stopped after 3 s without a match"); MessageBeep(MB_OK); }
    else { SetStatus(L"Can't find \"" + s_findText + L"\""); MessageBeep(MB_OK); }
}
static void ReplaceOne() {
    Doc* d = Cur();
    if (!d || d->hex) return;
    FindRefresh();
    if (!g_searcher.Ready()) return;
    size_t a = d->text.Offset(std::min(d->caret, d->anchor)), b = d->text.Offset(std::max(d->caret, d->anchor));
    for (auto& m : g_findMatches) if (m.off == a && m.off + m.len == b) {
        std::wstring r = g_searcher.Expand(d->text, m, EditText(s_replEdit));
        d->text.BreakCoalesce();
        EditorInsert(d, r);
        break;
    }
    FindNext(true);
}
static void ReplaceAll() {
    Doc* d = Cur();
    if (!d || d->hex) return;
    FindRefresh(true);
    if (!g_searcher.Ready()) return;
    np::Pos c = d->caret;
    int n = g_searcher.ReplaceAll(d->text, EditText(s_replEdit), c);
    if (n < 0) { SetStatus(L"Replace all stopped: the regex took too long on this document. Nothing was changed."); MessageBeep(MB_ICONWARNING); return; }
    d->caret = d->anchor = d->text.Clamp(c);
    EditorAfterEdit(d);
    SetStatus(std::to_wstring(n) + L" replacement" + (n == 1 ? L"" : L"s"));
    FindRefresh(true);
}
static void OpenFind(bool replace) {
    Doc* d = Cur();
    s_findOpen = true; s_replaceOpen = replace;
    if (d && !d->hex && EditorHasSel(d) && d->caret.line == d->anchor.line) SetWindowTextW(s_findEdit, EditorSelection(d).c_str());
    SetFocus(s_findEdit);
    SendMessageW(s_findEdit, EM_SETSEL, 0, -1);
    FindRefresh(true);
    Invalidate();
}
static void CloseFind() { s_findOpen = false; g_findActive = false; g_findMatches.clear(); SetFocus(g_hwnd); Invalidate(); }

static LRESULT CALLBACK EditSubProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN) {
        bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
        if (h == s_prompt.edit) {
            if (wp == VK_RETURN) { PromptClose(true); return 0; }
            if (wp == VK_ESCAPE) { PromptClose(false); return 0; }
        } else {
            if (wp == VK_ESCAPE) { CloseFind(); return 0; }
            if (wp == VK_RETURN) { if (h == s_replEdit) { if (ctrl) ReplaceAll(); else ReplaceOne(); } else FindNext(!shift); return 0; }
            if (wp == VK_F3) { FindNext(!shift); return 0; }
            if (wp == VK_TAB) { SetFocus(h == s_findEdit && s_replaceOpen ? s_replEdit : s_findEdit); return 0; }
            if (ctrl && (wp == 'H')) { s_replaceOpen = true; SetFocus(s_replEdit); Invalidate(); return 0; }
            if (ctrl && wp == 'F') { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
            if (ctrl && wp == 'A') { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
            if (GetKeyState(VK_MENU) < 0) {
                if (wp == 'C') { s_optCase = !s_optCase; FindRefresh(true); Invalidate(); return 0; }
                if (wp == 'W') { s_optWord = !s_optWord; FindRefresh(true); Invalidate(); return 0; }
                if (wp == 'R') { s_optRegex = !s_optRegex; FindRefresh(true); Invalidate(); return 0; }
            }
        }
    }
    if (m == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB || wp == 1 /*ctrl+a*/)) return 0;
    if (m == WM_SYSKEYDOWN && h != s_prompt.edit && (wp == 'C' || wp == 'W' || wp == 'R')) { SendMessageW(h, WM_KEYDOWN, wp, lp); return 0; }
    if (m == WM_SYSCHAR) return 0;
    LRESULT r = CallWindowProcW(s_editProcOrig, h, m, wp, lp);
    if (m == WM_SETFOCUS || m == WM_KILLFOCUS) Invalidate();
    return r;
}

// ------------------------------------------------------------------ menus (model based)
np::Dialect DocDialect(Doc* d) {
    if (!d) return np::Dialect::Markdown;
    if (d->dialect >= 0) return (np::Dialect)d->dialect;
    if (d->lang == np::L_HTML || d->lang == np::L_XML) return np::Dialect::Html;
    if (d->lang == np::L_BBCODE) return np::Dialect::BBCode;
    return np::Dialect::Markdown;   // .md, .txt and untitled: Markdown (what Win11 Notepad's formatting writes)
}
bool PreviewSupported(Doc* d) { return d && !d->binaryOnly && !d->large && (d->lang == np::L_MD || d->lang == np::L_BBCODE || d->lang == np::L_HTML || d->lang == np::L_TEXT); }
std::wstring PreviewMarkdown(Doc* d) {
    std::wstring t = d->text.Text();
    switch (DocDialect(d)) {
    case np::Dialect::BBCode: return np::BBCodeToMarkdown(t);
    case np::Dialect::Html: return np::HtmlToMarkdown(t);
    default: return t;
    }
}
static std::vector<MenuItem> EncodingItems(Doc* d, bool reopen) {
    std::vector<MenuItem> v;
    for (int i = 0; i < 5; ++i) v.push_back(MI((reopen ? C_REOPEN0 : C_ENC0) + i, np::EncName((np::Enc)i), L"", I_NONE, !reopen && d && (int)d->text.enc == i, !reopen || (d && !d->path.empty())));
    return v;
}
static std::vector<MenuItem> EolItems(Doc* d) {
    return { MI(C_EOL_CRLF, L"Windows (CRLF)", L"", I_NONE, d && d->text.eol == np::Eol::CRLF), MI(C_EOL_LF, L"Unix (LF)", L"", I_NONE, d && d->text.eol == np::Eol::LF),
             MI(C_EOL_CR, L"Macintosh (CR)", L"", I_NONE, d && d->text.eol == np::Eol::CR) };
}
static std::vector<MenuItem> LangItems(Doc* d) {
    std::vector<MenuItem> v;
    for (int i = 0; i < np::L_COUNT; ++i) v.push_back(MI(C_LANG0 + i, np::LangName((np::Lang)i), L"", I_NONE, d && d->lang == i));
    return v;
}
static std::vector<MenuItem> HeadingItems() {
    std::vector<MenuItem> v;
    for (int i = 0; i < 6; ++i) v.push_back(MI(C_FMT_H1 + i, L"Heading " + std::to_wstring(i + 1), i < 3 ? L"Ctrl+" + std::to_wstring(i + 1) + L" (Alt)" : L"", I_HEADING));
    v.push_back(MI(C_FMT_PARA, L"Body text", L"", I_FONT));
    v[0].keys = v[1].keys = v[2].keys = L"";
    return v;
}
static std::vector<MenuItem> ListItems() {
    return { MI(C_FMT_BULLET, L"Bulleted list", L"", I_LIST), MI(C_FMT_NUMBER, L"Numbered list", L"", I_LISTNUM), MI(C_FMT_TASK, L"Checklist", L"", I_TASK) };
}
static std::vector<MenuItem> TableItems() {
    static const int dims[6][2] = { {2,2}, {3,2}, {3,3}, {4,3}, {4,4}, {5,5} };
    std::vector<MenuItem> v;
    for (int i = 0; i < 6; ++i) v.push_back(MI(C_INS_TABLE0 + i, std::to_wstring(dims[i][0]) + L" × " + std::to_wstring(dims[i][1]) + L" table", L"", I_TABLE));
    v.push_back(MSep()); v.push_back(MI(C_INS_TABLECUSTOM, L"Custom size…", L"", I_TABLE));
    return v;
}
static std::vector<MenuItem> InsertItems() {
    return { MSub(L"Table", I_TABLE, TableItems()),
             MSub(L"Chart", I_CHART, { MI(C_INS_CHART_BAR, L"Bar chart", L"", I_CHART), MI(C_INS_CHART_LINE, L"Line chart", L"", I_CHART), MI(C_INS_CHART_PIE, L"Pie chart", L"", I_CHART) }),
             MI(C_INS_IMAGE, L"Image…", L"", I_IMAGE), MI(C_FMT_LINK, L"Link…", L"Ctrl+K", I_LINK), MI(C_FMT_CODEBLOCK, L"Code block", L"", I_CODE),
             MI(C_INS_RULE, L"Horizontal rule", L"", I_HR), MI(C_TIMEDATE, L"Time/Date", L"F5", I_RECENT) };
}
static std::vector<MenuItem> FormatItems() {
    return { MI(C_FMT_BOLD, L"Bold", L"Ctrl+B", I_BOLD), MI(C_FMT_ITALIC, L"Italic", L"Ctrl+I", I_ITALIC), MI(C_FMT_UNDERLINE, L"Underline", L"", I_UNDERLINE),
             MI(C_FMT_STRIKE, L"Strikethrough", L"", I_STRIKE), MI(C_FMT_CODE, L"Inline code", L"Ctrl+E", I_CODE), MI(C_FMT_LINK, L"Link…", L"Ctrl+K", I_LINK), MSep(),
             MSub(L"Heading", I_HEADING, HeadingItems()), MSub(L"List", I_LIST, ListItems()), MI(C_FMT_QUOTE, L"Quote", L"", I_QUOTE), MSep(),
             MI(C_FMT_CLEAR, L"Clear formatting", L"Ctrl+Space", I_CLEARFMT) };
}
static std::vector<MenuItem> DialectItems(Doc* d) {
    int cur = d ? d->dialect : -1;
    std::wstring autoName = L"Automatic (" + std::wstring(np::DialectName(DocDialect(d))) + L")";
    if (cur >= 0) { Doc tmp; tmp.lang = d->lang; autoName = L"Automatic (" + std::wstring(np::DialectName(DocDialect(&tmp))) + L")"; }
    return { MI(C_DIALECT_AUTO, autoName, L"", I_NONE, cur < 0), MSep(), MI(C_DIALECT_MD, L"Markdown", L"", I_MARKDOWN, cur == 0),
             MI(C_DIALECT_BB, L"BBCode", L"", I_CODE, cur == 1), MI(C_DIALECT_HTML, L"HTML", L"", I_CODE, cur == 2) };
}

static MenuModel BuildMenu(int which) {
    Doc* d = Cur();
    bool hex = d && d->hex, txt = d && !d->hex && !d->loading;
    MenuModel m;
    auto& v = m.items;
    if (which == 0) {
        std::vector<MenuItem> rec;
        for (int i = 0; i < (int)g_set.recent.size(); ++i) rec.push_back(MI(C_RECENT0 + i, g_set.recent[i], L"", I_FILE));
        if (rec.empty()) rec.push_back(MI(0, L"(empty)", L"", I_NONE, false, false));
        v = { MI(C_NEW, L"New tab", L"Ctrl+N", I_TAB), MI(C_NEWWIN, L"New window", L"Ctrl+Shift+N", I_NEWWIN), MI(C_NEWMD, L"New Markdown tab", L"", I_MARKDOWN),
              MI(C_OPEN, L"Open", L"Ctrl+O", I_OPEN), MSub(L"Recent", I_RECENT, rec), MSep(),
              MI(C_SAVE, L"Save", L"Ctrl+S", I_SAVE, false, d && !d->loading), MI(C_SAVEAS, L"Save as", L"Ctrl+Shift+S", I_SAVE, false, d && !d->loading),
              MI(C_SAVEALL, L"Save all", L"Ctrl+Alt+S", I_SAVE),
              MSub(L"Save with encoding", I_ENCODING, EncodingItems(d, false)), MSub(L"Reopen with encoding", I_ENCODING, EncodingItems(d, true)),
              MSub(L"Export as", I_EXPORT, { MI(C_EXPORT_HTML, L"HTML document", L"", I_CODE), MI(C_EXPORT_BB, L"BBCode", L"", I_CODE), MI(C_EXPORT_MD, L"Markdown", L"", I_MARKDOWN) }, txt),
              MSep(), MI(C_PRINT, L"Print", L"Ctrl+P", I_PRINT, false, txt), MSep(),
              MI(C_CLOSETAB, L"Close tab", L"Ctrl+W", I_CLOSE), MI(C_CLOSEWIN, L"Close window", L"Ctrl+Shift+W", I_NEWWIN), MI(C_EXIT, L"Exit", L"", I_EXIT) };
    } else if (which == 1) {
        bool sel = d && (hex ? d->hexCaret != d->hexAnchor : EditorHasSel(d));
        bool canUndo = d && (hex ? !d->hexUndo.empty() : d->text.CanUndo()), canRedo = d && (hex ? !d->hexRedo.empty() : d->text.CanRedo());
        v = { MI(C_UNDO, L"Undo", L"Ctrl+Z", I_UNDO, false, canUndo), MI(C_REDO, L"Redo", L"Ctrl+Y", I_REDO, false, canRedo), MSep(),
              MI(C_CUT, L"Cut", L"Ctrl+X", I_CUT, false, txt), MI(C_COPY, L"Copy", L"Ctrl+C", I_COPY, false, sel), MI(C_PASTE, L"Paste", L"Ctrl+V", I_PASTE),
              MI(C_DELETE, L"Delete", L"Del", I_DELETE), MSep(),
              MI(C_FIND, L"Find", L"Ctrl+F", I_SEARCH, false, txt), MI(C_FINDNEXT, L"Find next", L"F3", I_ARROW_DOWN, false, txt),
              MI(C_FINDPREV, L"Find previous", L"Shift+F3", I_ARROW_UP, false, txt), MI(C_REPLACE, L"Replace", L"Ctrl+H", I_REPLACE, false, txt),
              MI(C_GOTO, L"Go to", L"Ctrl+G", I_GOTO), MSep(),
              MI(C_SELALL, L"Select all", L"Ctrl+A", I_SELECTALL), MI(C_TIMEDATE, L"Time/Date", L"F5", I_RECENT, false, txt), MSep(),
              MSub(L"Format", I_BOLD, FormatItems(), txt), MSub(L"Insert", I_ADD, InsertItems(), txt),
              MSub(L"Evaluate", I_CALC, { MI(C_EVAL, L"Evaluate expression", L"Ctrl+Shift+Enter", I_CALC, false, txt), MI(C_EVAL_REPLACE, L"Replace with result", L"", I_NONE, false, txt), MSep(), MI(C_EVAL_RESET, L"Clear variables") }, txt),
              MSub(L"Line operations", I_INDENT, { MI(C_DUPLINE, L"Duplicate line", L"Ctrl+D", I_COPY), MI(C_MOVEUP, L"Move line up", L"Alt+Up", I_ARROW_UP),
                    MI(C_MOVEDOWN, L"Move line down", L"Alt+Down", I_ARROW_DOWN), MI(C_TOGGLECOMMENT, L"Toggle comment", L"Ctrl+/", I_COMMENT, false, d && np::LineCommentPrefix(d->lang)),
                    MSep(), MI(C_UPPER, L"UPPERCASE", L"Ctrl+Shift+U"), MI(C_LOWER, L"lowercase", L"Ctrl+U"), MI(C_TRIMWS, L"Trim trailing whitespace"),
                    MI(C_SORTLINES, L"Sort lines", L"", I_SORT) }, txt),
              MSub(L"Line endings", I_EOL, EolItems(d)), MSep(), MI(C_FONT, L"Font", L"", I_FONT) };
    } else {
        v = { MSub(L"Zoom", I_ZOOM, { MI(C_ZOOMIN, L"Zoom in", L"Ctrl+Plus"), MI(C_ZOOMOUT, L"Zoom out", L"Ctrl+Minus"), MI(C_ZOOMRESET, L"Restore default zoom", L"Ctrl+0") }),
              MI(C_STATUSBAR, L"Status bar", L"", I_STATUSBAR, g_set.statusBar), MI(C_WORDWRAP, L"Word wrap", L"Alt+Z", I_WRAP, g_set.wordWrap),
              MI(C_LINENUMS, L"Line numbers", L"", I_LINENUM, g_set.lineNumbers), MI(C_FORMATBAR, L"Formatting toolbar", L"", I_TOOLBAR, g_set.formatBar), MSep(),
              MSub(L"Code folding", I_FOLD, { MI(C_FOLDING, L"Enable code folding", L"", I_NONE, g_set.folding), MSep(),
                    MI(C_FOLDCUR, L"Fold", L"Ctrl+Shift+[", I_NONE, false, g_set.folding), MI(C_UNFOLDCUR, L"Unfold", L"Ctrl+Shift+]", I_NONE, false, g_set.folding),
                    MI(C_FOLDALL, L"Fold all", L"", I_NONE, false, g_set.folding), MI(C_UNFOLDALL, L"Unfold all", L"", I_NONE, false, g_set.folding) }),
              MI(C_SYNTAX, L"Syntax highlighting", L"", I_CODE, g_set.syntax), MSub(L"Language", I_LANG, LangItems(d)),
              MSub(L"Markup", I_MARKDOWN, DialectItems(d)),
              MI(C_SPELL, L"Spell check", L"", I_SPELL, g_set.spell, SpellAvailable()), MSep(),
              MI(C_FMTVIEW, L"Formatted view", L"Ctrl+Shift+F", I_FONT, d && d->fmtView, d && !d->binaryOnly && !d->large && (d->lang == np::L_MD || d->lang == np::L_TEXT)),
              MI(C_MDPREVIEW, L"Split with preview", L"Ctrl+Shift+M", I_EYE, d && d->mdPreview, PreviewSupported(d)),
              MI(C_GRID, L"Table view", L"Ctrl+Shift+T", I_TABLE, d && d->grid, d && !d->binaryOnly && !d->loading && !d->large),
              MI(C_HEX, L"Hex editor", L"Ctrl+Shift+H", I_HEX, hex, d && !d->binaryOnly && !d->loading) };
        if (g_set.online) v.push_back(MI(C_AIPANE, L"AI assist", L"Ctrl+Shift+I", I_SPARKLE, g_aiPane));
        v.push_back(MSep());
        v.push_back(MSub(L"Theme", I_THEME, { MI(C_THEME_LIGHT, L"Light", L"", I_NONE, g_set.theme == 1), MI(C_THEME_DARK, L"Dark", L"", I_NONE, g_set.theme == 2), MI(C_THEME_PINK, L"Pink", L"", I_NONE, g_set.theme == 3),
                                              MI(C_THEME_SYS, L"Use system setting", L"", I_NONE, g_set.theme == 0) }));
        v.push_back(MI(C_SETTINGS, L"Settings", L"Ctrl+,", I_GEAR));
    }
    return m;
}
static void TrackAndRun(const MenuModel& m, float xDip, float yDip, int flags = 0) {
    POINT p{ (LONG)(xDip * g_scale), (LONG)(yDip * g_scale) };
    ClientToScreen(g_hwnd, &p);
    int cmd = RunMenu(m, p.x, p.y, flags);
    if (cmd) RunCommand(cmd);
}

void ShowContextMenu(Doc* d, int sx, int sy) {
    if (d && d->grid && !d->hex) { GridContextMenu(d, sx, sy); return; }
    MenuModel m;
    s_ctxSug.clear();
    bool hex = d && d->hex, txt = d && !hex && !d->loading;
    bool sel = d && (hex ? d->hexCaret != d->hexAnchor : EditorHasSel(d));
    m.bar = { MI(C_CUT, L"Cut", L"", I_CUT, false, txt && sel), MI(C_COPY, L"Copy", L"", I_COPY, false, sel), MI(C_PASTE, L"Paste", L"", I_PASTE),
              MI(C_SELALL, L"Select all", L"", I_SELECTALL), MI(C_DELETE, L"Delete", L"", I_DELETE, false, sel) };
    auto& v = m.items;
    v.push_back(MI(C_UNDO, L"Undo", L"Ctrl+Z", I_UNDO, false, d && (hex ? !d->hexUndo.empty() : d->text.CanUndo())));
    if (txt) {
        v.push_back(MSep());
        v.push_back(MI(C_EVAL, EditorHasSel(d) ? L"Evaluate selection" : L"Evaluate expression", L"Ctrl+Shift+Enter", I_CALC));
        v.push_back(MI(C_EVAL_REPLACE, L"Replace with result", L"", I_CALC));
        v.push_back(MSep());
        v.push_back(MI(C_FMT_LINK, L"Link…", L"Ctrl+K", I_LINK));
        v.push_back(MI(C_INS_IMAGE, L"Image…", L"", I_IMAGE));
        v.push_back(MSub(L"Table", I_TABLE, TableItems()));
        v.push_back(MSub(L"Insert", I_ADD, InsertItems()));
        v.push_back(MSub(L"Format", I_BOLD, FormatItems()));
        if (g_set.online) {
            v.push_back(MSep());
            v.push_back(MI(C_AI_REWRITE, L"Rewrite", L"", I_SPARKLE)); v.push_back(MI(C_AI_SUMMARIZE, L"Summarize", L"", I_SPARKLE));
            v.push_back(MSub(L"More AI", I_SPARKLE, { MI(C_AI_EXPLAIN, L"Explain"), MI(C_AI_GRAMMAR, L"Fix grammar"), MI(C_AI_CONTINUE, L"Continue writing"), MI(C_AI_CUSTOM, L"Ask…") }));
        }
        v.push_back(MSep());
        if (g_set.spell && SpellAvailable() && !d->large) {
            const std::wstring& line = d->text.Line(d->caret.line);
            std::vector<std::pair<int, int>> errs; SpellCheckLine(line, errs);
            std::vector<MenuItem> sp;
            for (auto& e : errs) if (d->caret.col >= e.first && d->caret.col <= e.first + e.second) {
                s_ctxA = { d->caret.line, e.first }; s_ctxB = { d->caret.line, e.first + e.second };
                s_ctxSug = SpellSuggest(line.substr(e.first, e.second));
                for (int i = 0; i < (int)s_ctxSug.size() && i < 15; ++i) sp.push_back(MI(C_SPELL_SUG0 + i, s_ctxSug[i]));
                if (s_ctxSug.empty()) sp.push_back(MI(0, L"(no suggestions)", L"", I_NONE, false, false));
                sp.push_back(MSep()); sp.push_back(MI(C_SPELL_ADD, L"Add to dictionary")); sp.push_back(MI(C_SPELL_IGNORE, L"Ignore all"));
                s_ctxSug.push_back(line.substr(e.first, e.second));
                break;
            }
            if (!sp.empty()) v.push_back(MSub(L"Spelling", I_SPELL, sp));
        }
        v.push_back(MSub(L"Unicode control characters", I_UNICODE, {
            MI(C_UNI0 + 0, L"Left-to-right mark (LRM)"), MI(C_UNI0 + 1, L"Right-to-left mark (RLM)"), MI(C_UNI0 + 2, L"Zero width joiner (ZWJ)"),
            MI(C_UNI0 + 3, L"Zero width non-joiner (ZWNJ)"), MI(C_UNI0 + 4, L"Zero width space (ZWSP)"), MI(C_UNI0 + 5, L"No-break space (NBSP)"),
            MI(C_UNI0 + 6, L"Soft hyphen (SHY)"), MI(C_UNI0 + 7, L"Byte order mark (BOM)") }));
        if (np::LineCommentPrefix(d->lang)) v.push_back(MI(C_TOGGLECOMMENT, L"Toggle comment", L"Ctrl+/", I_COMMENT));
        if (g_set.folding && !d->large && !LiveOn(d)) { v.push_back(MI(C_FOLDCUR, L"Fold", L"", I_FOLD)); v.push_back(MI(C_UNFOLDCUR, L"Unfold", L"", I_CHEV_RIGHT)); }
    }
    int cmd = RunMenu(m, sx, sy);
    if (cmd) RunCommand(cmd);
}

// ------------------------------------------------------------------ markup formatting
static void FmtInline(np::Inline k) {
    Doc* d = Cur(); if (!d || d->hex || d->loading) return;
    np::Pos a = std::min(d->caret, d->anchor), b = std::max(d->caret, d->anchor);
    std::wstring sel = d->text.GetText(a, b);
    np::MarkupEdit e = np::WrapInline(DocDialect(d), k, sel);
    size_t base = d->text.Offset(a);
    d->text.BreakCoalesce();
    d->anchor = a; d->caret = b;
    EditorInsert(d, e.text);
    EditorSelect(d, d->text.PosAt(base + e.selStart), d->text.PosAt(base + e.selEnd));
}
static void FmtLines(np::LineOp op, int level = 1) {
    Doc* d = Cur(); if (!d || d->hex || d->loading) return;
    np::Pos a = std::min(d->caret, d->anchor), b = std::max(d->caret, d->anchor);
    if (b.col == 0 && b.line > a.line) b.line--;
    a.col = 0; b.col = (int)d->text.Line(b.line).size();
    std::wstring r = np::ApplyLineOp(DocDialect(d), op, d->text.GetText(a, b), level);
    size_t base = d->text.Offset(a);
    d->text.BreakCoalesce();
    d->anchor = a; d->caret = b;
    EditorInsert(d, r);
    EditorSelect(d, d->text.PosAt(base), d->text.PosAt(base + r.size()));
}
static void InsertBlock(const std::wstring& t) {
    Doc* d = Cur(); if (!d || d->hex || d->loading) return;
    std::wstring s = t;
    if (d->caret.col > 0 && !s.empty() && s[0] != L'\n') s = L"\n" + s;
    if (DocDialect(d) == np::Dialect::Markdown && d->caret.line > 0 && !s.empty() && s[0] == L'\n' && !d->text.Line(d->caret.line).empty()) s = L"\n" + s;
    else if (DocDialect(d) == np::Dialect::Markdown && d->caret.col == 0 && d->caret.line > 0 && !d->text.Line(d->caret.line - 1).empty()) s = L"\n" + s;
    if (!s.empty() && s.back() != L'\n') s += L"\n";
    d->text.BreakCoalesce();
    EditorInsert(d, s);
}
static void TableCustomDone(const std::wstring& s) {
    int r = 3, c = 3; swscanf(s.c_str(), L"%d %*[xX×*,] %d", &r, &c);
    if (swscanf(s.c_str(), L"%dx%d", &r, &c) < 2) swscanf(s.c_str(), L"%d %d", &r, &c);
    Doc* d = Cur(); int at = d ? d->caret.line : 0;
    InsertBlock(np::MakeTable(DocDialect(d), std::max(1, std::min(r, 200)), std::max(1, std::min(c, 50))));
    if (d) LiveEditTableNear(d, at);
}

// ------------------------------------------------------------------ misc commands
static void GotoDone(const std::wstring& s) {
    Doc* d = Cur(); if (!d) return;
    if (d->hex) {
        size_t off = wcstoull(s.c_str(), nullptr, (s.rfind(L"0x", 0) == 0 || s.find_first_of(L"abcdefABCDEF") != std::wstring::npos) ? 16 : 10);
        d->hexCaret = d->hexAnchor = std::min(off, d->bytes.size()); d->hexTop = (int)(d->hexCaret / 16); Invalidate(); return;
    }
    int line = _wtoi(s.c_str());
    if (line < 1 || line > d->text.LineCount()) { SetStatus(L"Line number out of range"); return; }
    np::Pos p{ line - 1, 0 };
    EditorUnfoldFor(d, p.line);
    EditorSelect(d, p, p);
}
static void SetEndpoint(const std::wstring& s) { g_set.aiEndpoint = s; SettingsSave(); }
static void SetModel(const std::wstring& s) { g_set.aiModel = s; SettingsSave(); }
static void SetKey(const std::wstring& s) { g_set.aiKey = s; SettingsSave(); }
static void ChooseEditorFont() {
    LOGFONTW lf{};
    HDC dc = GetDC(g_hwnd);
    lf.lfHeight = -MulDiv((int)std::lround(g_set.fontSize), GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(g_hwnd, dc);
    lf.lfWeight = g_set.fontBold ? FW_BOLD : FW_NORMAL; lf.lfItalic = g_set.fontItalic;
    wcsncpy_s(lf.lfFaceName, g_set.fontFace.c_str(), _TRUNCATE);
    CHOOSEFONTW cf{ sizeof(cf) };
    cf.hwndOwner = g_hwnd; cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;
    if (!ChooseFontW(&cf)) return;
    g_set.fontFace = lf.lfFaceName; g_set.fontSize = cf.iPointSize / 10.f;
    g_set.fontBold = lf.lfWeight >= FW_SEMIBOLD; g_set.fontItalic = lf.lfItalic != 0;
    EditorClearCache(); SettingsSave(); Invalidate();
}
static void SetZoom(int z) {
    g_set.zoom = std::max(10, std::min(500, z));
    EditorClearCache(); MdClearCache();
    Doc* d = Cur(); if (d) EditorEnsureCaret(d);
    Invalidate();
}
static void NewWindow() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" /new";
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}
static bool CloseWindowAll() {
    if (s_sessionOwner && g_set.restoreSession && !s_noSession) { SessionSave(); return true; }
    for (int i = 0; i < (int)g_docs.size(); ++i) { SelectTab(i); if (!ConfirmClose(g_docs[i].get())) return false; }
    for (auto& d : g_docs) SessionDeleteBackup(d.get());
    if (s_sessionOwner) { g_docs.clear(); g_cur = -1; SessionSave(); }
    return true;
}

static void ApplyBackdrop();
void RunCommand(int cmd) {
    Doc* d = Cur();
    if (cmd >= C_RECENT0 && cmd < C_RECENT0 + 10) { int i = cmd - C_RECENT0; if (i < (int)g_set.recent.size()) OpenPath(g_set.recent[i]); return; }
    if (cmd >= C_ENC0 && cmd < C_ENC0 + 5) { if (d) { d->text.enc = (np::Enc)(cmd - C_ENC0); d->text.MarkModified(); UpdateTitle(); SetStatus(L"Encoding will be applied when you save"); } return; }
    if (cmd >= C_REOPEN0 && cmd < C_REOPEN0 + 5) {
        if (d && !d->path.empty() && (!d->Modified() || ThemedMessageBox(g_hwnd, L"Discard unsaved changes and reopen?", L"ModernNotepad", MB_OKCANCEL) == IDOK))
            OpenPath(d->path, false, cmd - C_REOPEN0);
        return;
    }
    if (cmd >= C_LANG0 && cmd < C_LANG0 + (int)np::L_COUNT) {
        if (d) { d->lang = (np::Lang)(cmd - C_LANG0); d->langAuto = false; d->lex.Reset(); d->foldsVer = UINT64_MAX; EditorClearCache(); Invalidate(); }
        return;
    }
    if (cmd >= C_SPELL_SUG0 && cmd < C_SPELL_SUG0 + 16) {
        int i = cmd - C_SPELL_SUG0;
        if (d && i + 1 < (int)s_ctxSug.size()) { d->anchor = s_ctxA; d->caret = s_ctxB; d->text.BreakCoalesce(); EditorInsert(d, s_ctxSug[i]); }
        return;
    }
    if (cmd >= C_UNI0 && cmd < C_UNI0 + 8) {
        static const wchar_t uni[8] = { 0x200E, 0x200F, 0x200D, 0x200C, 0x200B, 0x00A0, 0x00AD, 0xFEFF };
        if (d && !d->hex && !d->loading) EditorInsert(d, std::wstring(1, uni[cmd - C_UNI0]));
        return;
    }
    if (cmd >= C_INS_TABLE0 && cmd < C_INS_TABLE0 + 6) {
        static const int dims[6][2] = { {2,2}, {3,2}, {3,3}, {4,3}, {4,4}, {5,5} };
        int at = d ? d->caret.line : 0;
        InsertBlock(np::MakeTable(DocDialect(d), dims[cmd - C_INS_TABLE0][1], dims[cmd - C_INS_TABLE0][0]));
        if (d) LiveEditTableNear(d, at);
        return;
    }
    if (cmd >= C_FMT_H1 && cmd <= C_FMT_H6) { FmtLines(np::LineOp::Heading, cmd - C_FMT_H1 + 1); return; }
    bool aiCmd = cmd == C_AIPANE || (cmd >= C_AI_EXPLAIN && cmd <= C_AI_CONTINUE);
    if (aiCmd && !g_set.online) { SetStatus(L"Online features are off. Enable them in Settings \u203A Online features."); return; }
    if (d && d->loading && cmd != C_CLOSETAB && cmd != C_NEW && cmd != C_OPEN && cmd != C_CLOSEWIN && cmd != C_EXIT && cmd != C_SETTINGS) return;
    switch (cmd) {
    case C_NEW: NewDoc(); SelectTab(g_cur); break;
    case C_NEWMD: { Doc* n = NewDoc(); n->lang = np::L_MD; n->langAuto = false; SelectTab(g_cur); break; }
    case C_EXPORT_HTML: case C_EXPORT_BB: case C_EXPORT_MD: if (d && !d->hex) {
        std::wstring md = PreviewMarkdown(d);
        std::wstring title = d->Name();
        std::wstring out = cmd == C_EXPORT_HTML ? np::MarkdownToHtml(md, true, title) : cmd == C_EXPORT_BB ? np::MarkdownToBBCode(md) : md;
        Doc* n = NewDoc();
        n->lang = cmd == C_EXPORT_HTML ? np::L_HTML : cmd == C_EXPORT_BB ? np::L_BBCODE : np::L_MD; n->langAuto = false;
        n->text.SetText(out); n->text.MarkModified();
        SelectTab(g_cur);
        SetStatus(L"Exported to a new tab");
        } break;
    case C_FMT_BOLD: FmtInline(np::Inline::Bold); break;
    case C_FMT_ITALIC: FmtInline(np::Inline::Italic); break;
    case C_FMT_STRIKE: FmtInline(np::Inline::Strike); break;
    case C_FMT_UNDERLINE: FmtInline(np::Inline::Underline); break;
    case C_FMT_CODE: FmtInline(np::Inline::Code); break;
    case C_FMT_LINK: LinkDialog(); break;
    case C_FMT_PARA: FmtLines(np::LineOp::Paragraph); break;
    case C_FMT_BULLET: FmtLines(np::LineOp::Bullet); break;
    case C_FMT_NUMBER: FmtLines(np::LineOp::Number); break;
    case C_FMT_TASK: FmtLines(np::LineOp::Task); break;
    case C_FMT_QUOTE: FmtLines(np::LineOp::Quote); break;
    case C_FMT_CODEBLOCK: FmtLines(np::LineOp::CodeBlock); break;
    case C_FMT_CLEAR: if (d && !d->hex && EditorHasSel(d)) {
        np::Pos a = std::min(d->caret, d->anchor), b = std::max(d->caret, d->anchor);
        std::wstring r = np::ClearFormatting(DocDialect(d), d->text.GetText(a, b));
        size_t base = d->text.Offset(a); d->anchor = a; d->caret = b; EditorInsert(d, r);
        EditorSelect(d, d->text.PosAt(base), d->text.PosAt(base + r.size())); } break;
    case C_INS_TABLECUSTOM: Prompt(L"Table size (rows \u00D7 columns), e.g. 4x3", L"4x3", TableCustomDone); break;
    case C_INS_CHART_BAR: case C_INS_CHART_LINE: case C_INS_CHART_PIE:
        InsertBlock(np::MakeChartBlock(DocDialect(d), cmd == C_INS_CHART_BAR ? L"bar" : cmd == C_INS_CHART_LINE ? L"line" : L"pie"));
        if (d && !d->mdPreview && PreviewSupported(d)) { d->mdPreview = true; MdClearCache(); }
        break;
    case C_INS_IMAGE: ImageDialog(); break;
    case C_INS_RULE: InsertBlock(np::MakeRule(DocDialect(d))); break;
    case C_DIALECT_AUTO: case C_DIALECT_MD: case C_DIALECT_BB: case C_DIALECT_HTML:
        if (d) { d->dialect = cmd == C_DIALECT_AUTO ? -1 : cmd - C_DIALECT_MD; MdClearCache(); } break;
    case C_FORMATBAR: g_set.formatBar = !g_set.formatBar; break;
    case C_MENUSTYLE: g_set.modernMenus = !g_set.modernMenus; break;
    case C_TRANSPARENCY: g_set.transparency = !g_set.transparency; ApplyBackdrop(); break;
    case C_ONLINE: g_set.online = !g_set.online; if (!g_set.online) g_aiPane = false; break;
    case C_NEWWIN: NewWindow(); break;
    case C_OPEN: OpenDialog(); break;
    case C_SAVE: SaveDoc(d, false); break;
    case C_SAVEAS: SaveDoc(d, true); break;
    case C_SAVEALL: for (auto& x : g_docs) if (x->Modified()) SaveDoc(x.get(), false); break;
    case C_PRINT: if (d) PrintDoc(d); break;
    case C_CLOSETAB: CloseTab(g_cur); break;
    case C_CLOSEWIN: case C_EXIT: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
    case C_UNDO: case C_REDO:
        if (LiveEditing()) LiveCommitEdit();
        if (d && d->grid && !d->hex) { GridCancelEdit(); EditorCommand(d, cmd); Invalidate(); }
        else if (d && d->hex) HexUndo(d, cmd == C_REDO); else if (d) EditorCommand(d, cmd);
        break;
    case C_CUT: if (d && d->grid && !d->hex) GridCopy(d, true); else if (d && !d->hex) EditorCommand(d, cmd); break;
    case C_COPY: if (d) { if (d->grid && !d->hex) GridCopy(d, false); else if (d->hex) HexCopy(d); else EditorCommand(d, cmd); } break;
    case C_PASTE: if (d) { if (d->grid && !d->hex) GridPaste(d); else if (d->hex) HexPaste(d); else EditorCommand(d, cmd); } break;
    case C_DELETE: if (d) { if (d->grid && !d->hex) GridKey(d, VK_DELETE); else if (d->hex) HexKey(d, VK_DELETE); else EditorCommand(d, cmd); } break;
    case C_SELALL: if (d) { if (d->grid && !d->hex) { d->ga = 0; d->gca = 0; d->gr = d->table.Rows(); d->gc = std::max(0, d->table.Cols() - 1); Invalidate(); } else if (d->hex) { d->hexAnchor = 0; d->hexCaret = d->bytes.size(); Invalidate(); } else EditorCommand(d, cmd); } break;
    case C_DUPLINE: case C_TOGGLECOMMENT: case C_MOVEUP: case C_MOVEDOWN: case C_UPPER: case C_LOWER: case C_TRIMWS: case C_SORTLINES:
    case C_FOLDCUR: case C_UNFOLDCUR: case C_FOLDALL: case C_UNFOLDALL:
        if (d && !d->hex) EditorCommand(d, cmd);
        break;
    case C_FIND: if (d && !d->hex) OpenFind(false); break;
    case C_REPLACE: if (d && !d->hex) OpenFind(true); break;
    case C_FINDNEXT: case C_FINDPREV:
        if (!s_findOpen && d && EditorHasSel(d)) { SetWindowTextW(s_findEdit, EditorSelection(d).c_str()); s_findOpen = true; }
        if (GetWindowTextLengthW(s_findEdit) == 0) OpenFind(false); else FindNext(cmd == C_FINDNEXT);
        break;
    case C_GOTO: if (d) Prompt(d->hex ? L"Go to offset (hex with 0x or decimal)" : L"Go to line", d->hex ? L"0x0" : std::to_wstring(d->caret.line + 1), GotoDone); break;
    case C_TIMEDATE: if (d && !d->hex) {
        wchar_t t[64] = {}, dt[64] = {};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, t, 64);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, dt, 64, nullptr);
        EditorInsert(d, std::wstring(t) + L" " + dt); } break;
    case C_FONT: ChooseEditorFont(); break;
    case C_EOL_CRLF: case C_EOL_LF: case C_EOL_CR:
        if (d) { np::Eol e = (np::Eol)(cmd - C_EOL_CRLF); if (d->text.eol != e) { d->text.eol = e; d->text.MarkModified(); UpdateTitle(); } }
        break;
    case C_ZOOMIN: SetZoom(g_set.zoom < 100 ? g_set.zoom + 10 : g_set.zoom + 10); break;
    case C_ZOOMOUT: SetZoom(g_set.zoom - 10); break;
    case C_ZOOMRESET: SetZoom(100); break;
    case C_STATUSBAR: g_set.statusBar = !g_set.statusBar; break;
    case C_WORDWRAP: g_set.wordWrap = !g_set.wordWrap; EditorClearCache(); if (d) { d->scrollX = 0; EditorEnsureCaret(d); } break;
    case C_LINENUMS: g_set.lineNumbers = !g_set.lineNumbers; break;
    case C_FOLDING:
        g_set.folding = !g_set.folding;
        for (auto& x : g_docs) { x->foldsVer = UINT64_MAX; if (!g_set.folding) { for (int i = 0; i < x->text.LineCount(); ++i) x->text.Flags(i) &= ~np::LF_FOLDED; x->folds.clear(); x->rowsDirty = true; } }
        break;
    case C_SYNTAX: g_set.syntax = !g_set.syntax; EditorClearCache(); break;
    case C_SPELL: g_set.spell = !g_set.spell; EditorClearCache(); break;
    case C_MDPREVIEW: if (d && !d->binaryOnly) { if (LiveEditing()) LiveCommitEdit(); d->mdPreview = !d->mdPreview; if (d->mdPreview) d->fmtView = false; d->rowsDirty = true; EditorClearCache(); MdClearCache(); MdSyncTo(d, d->caret.line); } break;
    case C_FMTVIEW: if (d && !d->binaryOnly && !d->large) {
        if (LiveEditing()) LiveCommitEdit();
        d->fmtView = !d->fmtView; if (d->fmtView) { d->mdPreview = false; d->grid = false; if (d->hex) HexLeave(d); }
        d->liveVer = UINT64_MAX; d->rowsDirty = true; d->anyFolded = false; d->foldsVer = UINT64_MAX;
        EditorClearCache(); EditorEnsureCaret(d);
        SetStatus(d->fmtView ? L"Formatted view (Ctrl+Shift+F shows the Markdown)" : L"Markdown source view");
    } break;
    case C_INS_LINKDLG: break;
    case C_EVAL: case C_EVAL_REPLACE: if (LiveEditing()) LiveCommitEdit(); EvaluateCommand(d, cmd == C_EVAL_REPLACE); break;
    case C_EVAL_RESET: EvaluateReset(); break;
    case C_GRID:
        if (d && !d->binaryOnly && !d->loading && !d->large) {
            if (d->hex) HexLeave(d);
            GridCancelEdit();
            if (d->grid) { d->grid = false; EditorEnsureCaret(d); } else { GridEnter(d); CloseFind(); d->mdPreview = false; }
            UpdateTitle();
        }
        break;
    case C_GRID_SORTASC: case C_GRID_SORTDESC: case C_GRID_INSROWABOVE: case C_GRID_INSROWBELOW: case C_GRID_DELROW:
    case C_GRID_INSCOLLEFT: case C_GRID_INSCOLRIGHT: case C_GRID_DELCOL: case C_GRID_AUTOFIT: case C_GRID_HEADER: case C_GRID_EDIT:
    case C_GRID_DELIM0: case C_GRID_DELIM0 + 1: case C_GRID_DELIM0 + 2: case C_GRID_DELIM0 + 3:
        if (d && d->grid) GridCommand(d, cmd);
        break;
    case C_HEX:
        if (d && !d->binaryOnly) {
            if (d->hex) { HexLeave(d); EditorEnsureCaret(d); }
            else { HexEnter(d); CloseFind(); }
            UpdateTitle();
        }
        break;
    case C_AIPANE: g_aiPane = !g_aiPane; break;
    case C_SETTINGS: s_settingsOpen = !s_settingsOpen; s_settingsScroll = 0; break;
    case C_THEME_SYS: case C_THEME_LIGHT: case C_THEME_DARK: case C_THEME_PINK:
        g_set.theme = cmd == C_THEME_SYS ? 0 : cmd == C_THEME_LIGHT ? 1 : cmd == C_THEME_PINK ? 3 : 2; BuildTheme(); ApplyBackdrop(); break;
    case C_AI_EXPLAIN: case C_AI_SUMMARIZE: case C_AI_REWRITE: case C_AI_GRAMMAR: case C_AI_CONTINUE: AiRun(cmd); break;
    case C_AI_CUSTOM: Prompt(L"Ask about the selection / document", L"", [](const std::wstring& s) { if (!s.empty()) AiRun(C_AI_CUSTOM, s); }); break;
    case C_SPELL_ADD: if (!s_ctxSug.empty()) SpellAdd(s_ctxSug.back()); break;
    case C_SPELL_IGNORE: if (!s_ctxSug.empty()) SpellIgnore(s_ctxSug.back()); break;
    case C_TAB_CLOSEOTHERS: { int keep = g_cur; for (int i = (int)g_docs.size() - 1; i >= 0; --i) if (i != keep) { CloseTab(i); if (i < keep) --keep; } break; }
    case C_TAB_CLOSERIGHT: for (int i = (int)g_docs.size() - 1; i > g_cur; --i) CloseTab(i); break;
    case C_TAB_COPYPATH: if (d && !d->path.empty()) ClipboardSet(d->path); break;
    case C_TAB_OPENFOLDER: if (d && !d->path.empty()) { std::wstring a = L"/select,\"" + d->path + L"\""; ShellExecuteW(g_hwnd, nullptr, L"explorer.exe", a.c_str(), nullptr, SW_SHOWNORMAL); } break;
    case C_ABOUT: break;
    }
    SettingsSave();
    Invalidate();
}

// ------------------------------------------------------------------ layout & painting
static void Hit(RectF r, int kind, int idx = 0) { s_hits.push_back({ r, kind, idx }); }
static bool IsHot(int kind, int idx = 0) { return s_hotKind == kind && s_hotIdx == idx; }
static bool IsPressed(int kind, int idx = 0) { return s_pressKind == kind && s_pressIdx == idx; }
static void HoverFill(RectF r, int kind, int idx, float rad = 4) {
    if (IsPressed(kind, idx) && IsHot(kind, idx)) FillRR(r, rad, g_th.press);
    else if (IsHot(kind, idx)) FillRR(r, rad, g_th.hover);
}
static float ClientW() { RECT rc; GetClientRect(g_hwnd, &rc); return rc.right / g_scale; }
static float ClientH() { RECT rc; GetClientRect(g_hwnd, &rc); return rc.bottom / g_scale; }

static Color BarFill() { return g_mica ? (g_th.pink ? Alpha(g_th.chrome, 0.72f) : D2D1::ColorF(0, 0, 0, 0)) : g_th.surface; }   // status bar only

// Concave "foot" joining the active tab to the menu bar. Drawn as a filled polygon (quarter circle flattened
// by hand) so it works over a transparent Mica tab strip, where the old trick of punching the curve out with
// the chrome colour would paint an opaque blob.
static void TabFoot(float cornerX, float bottom, float rad, bool left, Color c) {
    ID2D1PathGeometry* g = nullptr; if (FAILED(g_d2d->CreatePathGeometry(&g))) return;
    ID2D1GeometrySink* k = nullptr;
    if (SUCCEEDED(g->Open(&k))) {
        float sx = left ? -1.f : 1.f;              // which side of the tab the foot sticks out to
        float cx = cornerX + sx * rad, cy = bottom - rad;
        k->BeginFigure({ cornerX, bottom }, D2D1_FIGURE_BEGIN_FILLED);
        k->AddLine({ cornerX, cy });
        for (int i = 1; i <= 10; ++i) {            // from (cornerX, bottom-rad) to (cornerX+sx*rad, bottom)
            float a = 3.14159265f / 2 * i / 10;
            k->AddLine({ cx - sx * rad * std::cos(a), cy + rad * std::sin(a) });
        }
        k->EndFigure(D2D1_FIGURE_END_CLOSED);
        k->Close(); k->Release();
        g_rt->FillGeometry(g, Brush(c));
    }
    g->Release();
}

// With Mica on, the frame is extended over the whole client so DWM would draw its own min/max/close on top of
// ours (two overlapping sets, the DWM one sized for the system caption, not our taller tab strip). DWM only
// draws them for windows with WS_SYSMENU, so drop that style and provide the system menu ourselves.
static void SyncSysMenuStyle() {
    LONG_PTR st = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
    LONG_PTR want = g_mica ? (st & ~(LONG_PTR)WS_SYSMENU) : (st | WS_SYSMENU);
    if (want != st) { SetWindowLongPtrW(g_hwnd, GWL_STYLE, want); SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED); }
}
static void ShowSystemMenu(int sx, int sy) {
    HMENU m = GetSystemMenu(g_hwnd, FALSE); if (!m) return;
    bool z = IsZoomed(g_hwnd) != 0, ic = IsIconic(g_hwnd) != 0;
    EnableMenuItem(m, SC_RESTORE, MF_BYCOMMAND | (z || ic ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m, SC_MOVE, MF_BYCOMMAND | (z ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(m, SC_SIZE, MF_BYCOMMAND | (z ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(m, SC_MINIMIZE, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(m, SC_MAXIMIZE, MF_BYCOMMAND | (z ? MF_GRAYED : MF_ENABLED));
    SetMenuDefaultItem(m, SC_CLOSE, FALSE);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : 0), sx, sy, 0, g_hwnd, nullptr);
    if (cmd) PostMessageW(g_hwnd, WM_SYSCOMMAND, cmd, 0);
}

static void PaintTabs(float W) {
    float right = W - CAPBTN_W * 3;
    DrawAppIcon(12, (CAP_H - 18) / 2 + 1, 18);
    float x = 40, maxTabsW = right - x - 40 - 48;
    int n = (int)g_docs.size();
    float tw = std::max(64.f, std::min(240.f * std::min(g_textScale, 1.3f), maxTabsW / std::max(1, n)));
    IDWriteTextFormat* f = UiFmt(12.5f);
    float top = std::round(CAP_H * 0.15f);
    for (int i = 0; i < n; ++i) {
        Doc* d = g_docs[i].get();
        RectF r = RF(x, top, tw, CAP_H - top);
        if (s_dragTab == i) { r.left = s_dragTabX; r.right = r.left + tw; }
        bool act = i == g_cur;
        bool hot = IsHot(HT_TAB, i) || IsHot(HT_TABCLOSE, i);
        if (act) {
            // same opaque surface as the menu bar, flowing into it through two concave feet (v1.0 look);
            // only the strip around the tabs shows the Mica gradient
            FillRR(RF(r.left, r.top, RW(r), RH(r) + 10), 8, g_th.surface);
            TabFoot(r.left, r.bottom, 6, true, g_th.surface);
            TabFoot(r.right, r.bottom, 6, false, g_th.surface);
        } else {
            if (hot) FillRR(RF(r.left + 2, r.top + 2, RW(r) - 4, RH(r) - 8), 6, g_th.hover);
            if (i + 1 != g_cur && i + 1 < n) Line(r.right, r.top + 10, r.right, r.bottom - 12, g_th.border);
        }
        RectF tr = RF(r.left + 12, r.top, RW(r) - 12 - 34, RH(r) - (act ? 0 : 4));
        std::wstring name = d->Name();
        if (d->hex) name += L" [hex]"; else if (d->grid) name += L" [table]";
        if (d->loading) name = L"⌛ " + name;
        Text(name, tr, act ? UiFmt(12.5f, true) : f, act ? g_th.text : g_th.dim);
        RectF cr = RF(r.right - 32, r.top + (RH(r) - 24) / 2 - (act ? 0 : 2), 24, 24);
        // Win11: a modified tab shows a grey dot in the close slot; the X appears on hover
        if (d->Modified() && !hot) DrawIcon(I_DOT, (cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2, 11, g_th.dim);
        else if (act || hot) {
            HoverFill(cr, HT_TABCLOSE, i);
            DrawIcon(I_CLOSE, (cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2, 10, g_th.dim, 1.2f);
        }
        Hit(r, HT_TAB, i);
        Hit(cr, HT_TABCLOSE, i);
        x += tw;
    }
    RectF nr = RF(x + 6, top + 3, 32, CAP_H - top - 9);
    HoverFill(nr, HT_NEWTAB, 0, 5);
    DrawIcon(I_ADD, (nr.left + nr.right) / 2, (nr.top + nr.bottom) / 2, 13, g_th.text, 1.2f);
    Hit(nr, HT_NEWTAB);
    bool maxed = IsZoomed(g_hwnd) != 0;
    bool active = GetActiveWindow() == g_hwnd;
    for (int b = 0; b < 3; ++b) {
        RectF r = RF(W - CAPBTN_W * (3 - b), 0, CAPBTN_W, CAP_H - 4);
        int kind = b == 0 ? HT_MIN : b == 1 ? HT_MAX : HT_CLOSE;
        bool hot = IsHot(kind), pr = IsPressed(kind) && hot;
        if (kind == HT_CLOSE && hot) FillR(r, pr ? Hex(0xC42B1C, 0.85f) : Hex(0xC42B1C));
        else if (hot) FillR(r, pr ? g_th.press : g_th.hover);
        Color ic = (kind == HT_CLOSE && hot) ? Hex(0xFFFFFF) : (active ? g_th.text : g_th.dim);
        Icon icon = b == 0 ? I_MIN : b == 1 ? (maxed ? I_RESTORE : I_MAX) : I_CLOSE;
        DrawIcon(icon, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 10 * std::min(g_textScale, 1.25f), ic, 1.f);
    }
}

static const wchar_t* kMenuNames[3] = { L"File", L"Edit", L"View" };
enum { FB_DIALECT = 1, FB_HEADING, FB_LIST, FB_BOLD, FB_ITALIC, FB_STRIKE, FB_LINK, FB_TABLE, FB_INSERT, FB_CLEAR };

static void PaintFormatBar(float x0, float x1) {
    Doc* d = Cur();
    if (!g_set.formatBar || !d || d->hex || d->grid || !(d->lang == np::L_TEXT || d->lang == np::L_MD || d->lang == np::L_HTML || d->lang == np::L_BBCODE) || d->binaryOnly || d->loading || s_settingsOpen) return;
    struct B { int id; Icon icon; const wchar_t* label; bool drop; float w; };
    B bs[] = { { FB_HEADING, I_NONE, L"H1", true, 0 }, { FB_LIST, I_LIST, nullptr, true, 0 },
               { FB_BOLD, I_BOLD, nullptr, false, 0 }, { FB_ITALIC, I_ITALIC, nullptr, false, 0 }, { FB_STRIKE, I_STRIKE, nullptr, false, 0 },
               { FB_LINK, I_LINK, nullptr, false, 0 }, { FB_TABLE, I_TABLE, nullptr, true, 0 }, { FB_INSERT, I_ADD, nullptr, true, 0 }, { FB_CLEAR, I_CLEARFMT, nullptr, false, 0 } };
    float total = 0;
    for (auto& b : bs) { b.w = (b.label ? TextW(b.label, UiFmt(12.5f, true)) + 14 : 30) + (b.drop ? 14 : 0); total += b.w + 2; }
    total += 12;   // separators
    float x = std::max(x0, (x0 + x1) / 2 - total / 2);
    if (x + total > x1) return;   // not enough room: hide (still in Edit > Format)
    float y = CAP_H + 4, h = MENU_H - 8;
    for (auto& b : bs) {
        RectF r = RF(x, y, b.w, h);
        HoverFill(r, HT_FMT, b.id, 5);
        float cx = r.left + (b.drop ? (b.w - 14) / 2 : b.w / 2) + (b.drop ? 2 : 0);
        if (b.label) Text(b.label, RF(r.left + 4, r.top, b.w - (b.drop ? 18 : 8), RH(r)), UiFmt(12.5f, true), g_th.text, 1);
        else DrawIcon(b.icon, cx, (r.top + r.bottom) / 2, 15 * g_textScale, g_th.text, 1.2f);
        if (b.drop) DrawIcon(I_CHEV_DOWN, r.right - 9, (r.top + r.bottom) / 2, 8, g_th.dim, 1.2f);
        Hit(r, HT_FMT, b.id);
        x += b.w + 2;
        if (b.id == FB_LIST || b.id == FB_LINK) { Line(x + 2, y + 8, x + 2, y + h - 8, g_th.border); x += 6; }
    }
}

static void FormatBarClick(int id, const RectF& r) {
    MenuModel m;
    switch (id) {
    case FB_DIALECT: m.items = DialectItems(Cur()); break;
    case FB_HEADING: m.items = HeadingItems(); break;
    case FB_LIST: m.items = ListItems(); break;
    case FB_TABLE: m.items = TableItems(); break;
    case FB_INSERT: m.items = InsertItems(); break;
    case FB_BOLD: RunCommand(C_FMT_BOLD); return;
    case FB_ITALIC: RunCommand(C_FMT_ITALIC); return;
    case FB_STRIKE: RunCommand(C_FMT_STRIKE); return;
    case FB_LINK: RunCommand(C_FMT_LINK); return;
    case FB_CLEAR: RunCommand(C_FMT_CLEAR); return;
    }
    TrackAndRun(m, r.left, r.bottom + 2);
}

static void PaintMenuBar(float W) {
    RectF bar = RF(0, CAP_H, W, MENU_H);
    FillR(bar, g_th.surface);
    float x = 8;
    IDWriteTextFormat* f = UiFmt(13);
    for (int i = 0; i < 3; ++i) {
        float w = TextW(kMenuNames[i], f) + 22;
        RectF r = RF(x, CAP_H + 4, w, MENU_H - 8);
        HoverFill(r, HT_MENU, i, 5);
        Text(kMenuNames[i], r, f, g_th.text, 1);
        Hit(r, HT_MENU, i);
        x += w + 2;
    }
    Doc* d = Cur();
    float rx = W - 12;
    struct Tool { Icon i; int cmd; bool on; bool show; };
    Tool tools[5] = {
        { I_GEAR, C_SETTINGS, s_settingsOpen, true },
        { I_SPARKLE, C_AIPANE, g_aiPane, g_set.online },
        { I_HEX, C_HEX, d && d->hex, d && !d->binaryOnly && !d->loading },
        { I_TABLE, C_GRID, d && d->grid && !d->hex, d && !d->loading && !d->binaryOnly && !d->large && (d->lang == np::L_CSV || d->grid) },
        { I_EYE, C_MDPREVIEW, d && d->mdPreview, d && PreviewSupported(d) && (d->lang != np::L_TEXT || d->mdPreview) },
    };
    for (auto& t : tools) {
        if (!t.show) continue;
        RectF r = RF(rx - 36, CAP_H + 4, 36, MENU_H - 8);
        if (t.on) FillRR(r, 5, g_th.press);
        HoverFill(r, HT_TOOL, t.cmd, 5);
        DrawIcon(t.i, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 16 * g_textScale, t.i == I_SPARKLE ? g_th.accent : g_th.text, 1.25f);
        Hit(r, HT_TOOL, t.cmd);
        rx -= 38;
    }
    PaintFormatBar(x + 16, rx - 16);
    Line(0, CAP_H + MENU_H - 0.5f, W, CAP_H + MENU_H - 0.5f, g_th.border);
}

static void PaintStatus(float W, float H) {
    RectF bar = RF(0, H - STATUS_H, W, STATUS_H);
    FillR(bar, BarFill());
    if (!g_mica) Line(0, bar.top + 0.5f, W, bar.top + 0.5f, g_th.border);
    Doc* d = Cur(); if (!d) return;
    IDWriteTextFormat* f = UiFmt(12);
    float x = 14;
    auto seg = [&](const std::wstring& s, int id, bool left, float& pos) {
        float w = TextW(s, f) + 24;
        RectF r = left ? RF(pos, bar.top + 2, w, STATUS_H - 4) : RF(pos - w, bar.top + 2, w, STATUS_H - 4);
        if (id) { HoverFill(r, HT_STATUS, id, 4); Hit(r, HT_STATUS, id); }
        Text(s, r, f, g_th.dim, 1);
        if (left) { pos += w; Line(pos, bar.top + 7, pos, bar.bottom - 7, g_th.border); pos += 1; }
        else { pos -= w; Line(pos, bar.top + 7, pos, bar.bottom - 7, g_th.border); pos -= 1; }
    };
    wchar_t b[128];
    if (d->loading) {
        swprintf(b, 128, L"Opening %.1f MB…", d->loadMB); seg(b, 0, true, x);
    } else if (d->grid && !d->hex) {
        std::wstring stats; std::wstring cell = GridStatus(d, stats);
        seg(cell, 1, true, x);
        if (!stats.empty()) seg(stats, 0, true, x);
        swprintf(b, 128, L"%d rows \u00D7 %d columns", std::max(0, d->table.Rows() - (d->gHeader ? 1 : 0)), d->table.Cols()); seg(b, 0, true, x);
        static const wchar_t* dn[] = { L"Comma", L"Semicolon", L"Tab", L"Pipe" };
        int di = d->delim == L';' ? 1 : d->delim == L'\t' ? 2 : d->delim == L'|' ? 3 : 0;
        seg(dn[di], 0, true, x);
    } else if (d->hex) {
        swprintf(b, 128, L"Offset 0x%zX (%zu)", d->hexCaret, d->hexCaret); seg(b, 1, true, x);
        size_t n = d->hexCaret > d->hexAnchor ? d->hexCaret - d->hexAnchor : d->hexAnchor - d->hexCaret;
        if (n) { swprintf(b, 128, L"%zu bytes selected", n); seg(b, 0, true, x); }
        swprintf(b, 128, L"%zu bytes", d->bytes.size()); seg(b, 0, true, x);
        seg(d->hexInsert ? L"INS" : L"OVR", 0, true, x);
    } else {
        int col = 1; const std::wstring& ln = d->text.Line(d->caret.line);
        for (int i = 0; i < d->caret.col && i < (int)ln.size(); ++i) col = (ln[i] == L'\t') ? ((col - 1) / g_set.tabSize + 1) * g_set.tabSize + 1 : col + 1;
        swprintf(b, 128, L"Ln %d, Col %d", d->caret.line + 1, col); seg(b, 1, true, x);
        size_t total = d->text.Length();
        if (EditorHasSel(d)) { size_t sel = (size_t)std::llabs((long long)d->text.Offset(d->caret) - (long long)d->text.Offset(d->anchor)); swprintf(b, 128, L"%zu of %zu characters", sel, total); }
        else swprintf(b, 128, L"%zu characters", total);
        seg(b, 0, true, x);
        seg(np::LangName(d->lang), 2, true, x);
        if ((d->lang == np::L_MD || d->lang == np::L_TEXT) && !d->large) seg(d->fmtView ? L"Formatted" : L"Source", 6, true, x);
        if (d->large) seg(L"Large file mode", 0, true, x);
        if (d->overwrite) seg(L"OVR", 0, true, x);
    }
    if (GetTickCount64() < s_statusUntil) { float sw = std::max(W / 2 - x, W - x - 330); if (sw > 60) Text(s_status, RF(x + 8, bar.top, sw, STATUS_H), f, g_th.accent); }
    float rx = W - 14;
    seg(np::EncName(d->text.enc), 5, false, rx);
    seg(d->binaryOnly ? L"Binary" : np::EolName(d->text.eol), 4, false, rx);
    swprintf(b, 128, L"%d%%", g_set.zoom); seg(b, 3, false, rx);
}

static void PaintFind() {
    Doc* d = Cur();
    if (!s_findOpen || !d || d->hex) { ShowWindow(s_findEdit, SW_HIDE); ShowWindow(s_replEdit, SW_HIDE); return; }
    FindRefresh();
    float w = std::min(520.f, RW(s_editorArea) - 24), rowH = 36;
    float h = s_replaceOpen ? rowH * 2 + 14 : rowH + 10;
    s_findCard = RF(s_editorArea.right - w - 20, s_editorArea.top + 8, w, h);
    // shadow
    for (int i = 4; i >= 1; --i) FillRR(RF(s_findCard.left - i, s_findCard.top - i + 2, RW(s_findCard) + 2 * i, RH(s_findCard) + 2 * i), 8 + i, Hex(0x000000, g_th.dark ? 0.08f : 0.03f));
    FillRR(s_findCard, 8, g_th.surface); StrokeRR(s_findCard, 8, g_th.border);
    Hit(s_findCard, HT_FIND, 0);
    float x = s_findCard.left + 6, y = s_findCard.top + 5;
    RectF tg = RF(x, y + 4, 24, rowH - 8);
    HoverFill(tg, HT_FIND, 1); DrawIcon(s_replaceOpen ? I_CHEV_DOWN : I_CHEV_RIGHT, (tg.left + tg.right) / 2, (tg.top + tg.bottom) / 2, 11, g_th.text);
    Hit(tg, HT_FIND, 1);
    x += 28;
    float btnArea = 3 * 28 + 70 + 3 * 28 + 8;
    float boxW = w - (x - s_findCard.left) - btnArea;
    s_findBox = RF(x, y + 2, boxW, rowH - 4);
    bool focusF = GetFocus() == s_findEdit;
    FillRR(s_findBox, 4, g_th.editor); StrokeRR(s_findBox, 4, focusF ? g_th.accent : g_th.border, focusF ? 1.5f : 1.f);
    float bx = s_findBox.right + 4;
    // option toggles
    const wchar_t* opt[3] = { L"Aa", L"ab", L".*" }; bool on[3] = { s_optCase, s_optWord, s_optRegex };
    for (int i = 0; i < 3; ++i) {
        RectF r = RF(bx, y + 5, 26, rowH - 10);
        if (on[i]) { FillRR(r, 4, Alpha(g_th.accent, 0.25f)); StrokeRR(r, 4, g_th.accent); }
        HoverFill(r, HT_FIND, 10 + i);
        Text(opt[i], r, Fmt(g_monoFont.c_str(), 12.f, i == 0 ? DWRITE_FONT_WEIGHT_NORMAL : DWRITE_FONT_WEIGHT_SEMI_BOLD), on[i] ? g_th.text : g_th.dim, 1);
        if (i == 1) Line(r.left + 7, r.bottom - 6, r.right - 7, r.bottom - 6, on[i] ? g_th.text : g_th.dim);
        Hit(r, HT_FIND, 10 + i);
        bx += 28;
    }
    std::wstring cnt = s_findMsg;
    if (cnt.empty() && !s_findText.empty()) {
        if (g_findMatches.empty()) cnt = L"No results";
        else {
            size_t a = d->text.Offset(std::min(d->caret, d->anchor)); int idx = 0;
            for (size_t i = 0; i < g_findMatches.size(); ++i) if (g_findMatches[i].off == a) { idx = (int)i + 1; break; }
            wchar_t b[64];
            const wchar_t* plus = g_findMatches.size() >= 100000 ? L"+" : L"";
            if (idx) swprintf(b, 64, L"%d of %zu%ls", idx, g_findMatches.size(), plus); else swprintf(b, 64, L"%zu%ls found", g_findMatches.size(), plus);
            cnt = b;
        }
    }
    Text(cnt, RF(bx + 2, y, 68, rowH), UiFmt(11.5f), s_findMsg.empty() ? g_th.dim : g_th.danger, 1);
    bx += 70;
    Icon ic[3] = { I_ARROW_UP, I_ARROW_DOWN, I_CLOSE };
    for (int i = 0; i < 3; ++i) {
        RectF r = RF(bx, y + 4, 26, rowH - 8);
        HoverFill(r, HT_FIND, 20 + i);
        DrawIcon(ic[i], (r.left + r.right) / 2, (r.top + r.bottom) / 2, i == 2 ? 10 : 13, g_th.text, 1.2f);
        Hit(r, HT_FIND, 20 + i);
        bx += 28;
    }
    auto place = [](HWND e, RectF r) {
        int px = (int)((r.left + 8) * g_scale), py = (int)((r.top + (RH(r) - 18) / 2) * g_scale);
        SetWindowPos(e, HWND_TOP, px, py, (int)((RW(r) - 14) * g_scale), (int)(18 * g_scale), SWP_SHOWWINDOW | SWP_NOACTIVATE);
    };
    place(s_findEdit, s_findBox);
    if (s_replaceOpen) {
        y += rowH + 2;
        s_replBox = RF(s_findBox.left, y + 2, boxW, rowH - 4);
        bool focusR = GetFocus() == s_replEdit;
        FillRR(s_replBox, 4, g_th.editor); StrokeRR(s_replBox, 4, focusR ? g_th.accent : g_th.border, focusR ? 1.5f : 1.f);
        RectF r1 = RF(s_replBox.right + 4, y + 3, 92, rowH - 6), r2 = RF(r1.right + 4, y + 3, 92, rowH - 6);
        FillRR(r1, 4, g_th.editor); StrokeRR(r1, 4, g_th.border); HoverFill(r1, HT_FIND, 30);
        FillRR(r2, 4, g_th.editor); StrokeRR(r2, 4, g_th.border); HoverFill(r2, HT_FIND, 31);
        Text(L"Replace", r1, UiFmt(12), g_th.text, 1); Text(L"Replace all", r2, UiFmt(12), g_th.text, 1);
        Hit(r1, HT_FIND, 30); Hit(r2, HT_FIND, 31);
        place(s_replEdit, s_replBox);
        if (GetWindowTextLengthW(s_replEdit) == 0 && !focusR) Text(L"Replace", RF(s_replBox.left + 8, s_replBox.top, RW(s_replBox), RH(s_replBox)), UiFmt(12.5f), g_th.faint);
    } else ShowWindow(s_replEdit, SW_HIDE);
    if (GetWindowTextLengthW(s_findEdit) == 0 && !focusF) Text(L"Find", RF(s_findBox.left + 8, s_findBox.top, RW(s_findBox), RH(s_findBox)), UiFmt(12.5f), g_th.faint);
}

// settings page --------------------------------------------------------------
struct SRow { int type; const wchar_t* label; const wchar_t* desc; int id; Icon icon; };   // type 0 header, 1 toggle, 2 choice, 3 action, 4 text
enum { S_THEME = 1, S_FONT, S_WRAP, S_TAB, S_SPACES, S_INDENT, S_LINES, S_FOLD, S_SYNTAX, S_SPELL, S_STATUS, S_SESSION, S_AUTOSAVE,
       S_PROVIDER, S_ENDPOINT, S_MODEL, S_KEY, S_ABOUT, S_TRANSP, S_MENUS, S_FMTBAR, S_DIALECT, S_ONLINE, S_CSVTABLE, S_FORMATTED, S_PATTERN, S_HICONTRAST };
static const SRow kRows[] = {
    { 0, L"Appearance", nullptr, 0, I_NONE },
    { 2, L"App theme", L"Select which app theme to display", S_THEME, I_THEME },
    { 1, L"High contrast", L"Stronger menu highlights, selection, borders and secondary text (on automatically in Windows high-contrast mode)", S_HICONTRAST, I_EYE },
    { 2, L"Editor background", L"Pattern behind the text (Auto: hearts on Pink, plain otherwise)", S_PATTERN, I_IMAGE },
    { 1, L"Transparency effects", L"Frosted-glass menus; Mica tab strip on Windows 11 22H2 or later", S_TRANSP, I_TRANSPARENCY },
    { 2, L"Menu style", L"Modern flyout menus or classic Windows menus", S_MENUS, I_MENUSTYLE },
    { 0, L"Text formatting", nullptr, 0, I_NONE },
    { 3, L"Font", L"Family, style and size of the editor font", S_FONT, I_FONT },
    { 1, L"Word wrap", L"Fit text within window by default", S_WRAP, I_WRAP },
    { 1, L"Formatting toolbar", L"Headings, lists, bold, links, images, tables and charts", S_FMTBAR, I_TOOLBAR },
    { 1, L"Formatted view", L"New tabs and Markdown files show formatting instead of Markdown symbols", S_FORMATTED, I_MARKDOWN },
    { 2, L"Tab size", nullptr, S_TAB, I_INDENT },
    { 1, L"Insert spaces", L"Pressing Tab inserts spaces instead of a tab character", S_SPACES, I_INDENT },
    { 1, L"Auto indent", L"New lines keep the indentation of the previous line", S_INDENT, I_INDENT },
    { 0, L"Editor", nullptr, 0, I_NONE },
    { 1, L"Line numbers", nullptr, S_LINES, I_LINENUM },
    { 1, L"Code folding", L"Collapse blocks by braces, indentation or headings", S_FOLD, I_FOLD },
    { 1, L"Syntax highlighting", L"Colour code based on the file type", S_SYNTAX, I_CODE },
    { 1, L"Spell check", L"Uses the Windows spell checker (offline); in code only comments and strings", S_SPELL, I_SPELL },
    { 1, L"Status bar", nullptr, S_STATUS, I_STATUSBAR },
    { 1, L"Open CSV files as a table", L"Show .csv, .tsv and .tab files in the table view (Ctrl+Shift+T switches to text)", S_CSVTABLE, I_TABLE },
    { 0, L"Opening Notepad", nullptr, 0, I_NONE },
    { 1, L"Continue previous session", L"Restore tabs and unsaved changes when ModernNotepad starts", S_SESSION, I_SESSION },
    { 1, L"Auto-save to disk", L"Periodically write changes of files that already have a path", S_AUTOSAVE, I_SAVE },
    { 0, L"Online features", nullptr, 0, I_NONE },
    { 1, L"Allow online features", L"Off by default. Nothing ever leaves this PC unless this is on and you invoke AI assist.", S_ONLINE, I_GLOBE },
    { 2, L"AI provider", L"Anthropic Messages API or any OpenAI-compatible endpoint (incl. local servers)", S_PROVIDER, I_SPARKLE },
    { 3, L"Endpoint", nullptr, S_ENDPOINT, I_SERVER },
    { 3, L"Model", nullptr, S_MODEL, I_MODEL },
    { 3, L"API key", L"Stored encrypted for your Windows account (DPAPI)", S_KEY, I_KEY },
};
static bool SToggle(int id) {
    switch (id) {
    case S_WRAP: return g_set.wordWrap; case S_SPACES: return g_set.insertSpaces; case S_INDENT: return g_set.autoIndent;
    case S_LINES: return g_set.lineNumbers; case S_FOLD: return g_set.folding; case S_SYNTAX: return g_set.syntax; case S_SPELL: return g_set.spell;
    case S_STATUS: return g_set.statusBar; case S_SESSION: return g_set.restoreSession; case S_AUTOSAVE: return g_set.autoSaveDisk;
    case S_TRANSP: return g_set.transparency; case S_FMTBAR: return g_set.formatBar; case S_ONLINE: return g_set.online; case S_CSVTABLE: return g_set.csvAsTable; case S_FORMATTED: return g_set.formatted; case S_HICONTRAST: return g_set.highContrast;
    }
    return false;
}
static void SFlip(int id) {
    switch (id) {
    case S_WRAP: RunCommand(C_WORDWRAP); return; case S_FOLD: RunCommand(C_FOLDING); return;
    case S_SYNTAX: RunCommand(C_SYNTAX); return; case S_SPELL: RunCommand(C_SPELL); return;
    case S_TRANSP: RunCommand(C_TRANSPARENCY); return; case S_ONLINE: RunCommand(C_ONLINE); return;
    case S_FMTBAR: g_set.formatBar = !g_set.formatBar; break;
    case S_CSVTABLE: g_set.csvAsTable = !g_set.csvAsTable; break;
    case S_FORMATTED: g_set.formatted = !g_set.formatted; break;
    case S_HICONTRAST: g_set.highContrast = !g_set.highContrast; BuildTheme(); break;
    case S_SPACES: g_set.insertSpaces = !g_set.insertSpaces; break; case S_INDENT: g_set.autoIndent = !g_set.autoIndent; break;
    case S_LINES: g_set.lineNumbers = !g_set.lineNumbers; break; case S_STATUS: g_set.statusBar = !g_set.statusBar; break;
    case S_SESSION: g_set.restoreSession = !g_set.restoreSession; break; case S_AUTOSAVE: g_set.autoSaveDisk = !g_set.autoSaveDisk; break;
    }
    SettingsSave(); Invalidate();
}
static bool SEnabled(int id) {
    if (id == S_PROVIDER || id == S_ENDPOINT || id == S_MODEL || id == S_KEY) return g_set.online;
    if (id == S_SPELL) return SpellAvailable();
    return true;
}
static std::vector<std::wstring> SChoices(int id) {
    if (id == S_THEME) return { L"Light", L"Dark", L"Pink", L"System" };
    if (id == S_PATTERN) return { L"Auto", L"None", L"Dots", L"Hearts", L"Stars", L"Grid" };
    if (id == S_TAB) return { L"2", L"4", L"8" };
    if (id == S_PROVIDER) return { L"Anthropic", L"OpenAI-compatible" };
    if (id == S_MENUS) return { L"Modern", L"Classic" };
    if (id == S_DIALECT) return { L"Markdown", L"BBCode", L"HTML" };
    return {};
}
static int SChoice(int id) {
    if (id == S_THEME) return g_set.theme == 1 ? 0 : g_set.theme == 2 ? 1 : g_set.theme == 3 ? 2 : 3;
    if (id == S_PATTERN) return g_set.bgPattern;
    if (id == S_TAB) return g_set.tabSize == 2 ? 0 : g_set.tabSize == 8 ? 2 : 1;
    if (id == S_PROVIDER) return g_set.aiProvider;
    if (id == S_MENUS) return g_set.modernMenus ? 0 : 1;
    if (id == S_DIALECT) return g_set.defaultDialect;
    return 0;
}
static void SSetChoice(int id, int v) {
    if (id == S_THEME) { g_set.theme = v == 0 ? 1 : v == 1 ? 2 : v == 2 ? 3 : 0; BuildTheme(); ApplyBackdrop(); }
    if (id == S_PATTERN) g_set.bgPattern = v;
    if (id == S_TAB) { g_set.tabSize = v == 0 ? 2 : v == 2 ? 8 : 4; EditorClearCache(); }
    if (id == S_MENUS) g_set.modernMenus = v == 0;
    if (id == S_DIALECT) { g_set.defaultDialect = v; MdClearCache(); }
    if (id == S_PROVIDER && v != g_set.aiProvider) {
        g_set.aiProvider = v;
        if (v == 0) { g_set.aiEndpoint = L"https://api.anthropic.com/v1/messages"; g_set.aiModel = L"claude-sonnet-5-5"; }
        else { g_set.aiEndpoint = L"http://localhost:11434/v1/chat/completions"; g_set.aiModel = L"llama3.1"; }
    }
    SettingsSave(); Invalidate();
}
static std::wstring SValue(int id) {
    wchar_t b[160];
    switch (id) {
    case S_FONT: swprintf(b, 160, L"%ls, %g pt%ls%ls", g_set.fontFace.c_str(), g_set.fontSize, g_set.fontBold ? L", bold" : L"", g_set.fontItalic ? L", italic" : L""); return b;
    case S_ENDPOINT: return g_set.aiEndpoint;
    case S_MODEL: return g_set.aiModel;
    case S_KEY: return g_set.aiKey.empty() ? L"Not set" : L"••••••" + g_set.aiKey.substr(g_set.aiKey.size() > 4 ? g_set.aiKey.size() - 4 : 0);
    }
    return L"";
}
static void PaintSettings(const RectF& area) {
    FillR(area, g_mica ? D2D1::ColorF(0, 0, 0, 0) : g_th.chrome);
    g_rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    float ts = g_textScale;
    float cw = std::min(980.f, RW(area) - 48), x = area.left + 32;
    // credits + Glyph Invaders: a sticky column on the right when there is room, else a card after the settings
    float avail = RW(area) - 64;
    bool side = avail >= 1000;
    float sideW = std::max(330.f, std::min(460.f, avail * 0.36f));
    if (side) cw = std::min(980.f, avail - 24 - sideW);
    float y = area.top + 14 - s_settingsScroll;
    RectF back = RF(x - 6, y, 32, 32);
    HoverFill(back, HT_SETTINGS, 1000, 5);
    DrawIcon(I_BACK, (back.left + back.right) / 2, (back.top + back.bottom) / 2, 14, g_th.text);
    Hit(back, HT_SETTINGS, 1000);
    y += 38;
    Text(L"Settings", RF(x, y, cw, 44 * ts), UiFmt(28, true), g_th.text);
    y += 58 * ts;
    Color card = g_mica ? (g_th.dark ? Hex(0xFFFFFF, 0.055f) : Hex(0xFFFFFF, 0.72f)) : g_th.surface;
    for (auto& row : kRows) {
        if (row.type == 0) { y += 8; Text(row.label, RF(x, y, cw, 24 * ts), UiFmt(14, true), g_th.text); y += 30 * ts; continue; }
        bool en = SEnabled(row.id);
        Color tc = en ? g_th.text : g_th.faint, dc = en ? g_th.dim : g_th.faint;
        float h = (row.desc ? 64.f : 52.f) * ts;
        RectF rc = RF(x, y, cw, h);
        FillRR(rc, 6, card); StrokeRR(rc, 6, g_th.border);
        if (row.icon != I_NONE) DrawIcon(row.icon, x + 26, y + h / 2, 17 * ts, tc, 1.2f);
        float lx = x + 54;
        float ty = row.desc ? y + 11 * ts : y;
        Text(row.label, RF(lx, ty, cw * 0.55f, row.desc ? 20 * ts : h), UiFmt(13.5f), tc);
        if (row.desc) Text(row.desc, RF(lx, y + 33 * ts, cw * 0.62f, 18 * ts), UiFmt(11.5f), dc);
        float rr = rc.right - 18;
        if (row.type == 1) {
            bool on = SToggle(row.id) && en;
            RectF sw = RF(rr - 40, y + h / 2 - 10, 40, 20);
            if (on) FillRR(sw, 10, g_th.accent); else StrokeRR(sw, 10, dc, 1.2f);
            float kx = on ? sw.right - 10 : sw.left + 10;
            float kr = IsHot(HT_SETTINGS, row.id) && en ? 7.f : 6.f;
            g_rt->FillEllipse(D2D1::Ellipse({ kx, y + h / 2 }, on ? kr : kr - 1, on ? kr : kr - 1), Brush(on ? g_th.accentText : dc));
            Text(on ? L"On" : L"Off", RF(sw.left - 46, y, 40, h), UiFmt(12.5f), tc, 2);
            if (en) Hit(rc, HT_SETTINGS, row.id);
        } else if (row.type == 2) {
            auto ch = SChoices(row.id); int cur = SChoice(row.id);
            float segW = (row.id == S_PROVIDER ? 140.f : row.id == S_PATTERN ? 66.f : row.id == S_DIALECT ? 96.f : 82.f) * ts, tw = segW * ch.size();
            RectF g = RF(rr - tw, y + h / 2 - 15 * ts, tw, 30 * ts);
            FillRR(g, 6, g_mica ? Alpha(g_th.editor, 0.6f) : g_th.editor); StrokeRR(g, 6, g_th.border);
            for (int i = 0; i < (int)ch.size(); ++i) {
                RectF sg = RF(g.left + segW * i + 2, g.top + 2, segW - 4, RH(g) - 4);
                if (i == cur) FillRR(sg, 4, Alpha(g_th.accent, en ? 0.25f : 0.1f));
                else if (en) HoverFill(sg, HT_SETTINGS, row.id * 10 + i + 5000, 4);
                Text(ch[i], sg, UiFmt(12.5f, i == cur), tc, 1);
                if (en) Hit(sg, HT_SETTINGS, row.id * 10 + i + 5000);
            }
        } else if (row.type == 3) {
            if (en) HoverFill(rc, HT_SETTINGS, row.id, 6);
            Text(SValue(row.id), RF(rc.left + cw * 0.38f, y, cw * 0.62f - 46, h), UiFmt(12.5f), dc, 2);
            DrawIcon(I_CHEV_RIGHT, rr - 6, y + h / 2, 12, dc);
            if (en) Hit(rc, HT_SETTINGS, row.id);
        }
        y += h + 4;
    }
    if (side) {
        float sx = x + cw + 24, sw = std::min(sideW, area.right - 32 - sx);
        GamePaint(RF(sx, area.top + 14 + 38 + 58 * ts, sw, std::min(660.f, RH(area) - (14 + 38 + 58 * ts) - 20)));
    } else {
        y += 16; Text(L"About", RF(x, y, cw, 24 * ts), UiFmt(14, true), g_th.text); y += 30 * ts;
        GamePaint(RF(x, y, cw, 560)); y += 560;
    }
    s_settingsH = y + s_settingsScroll - area.top + 40;
    g_rt->PopAxisAlignedClip();
}
static void SettingsClick(int id) {
    if (id == 1000) { s_settingsOpen = false; Invalidate(); return; }
    if (id >= 5000) { SSetChoice((id - 5000) / 10, (id - 5000) % 10); return; }
    switch (id) {
    case S_FONT: ChooseEditorFont(); break;
    case S_ENDPOINT: Prompt(L"AI endpoint URL", g_set.aiEndpoint, SetEndpoint); break;
    case S_MODEL: Prompt(L"Model name", g_set.aiModel, SetModel); break;
    case S_KEY: Prompt(L"API key (leave empty to clear)", L"", SetKey, true); break;
    default: SFlip(id);
    }
}

static void PaintPrompt(float W, float H) {
    if (!s_prompt.on) return;
    FillR(RF(0, 0, W, H), Hex(0x000000, 0.35f));
    float cw = std::min(440.f, W - 40), ch = 150;
    s_prompt.card = RF((W - cw) / 2, (H - ch) / 2 - 40, cw, ch);
    FillRR(s_prompt.card, 8, g_th.surface); StrokeRR(s_prompt.card, 8, g_th.border);
    float x = s_prompt.card.left + 22, y = s_prompt.card.top + 16;
    Text(s_prompt.title, RF(x, y, cw - 44, 26), UiFmt(15, true), g_th.text);
    s_prompt.box = RF(x, y + 34, cw - 44, 32);
    bool f = GetFocus() == s_prompt.edit;
    FillRR(s_prompt.box, 4, g_th.editor); StrokeRR(s_prompt.box, 4, f ? g_th.accent : g_th.border, f ? 1.5f : 1.f);
    SetWindowPos(s_prompt.edit, HWND_TOP, (int)((s_prompt.box.left + 8) * g_scale), (int)((s_prompt.box.top + 7) * g_scale),
                 (int)((RW(s_prompt.box) - 16) * g_scale), (int)(18 * g_scale), SWP_SHOWWINDOW | SWP_NOACTIVATE);
    s_prompt.ok = RF(s_prompt.card.right - 22 - 200 - 8, s_prompt.card.bottom - 46, 100, 32);
    s_prompt.cancel = RF(s_prompt.card.right - 22 - 100, s_prompt.card.bottom - 46, 100, 32);
    FillRR(s_prompt.ok, 4, g_th.accent); Text(L"OK", s_prompt.ok, UiFmt(13), g_th.accentText, 1);
    FillRR(s_prompt.cancel, 4, g_th.editor); StrokeRR(s_prompt.cancel, 4, g_th.border); Text(L"Cancel", s_prompt.cancel, UiFmt(13), g_th.text, 1);
}

static void ComputeContent(float W, float H) {
    float top = CAP_H + MENU_H, bottom = H - (g_set.statusBar ? STATUS_H : 0);
    s_contentR = RF(0, top, W, bottom - top);
    float right = W;
    s_aiArea = {};
    if (g_aiPane) { float aw = std::min(360.f, W * 0.4f); s_aiArea = RF(W - aw, top, aw, bottom - top); right -= aw; }
    Doc* d = Cur();
    s_mdArea = {}; s_splitR = {};
    if (d && d->mdPreview && !d->hex) {
        float split = std::max(0.2f, std::min(0.8f, g_set.mdSplit)) * right;
        s_editorArea = RF(0, top, split - 3, bottom - top);
        s_splitR = RF(split - 3, top, 6, bottom - top);
        s_mdArea = RF(split + 3, top, right - split - 3, bottom - top);
    } else s_editorArea = RF(0, top, right, bottom - top);
}

static void Paint() {
    if (!g_rt) return;
    float W = ClientW(), H = ClientH();
    s_hits.clear();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(g_mica ? D2D1::ColorF(0, 0, 0, 0) : g_th.chrome);
    if (g_mica && g_th.pink) FillR(RF(0, 0, W, CAP_H), Alpha(g_th.chrome, 0.72f));   // blush tint over Mica
    ComputeContent(W, H);
    if (!s_settingsOpen && GameRunning()) GamePause();
    PaintTabs(W);
    PaintMenuBar(W);
    Doc* d = Cur();
    if (s_settingsOpen) {
        PaintSettings(s_contentR);
        ShowWindow(s_findEdit, SW_HIDE); ShowWindow(s_replEdit, SW_HIDE);
    } else if (d) {
        if (d->hex) HexPaint(d, s_editorArea); else if (d->grid) GridPaint(d, s_editorArea); else EditorPaint(d, s_editorArea);
        if (RW(s_mdArea) > 0) {
            MdPaint(d, s_mdArea);
            FillR(s_splitR, g_mica ? g_th.editor : g_th.chrome);
            Line((s_splitR.left + s_splitR.right) / 2, s_splitR.top, (s_splitR.left + s_splitR.right) / 2, s_splitR.bottom, g_th.border);
            Hit(s_splitR, HT_SPLIT);
        }
        PaintFind();
    }
    if (g_aiPane && !s_settingsOpen) AiPaint(s_aiArea);
    if (g_set.statusBar) PaintStatus(W, H);
    PaintPrompt(W, H);
    PaintForm(W, H);
    if (!g_mica && IsZoomed(g_hwnd) == 0) Line(0, 0.5f, W, 0.5f, Alpha(g_th.border, 0.6f));
    HRESULT hr = g_rt->EndDraw();
    UiaNotify();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { ResetDeviceResources(); EditorClearCache(); MdClearCache(); g_rt->Release(); g_rt = nullptr; }
}

static void CreateTarget() {
    if (g_rt) return;
    RECT rc; GetClientRect(g_hwnd, &rc);
    // premultiplied alpha: pixels left transparent show the Mica backdrop (frame is extended over the client)
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * g_scale, 96.f * g_scale);
    g_d2d->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt);
    if (g_rt) g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ResetDeviceResources(); EditorClearCache(); MdClearCache();
}

// Mica Alt (DWMSBT_TABBEDWINDOW) behind the tab strip, menu bar and status bar - the "blue to purple"
// wallpaper-tinted gradient Win11 Notepad shows while focused; DWM swaps it for a flat grey when inactive.
static void ApplyBackdrop() {
    g_mica = g_set.transparency && TransparencySupported();
    if (g_mica) {
        MARGINS mg{ -1, -1, -1, -1 }; DwmExtendFrameIntoClientArea(g_hwnd, &mg);
        DWORD bd = 4; DwmSetWindowAttribute(g_hwnd, 38, &bd, sizeof(bd));
    } else {
        DWORD bd = 1; DwmSetWindowAttribute(g_hwnd, 38, &bd, sizeof(bd));   // DWMSBT_NONE
        MARGINS mg{ 0, 0, 1, 0 }; DwmExtendFrameIntoClientArea(g_hwnd, &mg);
    }
    BOOL dk = g_th.dark; DwmSetWindowAttribute(g_hwnd, 20, &dk, sizeof(dk));
    SyncSysMenuStyle();
    Invalidate();
}

static void MakeEditFont() {
    if (s_editFont) DeleteObject(s_editFont);
    s_editFont = CreateFontW(-(int)std::lround(13.5f * g_scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_uiFont.c_str());
    if (s_findEdit) SendMessageW(s_findEdit, WM_SETFONT, (WPARAM)s_editFont, TRUE);
    if (s_replEdit) SendMessageW(s_replEdit, WM_SETFONT, (WPARAM)s_editFont, TRUE);
    if (s_prompt.edit) SendMessageW(s_prompt.edit, WM_SETFONT, (WPARAM)s_editFont, TRUE);
}

// ------------------------------------------------------------------ input
static HitItem* HitAt(float x, float y) {
    for (auto it = s_hits.rbegin(); it != s_hits.rend(); ++it) if (In(it->r, x, y)) return &*it;
    return nullptr;
}
static bool Accelerator(WPARAM vk) {
    bool c = GetKeyState(VK_CONTROL) < 0, s = GetKeyState(VK_SHIFT) < 0, a = GetKeyState(VK_MENU) < 0;
    Doc* d = Cur();
    if (c && !a) {
        switch (vk) {
        case 'N': RunCommand(s ? C_NEWWIN : C_NEW); return true;
        case 'O': RunCommand(C_OPEN); return true;
        case 'S': RunCommand(s ? C_SAVEAS : C_SAVE); return true;
        case 'P': RunCommand(C_PRINT); return true;
        case 'W': RunCommand(s ? C_CLOSEWIN : C_CLOSETAB); return true;
        case 'Z': RunCommand(s ? C_REDO : C_UNDO); return true;
        case 'Y': RunCommand(C_REDO); return true;
        case 'X': RunCommand(C_CUT); return true;
        case 'C': RunCommand(C_COPY); return true;
        case 'V': RunCommand(C_PASTE); return true;
        case 'A': RunCommand(C_SELALL); return true;
        case 'F': RunCommand(s ? C_FMTVIEW : C_FIND); return true;
        case 'H': RunCommand(s ? C_HEX : C_REPLACE); return true;
        case 'T': if (s) { RunCommand(C_GRID); return true; } break;
        case 'G': RunCommand(C_GOTO); return true;
        case 'D': RunCommand(C_DUPLINE); return true;
        case 'U': RunCommand(s ? C_UPPER : C_LOWER); return true;
        case 'B': if (!s) { RunCommand(C_FMT_BOLD); return true; } break;
        case 'K': RunCommand(C_FMT_LINK); return true;
        case 'E': RunCommand(C_FMT_CODE); return true;
        case VK_SPACE: RunCommand(C_FMT_CLEAR); return true;
        case 'M': if (s) { RunCommand(C_MDPREVIEW); return true; } break;
        case 'I': RunCommand(s ? C_AIPANE : C_FMT_ITALIC); return true;
        case VK_OEM_2: case VK_DIVIDE: RunCommand(C_TOGGLECOMMENT); return true;
        case VK_OEM_4: if (s) { RunCommand(C_FOLDCUR); return true; } break;
        case VK_OEM_6: if (s) { RunCommand(C_UNFOLDCUR); return true; } break;
        case VK_OEM_PLUS: case VK_ADD: RunCommand(C_ZOOMIN); return true;
        case VK_OEM_MINUS: case VK_SUBTRACT: RunCommand(C_ZOOMOUT); return true;
        case '0': case VK_NUMPAD0: RunCommand(C_ZOOMRESET); return true;
        case VK_OEM_COMMA: RunCommand(C_SETTINGS); return true;
        case VK_TAB: if (!g_docs.empty()) SelectTab((g_cur + (s ? -1 : 1) + (int)g_docs.size()) % (int)g_docs.size()); return true;
        case VK_PRIOR: if (g_cur > 0) SelectTab(g_cur - 1); return true;
        case VK_NEXT: if (g_cur + 1 < (int)g_docs.size()) SelectTab(g_cur + 1); return true;
        }
        if (vk >= '1' && vk <= '9') { int i = vk == '9' ? (int)g_docs.size() - 1 : (int)(vk - '1'); SelectTab(i); return true; }
    }
    if (c && a && vk == 'S') { RunCommand(C_SAVEALL); return true; }
    if (c && a && vk >= '1' && vk <= '6') { RunCommand(C_FMT_H1 + (int)(vk - '1')); return true; }
    if (c && a && vk == '0') { RunCommand(C_FMT_PARA); return true; }
    if (a && !c) {
        if (vk == 'Z') { RunCommand(C_WORDWRAP); return true; }
        if (vk == 'F' || vk == 'E' || vk == 'V') {
            int i = vk == 'F' ? 0 : vk == 'E' ? 1 : 2;
            for (auto& h : s_hits) if (h.kind == HT_MENU && h.idx == i) { TrackAndRun(BuildMenu(i), h.r.left, h.r.bottom); return true; }
        }
    }
    if (vk == VK_F10 && !c && !a) { for (auto& h : s_hits) if (h.kind == HT_MENU && h.idx == 0) TrackAndRun(BuildMenu(0), h.r.left, h.r.bottom); return true; }
    if (vk == VK_RETURN && c && s && d && !d->hex && !d->grid) { RunCommand(C_EVAL); return true; }
    if (vk == VK_F3) { RunCommand(s ? C_FINDPREV : C_FINDNEXT); return true; }
    if (vk == VK_F5 && !c) { RunCommand(C_TIMEDATE); return true; }
    if (vk == VK_ESCAPE && s_settingsOpen) { s_settingsOpen = false; Invalidate(); return true; }
    if (vk == VK_ESCAPE && s_findOpen && d && !EditorHasSel(d)) { CloseFind(); return true; }
    if (vk == VK_APPS || (vk == VK_F10 && s)) { RectF cr = EditorCaretRect(d); POINT p{ (LONG)(cr.left * g_scale), (LONG)(cr.bottom * g_scale) }; ClientToScreen(g_hwnd, &p); ShowContextMenu(d, p.x, p.y); return true; }
    return false;
}

static int NcHit(HWND h, LPARAM lp) {
    POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(h, &p);
    float x = p.x / g_scale, y = p.y / g_scale, W = ClientW();
    int frame = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    if (!IsZoomed(h) && p.y < frame) { if (x < 12) return HTTOPLEFT; if (x > W - 12) return HTTOPRIGHT; return HTTOP; }
    if (y < CAP_H) {
        if (x >= W - CAPBTN_W) return HTCLOSE;
        if (x >= W - CAPBTN_W * 2) return HTMAXBUTTON;
        if (x >= W - CAPBTN_W * 3) return HTMINBUTTON;
        for (auto& it : s_hits) if ((it.kind == HT_TAB || it.kind == HT_NEWTAB || it.kind == HT_TABCLOSE) && In(it.r, x, y)) return HTCLIENT;
        return HTCAPTION;
    }
    return HTCLIENT;
}
static int CapKind(WPARAM ht) { return ht == HTMINBUTTON ? HT_MIN : ht == HTMAXBUTTON ? HT_MAX : ht == HTCLOSE ? HT_CLOSE : 0; }

static void TabContext(int i, float x, float y) {
    SelectTab(i);
    MenuModel m;
    bool p = !g_docs[i]->path.empty();
    m.items = { MI(C_CLOSETAB, L"Close tab", L"Ctrl+W", I_CLOSE), MI(C_TAB_CLOSEOTHERS, L"Close other tabs", L"", I_NONE, false, g_docs.size() > 1),
                MI(C_TAB_CLOSERIGHT, L"Close tabs to the right", L"", I_NONE, false, i + 1 < (int)g_docs.size()), MSep(),
                MI(C_TAB_COPYPATH, L"Copy path", L"", I_COPY, false, p), MI(C_TAB_OPENFOLDER, L"Open in File Explorer", L"", I_OPEN, false, p) };
    TrackAndRun(m, x, y);
}

static void MouseClient(UINT msg, WPARAM wp, float x, float y) {
    Doc* d = Cur();
    if (s_form.on) {
        if (msg == WM_LBUTTONDOWN) {
            if (In(s_form.ok, x, y)) FormClose(true);
            else if (In(s_form.cancel, x, y) || !In(s_form.card, x, y)) FormClose(false);
            else if (s_form.browse && In(s_form.brw, x, y)) FormBrowse();
            else for (auto& f : s_form.f) if (In(f.box, x, y)) SetFocus(f.edit);
        }
        if (msg == WM_MOUSEMOVE) SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return;
    }
    if (s_prompt.on) {
        if (msg == WM_LBUTTONDOWN) {
            if (In(s_prompt.ok, x, y)) PromptClose(true);
            else if (In(s_prompt.cancel, x, y) || !In(s_prompt.card, x, y)) PromptClose(false);
            else SetFocus(s_prompt.edit);
        }
        return;
    }
    if (s_settingsOpen && GameMouse(msg, x, y, wp)) return;
    if (msg == WM_MOUSEMOVE) {
        if (s_splitDrag) { float right = g_aiPane ? RW(s_contentR) - RW(s_aiArea) : RW(s_contentR); g_set.mdSplit = std::max(0.2f, std::min(0.8f, x / right)); Invalidate(); return; }
        if (s_dragTab >= 0 && (wp & MK_LBUTTON)) {
            float W = ClientW(), maxTabsW = W - CAPBTN_W * 3 - 40 - 40 - 48;
            float tw = std::max(64.f, std::min(240.f * std::min(g_textScale, 1.3f), maxTabsW / std::max<size_t>(1, g_docs.size())));
            s_dragTabX = std::max(40.f, std::min(40 + tw * (g_docs.size() - 1), x - tw / 2));
            int target = (int)((s_dragTabX + tw / 2 - 40) / tw);
            target = std::max(0, std::min((int)g_docs.size() - 1, target));
            if (target != s_dragTab) {
                auto moved = std::move(g_docs[s_dragTab]); g_docs.erase(g_docs.begin() + s_dragTab);
                g_docs.insert(g_docs.begin() + target, std::move(moved));
                if (g_cur == s_dragTab) g_cur = target;
                s_dragTab = target;
            }
            Invalidate(); return;
        }
        HitItem* h = HitAt(x, y);
        int k = h ? h->kind : 0, i = h ? h->idx : -1;
        if (k != s_hotKind || i != s_hotIdx) { s_hotKind = k; s_hotIdx = i; Invalidate(); }
        bool overSplit = In(s_splitR, x, y);
        SetCursor(LoadCursor(nullptr, overSplit ? IDC_SIZEWE : (d && !s_settingsOpen && In(s_editorArea, x, y) && !(s_findOpen && In(s_findCard, x, y)) ? IDC_IBEAM : IDC_ARROW)));
        if (g_aiPane) AiMouse(msg, x, y);
        if (d && !s_settingsOpen) { if (d->hex) HexMouse(d, msg, x, y, wp); else if (d->grid) GridMouse(d, msg, x, y, wp); else EditorMouse(d, msg, x, y, wp); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, g_hwnd, 0 }; TrackMouseEvent(&t);
        return;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        SetFocus(g_hwnd);
        HitItem* h = HitAt(x, y);
        if (h) {
            s_pressKind = h->kind; s_pressIdx = h->idx;
            if (h->kind == HT_TAB) { if (h->idx != g_cur) SelectTab(h->idx); s_dragTab = h->idx; s_dragTabX = h->r.left; SetCapture(g_hwnd); Invalidate(); return; }
            if (h->kind == HT_SPLIT) { s_splitDrag = true; SetCapture(g_hwnd); return; }
            if (h->kind == HT_MENU) { int i = h->idx; RectF r = h->r; s_pressKind = 0; Invalidate(); TrackAndRun(BuildMenu(i), r.left, r.bottom); return; }
            if (h->kind == HT_FIND && h->idx == 0) {
                if (In(s_findBox, x, y)) SetFocus(s_findEdit);
                else if (s_replaceOpen && In(s_replBox, x, y)) SetFocus(s_replEdit);
                return;
            }
            Invalidate();
            if (h->kind != HT_FIND && h->kind != HT_SETTINGS && h->kind != HT_STATUS && h->kind != HT_TOOL && h->kind != HT_NEWTAB && h->kind != HT_TABCLOSE && h->kind != HT_FMT) {}
            else return;
        }
        if (g_aiPane && In(s_aiArea, x, y)) { AiMouse(msg, x, y); return; }
        if (!d || s_settingsOpen) return;
        if (RW(s_mdArea) > 0 && In(s_mdArea, x, y)) { MdClick(d, x, y); return; }
        if (d->hex) HexMouse(d, msg, x, y, wp); else if (d->grid) GridMouse(d, msg, x, y, wp); else EditorMouse(d, msg, x, y, wp);
        return;
    }
    if (msg == WM_LBUTTONUP) {
        if (s_splitDrag) { s_splitDrag = false; ReleaseCapture(); SettingsSave(); return; }
        if (s_dragTab >= 0) { s_dragTab = -1; ReleaseCapture(); Invalidate(); }
        HitItem* h = HitAt(x, y);
        int pk = s_pressKind, pi = s_pressIdx;
        s_pressKind = 0; s_pressIdx = -1;
        if (h && h->kind == pk && h->idx == pi) {
            switch (pk) {
            case HT_TABCLOSE: CloseTab(pi); break;
            case HT_NEWTAB: RunCommand(C_NEW); break;
            case HT_TOOL: RunCommand(pi); break;
            case HT_FMT: { RectF r = h->r; FormatBarClick(pi, r); break; }
            case HT_SETTINGS: SettingsClick(pi); break;
            case HT_STATUS: {
                RectF r = h->r;
                if (pi == 1) RunCommand(C_GOTO);
                else if (pi == 6) RunCommand(C_FMTVIEW);
                else if (pi == 2) { MenuModel m; m.items = LangItems(d); TrackAndRun(m, r.left, r.top - 2, MF_ANCHOR_ABOVE); }
                else if (pi == 3) { MenuModel m; int zs[] = { 50, 75, 100, 125, 150, 200, 300 };
                                    for (int z : zs) m.items.push_back(MI(9100 + z, std::to_wstring(z) + L"%", L"", I_NONE, g_set.zoom == z));
                                    POINT p{ (LONG)(r.left * g_scale), (LONG)(r.top * g_scale) }; ClientToScreen(g_hwnd, &p);
                                    int c = RunMenu(m, p.x, p.y, MF_ANCHOR_ABOVE); if (c) SetZoom(c - 9100); }
                else if (pi == 4) { if (d && !d->binaryOnly) { MenuModel m; m.items = EolItems(d); TrackAndRun(m, r.left, r.top - 2, MF_ANCHOR_ABOVE); } }
                else if (pi == 5) { MenuModel m; m.items = { MSub(L"Save with encoding", I_ENCODING, EncodingItems(d, false)), MSub(L"Reopen with encoding", I_ENCODING, EncodingItems(d, true)) };
                                    TrackAndRun(m, r.left, r.top - 2, MF_ANCHOR_ABOVE); }
                break; }
            case HT_FIND:
                switch (pi) {
                case 1: s_replaceOpen = !s_replaceOpen; SetFocus(s_replaceOpen ? s_replEdit : s_findEdit); break;
                case 10: s_optCase = !s_optCase; FindRefresh(true); break;
                case 11: s_optWord = !s_optWord; FindRefresh(true); break;
                case 12: s_optRegex = !s_optRegex; FindRefresh(true); break;
                case 20: FindNext(false); break;
                case 21: FindNext(true); break;
                case 22: CloseFind(); break;
                case 30: ReplaceOne(); break;
                case 31: ReplaceAll(); break;
                }
                break;
            }
            Invalidate();
            return;
        }
        if (g_aiPane && In(s_aiArea, x, y)) { AiMouse(msg, x, y); return; }
        if (d && !s_settingsOpen) { if (d->hex) HexMouse(d, msg, x, y, wp); else if (d->grid) GridMouse(d, msg, x, y, wp); else EditorMouse(d, msg, x, y, wp); }
        Invalidate();
        return;
    }
    if (msg == WM_MBUTTONUP) { HitItem* h = HitAt(x, y); if (h && (h->kind == HT_TAB || h->kind == HT_TABCLOSE)) CloseTab(h->idx); return; }
    if (msg == WM_RBUTTONUP) {
        HitItem* h = HitAt(x, y);
        if (h && h->kind == HT_TAB) { TabContext(h->idx, x, y); return; }
        if (d && !s_settingsOpen && In(s_editorArea, x, y)) {
            { POINT q{ (LONG)(x * g_scale), (LONG)(y * g_scale) }; ClientToScreen(g_hwnd, &q); if (!d->grid && !d->hex && LiveContextMenu(d, x, y, q.x, q.y)) return; }
            if (d->grid && !d->hex) GridMouse(d, WM_RBUTTONDOWN, x, y, wp); else if (!d->hex) EditorMouse(d, WM_RBUTTONDOWN, x, y, wp);
            POINT p{ (LONG)(x * g_scale), (LONG)(y * g_scale) }; ClientToScreen(g_hwnd, &p);
            ShowContextMenu(d, p.x, p.y);
        }
    }
}

static std::wstring s_uiaTestOut;
static void OpenCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"/new")) { s_noSession = true; continue; }
        if (!_wcsicmp(argv[i], L"/uiaselftest") && i + 1 < argc) { s_uiaTestOut = argv[++i]; continue; }
        if (argv[i][0] == L'/' && wcslen(argv[i]) < 4) continue;
        OpenPath(argv[i]);
    }
    LocalFree(argv);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = h;
        using DpiFn = UINT(WINAPI*)(HWND);
        static auto gd = (DpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        g_scale = (gd ? gd(h) : 96) / 96.f;
        BuildTheme();
        ApplyBackdrop();
        DWORD corner = 2; DwmSetWindowAttribute(h, 33, &corner, sizeof(corner));
        MakeEditFont();
        s_findEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, h, (HMENU)(INT_PTR)9002, g_inst, nullptr);
        s_replEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, h, (HMENU)(INT_PTR)9003, g_inst, nullptr);
        for (HWND e : { s_findEdit, s_replEdit }) {
            SendMessageW(e, WM_SETFONT, (WPARAM)s_editFont, TRUE);
            s_editProcOrig = (WNDPROC)SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR)EditSubProc);
        }
        DragAcceptFiles(h, TRUE);
        SetTimer(h, 1, GetCaretBlinkTime(), nullptr);
        SetTimer(h, 2, 5000, nullptr);
        SetTimer(h, 3, 40, nullptr);
        SetTimer(h, 4, 400, nullptr);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        return 0; }
    case WM_NCCALCSIZE:
        if (wp) {
            auto* p = (NCCALCSIZE_PARAMS*)lp;
            int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER), fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            p->rgrc[0].left += fx; p->rgrc[0].right -= fx; p->rgrc[0].bottom -= fy;
            if (IsZoomed(h)) p->rgrc[0].top += fy;
            return 0;
        }
        break;
    case WM_NCHITTEST: { LRESULT r = DefWindowProcW(h, msg, wp, lp); if (r != HTCLIENT) return r; return NcHit(h, lp); }
    case WM_NCMOUSEMOVE: {
        int k = CapKind(wp);
        if (k != s_hotKind) { s_hotKind = k; s_hotIdx = 0; Invalidate(); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE | TME_NONCLIENT, h, 0 }; TrackMouseEvent(&t);
        break; }
    case WM_NCMOUSELEAVE: case WM_MOUSELEAVE: s_hotKind = 0; s_hotIdx = -1; Invalidate(); break;
    case WM_NCRBUTTONUP: if (wp == HTCAPTION && g_mica) { ShowSystemMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0; } break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU && lp == L' ' && g_mica) { RECT wr; GetWindowRect(h, &wr); ShowSystemMenu(wr.left + 8, wr.top + (int)(CAP_H * g_scale)); return 0; }
        break;
    case WM_NCLBUTTONDOWN: if (CapKind(wp)) { s_pressKind = CapKind(wp); s_pressIdx = 0; Invalidate(); return 0; } break;
    case WM_NCLBUTTONUP: {
        int k = CapKind(wp);
        if (k && k == s_pressKind) {
            s_pressKind = 0;
            if (k == HT_MIN) ShowWindow(h, SW_MINIMIZE);
            else if (k == HT_MAX) ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
            else PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        s_pressKind = 0; break; }
    case WM_SIZE:
        if (g_rt) { RECT rc; GetClientRect(h, &rc); g_rt->Resize(D2D1::SizeU(rc.right, rc.bottom)); }
        Invalidate(); return 0;
    case WM_DPICHANGED: {
        g_scale = HIWORD(wp) / 96.f;
        if (g_rt) { g_rt->Release(); g_rt = nullptr; }
        MakeEditFont();
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0; }
    case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize = { (LONG)(480 * g_scale), (LONG)(300 * g_scale) }; return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); CreateTarget(); Paint(); EndPaint(h, &ps); return 0; }
    case WM_SETTINGCHANGE:
        {
            float old = g_textScale; ReadTextScale(); UpdateMetrics();
            if (old != g_textScale) { EditorClearCache(); MdClearCache(); MakeEditFont(); }
            if (lp && !lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet")) { if (g_set.theme == 0) BuildTheme(); ApplyBackdrop(); }
            Invalidate();
        }
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED: BuildTheme(); Invalidate(); break;
    case WM_SETFOCUS: case WM_KILLFOCUS: g_caretOn = true; Invalidate(); break;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE) GamePause(); else PostMessageW(h, WM_APP + 14, 0, 0); Invalidate(); break;
    case WM_APP + 14: CheckExternalChange(); return 0;
    case WM_APP + 15: UiaSelfTest(s_uiaTestOut); return 0;
    case WM_TIMER:
        if (wp == 7) { GameTick(); return 0; }
        if (wp == 1) { g_caretOn = !g_caretOn; Doc* d = Cur(); if (d && GetFocus() == h) { if (d->grid && !d->hex) { } else { RectF r = d->hex ? s_editorArea : EditorCaretRect(d); RECT rr{ (LONG)((r.left - 4) * g_scale), (LONG)((r.top - 2) * g_scale), (LONG)((r.right + 40) * g_scale), (LONG)((r.bottom + 2) * g_scale) }; if (d->hex) Invalidate(); else InvalidateRect(h, &rr, FALSE); } } }
        else if (wp == 2) { if (s_sessionOwner && !s_noSession) SessionAutosaveTick(); }
        else if (wp == 3) EditorTick();
        else if (wp == 4) { Doc* d = Cur(); if (d && (d->loading || (d->foldsVer != d->text.Version() && g_set.folding) || (s_findOpen && s_findVer != d->text.Version()))) Invalidate(); if (GetTickCount64() < s_statusUntil + 500) Invalidate(); }
        else if (wp == 7) { KillTimer(h, 7); if (AiBusy()) Invalidate(); }
        return 0;
    case WM_APP_AI: AiOnResult(lp); return 0;
    case WM_APP_LOADED: OnLoaded(lp); return 0;
    case WM_APP_GRIDCOMMIT: if (GridEditing() && GetFocus() != nullptr) GridCommit(); return 0;
    case WM_APP_LIVECOMMIT: if (LiveEditing() && GetFocus() != nullptr) LiveCommitEdit(); return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == s_findEdit) { FindRefresh(); Doc* d = Cur();
            if (d && !g_findMatches.empty()) {   // incremental: jump to first match at/after selection start
                size_t a = d->text.Offset(std::min(d->caret, d->anchor)); np::Match m;
                if (g_searcher.Find(d->text, a, true, m)) { np::Pos pa = d->text.PosAt(m.off), pb = d->text.PosAt(m.off + m.len); EditorUnfoldFor(d, pa.line); EditorSelect(d, pa, pb); }
            }
            Invalidate(); }
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        Color t = g_th.text, b = (HWND)lp == s_prompt.edit ? g_th.editor : g_th.editor;
        SetTextColor(dc, RGB((int)(t.r * 255), (int)(t.g * 255), (int)(t.b * 255)));
        SetBkColor(dc, RGB((int)(b.r * 255), (int)(b.g * 255), (int)(b.b * 255)));
        static HBRUSH br = nullptr; static COLORREF last = 0xFFFFFFFF;
        COLORREF c = RGB((int)(b.r * 255), (int)(b.g * 255), (int)(b.b * 255));
        if (c != last) { if (br) DeleteObject(br); br = CreateSolidBrush(c); last = c; }
        return (LRESULT)br; }
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONDOWN: {
        if (msg == WM_MOUSEMOVE && s_hotKind >= HT_MIN && s_hotKind <= HT_CLOSE) { s_hotKind = 0; Invalidate(); }
        MouseClient(msg, wp, GET_X_LPARAM(lp) / g_scale, GET_Y_LPARAM(lp) / g_scale);
        return 0; }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &p);
        float x = p.x / g_scale, y = p.y / g_scale;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        Doc* d = Cur();
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) { SetZoom(g_set.zoom + (delta > 0 ? 10 : -10)); return 0; }
        if (s_settingsOpen) { s_settingsScroll = std::max(0.f, std::min(std::max(0.f, s_settingsH - RH(s_contentR)), s_settingsScroll - delta * 0.6f)); Invalidate(); return 0; }
        if (g_aiPane && In(s_aiArea, x, y)) { AiWheel(delta); return 0; }
        if (!d) return 0;
        if (RW(s_mdArea) > 0 && In(s_mdArea, x, y)) { MdWheel(d, delta); return 0; }
        if (d->hex) HexWheel(d, delta); else if (d->grid) GridWheel(d, msg == WM_MOUSEHWHEEL ? -delta : delta, msg == WM_MOUSEHWHEEL); else EditorWheel(d, msg == WM_MOUSEHWHEEL ? -delta : delta, msg == WM_MOUSEHWHEEL);
        return 0; }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        if (Accelerator(wp)) return 0;
        Doc* d = Cur();
        if (d && !s_settingsOpen && !d->loading) {
            bool handled = d->hex ? HexKey(d, wp) : d->grid ? GridKey(d, wp) : EditorKey(d, wp);
            if (handled) return 0;
        }
        if (msg == WM_SYSKEYDOWN) break;
        return 0; }
    case WM_SYSCHAR: if (GetKeyState(VK_MENU) < 0 && (wp == 'z' || wp == 'Z' || wp == 'f' || wp == 'F' || wp == 'e' || wp == 'E' || wp == 'v' || wp == 'V')) return 0; break;
    case WM_CHAR: {
        Doc* d = Cur();
        if (!d || s_settingsOpen || d->loading || (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0)) return 0;
        if (d->hex) HexChar(d, (wchar_t)wp); else if (d->grid) GridChar(d, (wchar_t)wp); else EditorChar(d, (wchar_t)wp);
        return 0; }
    case WM_IME_STARTCOMPOSITION: {
        Doc* d = Cur();
        if (d && !d->hex) {
            RectF r = EditorCaretRect(d);
            if (HIMC imc = ImmGetContext(h)) {
                COMPOSITIONFORM cf{ CFS_POINT, { (LONG)(r.left * g_scale), (LONG)(r.top * g_scale) } };
                ImmSetCompositionWindow(imc, &cf);
                LOGFONTW lf{}; lf.lfHeight = -(LONG)(EditorFontPx() * g_scale); wcsncpy_s(lf.lfFaceName, g_set.fontFace.c_str(), _TRUNCATE);
                ImmSetCompositionFontW(imc, &lf);
                ImmReleaseContext(h, imc);
            }
        }
        break; }
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) { wchar_t f[MAX_PATH * 4]; DragQueryFileW(hd, i, f, MAX_PATH * 4); OpenPath(f); }
        DragFinish(hd); SetForegroundWindow(h); return 0; }
    case WM_CLOSE:
        if (!CloseWindowAll()) return 0;
        {
            WINDOWPLACEMENT wpl{ sizeof(wpl) }; GetWindowPlacement(h, &wpl);
            HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ModernNotepad", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
                RegSetValueExW(k, L"Placement", 0, REG_BINARY, (BYTE*)&wpl.rcNormalPosition, sizeof(RECT));
                DWORD mx = wpl.showCmd == SW_SHOWMAXIMIZED; RegSetValueExW(k, L"Maximized", 0, REG_DWORD, (BYTE*)&mx, sizeof(mx));
                RegCloseKey(k);
            }
        }
        SettingsSave();
        DestroyWindow(h);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == h) { SetCursorNative(AppCursor()); return TRUE; }
        break;
    case WM_GETOBJECT: { bool hd; LRESULT r = UiaHandleGetObject(h, wp, lp, hd); if (hd) return r; break; }
    case WM_DESTROY: UiaShutdown(); GameShutdown(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace app

using namespace app;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    g_inst = inst;
    using SetCtx = BOOL(WINAPI*)(HANDLE);
    if (auto f = (SetCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) f((HANDLE)-4);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    if (!g_d2d || !g_dw) { MessageBoxW(nullptr, L"Direct2D/DirectWrite are not available.", L"ModernNotepad", MB_ICONERROR); return 1; }
    g_uiFont = UiFont();
    ReadTextScale(); UpdateMetrics();
    g_monoFont = MonoFont(L"Consolas");
    SettingsLoad();

    HANDLE owner = CreateMutexW(nullptr, TRUE, L"ModernNotepad.SessionOwner");
    s_sessionOwner = owner && GetLastError() != ERROR_ALREADY_EXISTS;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = nullptr;   // see AppSetCursor
    wc.lpszClassName = L"ModernNotepadWnd";
    RegisterClassExW(&wc);

    RECT pr{ CW_USEDEFAULT, CW_USEDEFAULT, 0, 0 }; DWORD maxed = 0;
    { HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ModernNotepad", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD sz = sizeof(pr); RECT t; if (RegQueryValueExW(k, L"Placement", nullptr, nullptr, (BYTE*)&t, &sz) == ERROR_SUCCESS && MonitorFromRect(&t, MONITOR_DEFAULTTONULL) && s_sessionOwner) pr = t;
        sz = sizeof(maxed); RegQueryValueExW(k, L"Maximized", nullptr, nullptr, (BYTE*)&maxed, &sz); RegCloseKey(k); } }
    int x = pr.left, y = pr.top, w = pr.right ? pr.right - pr.left : 1000, hgt = pr.bottom ? pr.bottom - pr.top : 700;
    if (pr.left == CW_USEDEFAULT) { x = CW_USEDEFAULT; y = CW_USEDEFAULT; }
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"ModernNotepad", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, hgt, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    OpenCommandLine();
    bool restored = false;
    if (s_sessionOwner && !s_noSession && g_set.restoreSession) {
        size_t before = g_docs.size(); int cur = g_cur;
        restored = SessionRestore();
        if (before && restored) SelectTab(cur);   // files from the command line keep focus
    }
    if (g_docs.empty()) { NewDoc(); }
    SelectTab(g_cur < 0 ? 0 : g_cur);
    ShowWindow(hwnd, maxed ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd);
    SpellInit();
    if (!s_uiaTestOut.empty()) SetTimer(hwnd, 15, 2500, [](HWND w, UINT, UINT_PTR id, DWORD) { KillTimer(w, id); PostMessageW(w, WM_APP + 15, 0, 0); });

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (g_rt) g_rt->Release();
    g_dw->Release(); g_d2d->Release();
    CoUninitialize();
    if (owner) { ReleaseMutex(owner); CloseHandle(owner); }
    return (int)m.wParam;
}
