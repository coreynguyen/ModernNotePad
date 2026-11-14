// session.cpp - settings (HKCU\Software\ModernNotepad), API key protection (DPAPI),
// auto-save of unsaved tabs to %LOCALAPPDATA%\ModernNotepad\Backup and session restore.
#include "app.h"
#include <wincrypt.h>
#include <shlobj.h>
#include <cstdio>
#include <fstream>
#include <set>

namespace app {

static const wchar_t* kKey = L"Software\\ModernNotepad";

static DWORD GetDw(HKEY k, const wchar_t* n, DWORD def) { DWORD v = def, sz = sizeof(v); RegGetValueW(k, nullptr, n, RRF_RT_REG_DWORD, nullptr, &v, &sz); return v; }
static std::wstring GetSz(HKEY k, const wchar_t* n, const std::wstring& def) {
    DWORD sz = 0;
    if (RegGetValueW(k, nullptr, n, RRF_RT_REG_SZ, nullptr, nullptr, &sz) != ERROR_SUCCESS || sz < 2) return def;
    std::wstring s(sz / sizeof(wchar_t), 0);
    RegGetValueW(k, nullptr, n, RRF_RT_REG_SZ, nullptr, &s[0], &sz);
    s.resize(wcslen(s.c_str()));
    return s;
}
static void SetDw(HKEY k, const wchar_t* n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, (BYTE*)&v, sizeof(v)); }
static void SetSz(HKEY k, const wchar_t* n, const std::wstring& v) { RegSetValueExW(k, n, 0, REG_SZ, (BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); }

void SettingsLoad() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    Settings& s = g_set;
    s.theme = (int)GetDw(k, L"Theme", s.theme);
    s.fontFace = GetSz(k, L"FontFace", s.fontFace);
    s.fontSize = GetDw(k, L"FontSize10", (DWORD)(s.fontSize * 10)) / 10.f;
    s.fontBold = GetDw(k, L"FontBold", s.fontBold) != 0; s.fontItalic = GetDw(k, L"FontItalic", s.fontItalic) != 0;
    s.wordWrap = GetDw(k, L"WordWrap", s.wordWrap) != 0; s.statusBar = GetDw(k, L"StatusBar", s.statusBar) != 0;
    s.lineNumbers = GetDw(k, L"LineNumbers", s.lineNumbers) != 0; s.folding = GetDw(k, L"Folding", s.folding) != 0;
    s.syntax = GetDw(k, L"Syntax", s.syntax) != 0; s.spell = GetDw(k, L"Spell", s.spell) != 0;
    s.autoIndent = GetDw(k, L"AutoIndent", s.autoIndent) != 0; s.insertSpaces = GetDw(k, L"InsertSpaces", s.insertSpaces) != 0;
    s.restoreSession = GetDw(k, L"RestoreSession", s.restoreSession) != 0; s.autoSaveDisk = GetDw(k, L"AutoSaveDisk", s.autoSaveDisk) != 0;
    s.tabSize = std::max(1, std::min(16, (int)GetDw(k, L"TabSize", s.tabSize)));
    s.zoom = std::max(10, std::min(500, (int)GetDw(k, L"Zoom", s.zoom)));
    s.aiProvider = (int)GetDw(k, L"AiProvider", s.aiProvider);
    s.aiEndpoint = GetSz(k, L"AiEndpoint", s.aiEndpoint);
    s.aiModel = GetSz(k, L"AiModel", s.aiModel);
    s.mdSplit = GetDw(k, L"MdSplit", (DWORD)(s.mdSplit * 1000)) / 1000.f;
    s.online = GetDw(k, L"Online", s.online) != 0; s.modernMenus = GetDw(k, L"ModernMenus", s.modernMenus) != 0;
    s.transparency = GetDw(k, L"Transparency", s.transparency) != 0; s.formatBar = GetDw(k, L"FormatBar", s.formatBar) != 0;
    s.csvAsTable = GetDw(k, L"CsvAsTable", s.csvAsTable) != 0;
    s.formatted = GetDw(k, L"Formatted", s.formatted) != 0;
    s.bgPattern = (int)GetDw(k, L"BgPattern", s.bgPattern);
    s.highContrast = GetDw(k, L"HighContrast", s.highContrast) != 0;
    s.defaultDialect = std::max(0, std::min(2, (int)GetDw(k, L"DefaultDialect", s.defaultDialect)));
    if (GetDw(k, L"SettingsVersion", 1) < 2) s.lineNumbers = false;   // v1.1: line numbers default off (Win11 parity)
    s.recent.clear();
    for (int i = 0; i < 10; ++i) { wchar_t n[16]; swprintf(n, 16, L"Recent%d", i); std::wstring r = GetSz(k, n, L""); if (!r.empty()) s.recent.push_back(r); }
    // API key: DPAPI blob
    DWORD sz = 0;
    if (RegQueryValueExW(k, L"AiKey", nullptr, nullptr, nullptr, &sz) == ERROR_SUCCESS && sz) {
        std::vector<BYTE> blob(sz);
        RegQueryValueExW(k, L"AiKey", nullptr, nullptr, blob.data(), &sz);
        DATA_BLOB in{ sz, blob.data() }, out{};
        if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            s.aiKey.assign((wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
            LocalFree(out.pbData);
        }
    }
    RegCloseKey(k);
}

void SettingsSave() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    const Settings& s = g_set;
    SetDw(k, L"Theme", s.theme); SetSz(k, L"FontFace", s.fontFace); SetDw(k, L"FontSize10", (DWORD)(s.fontSize * 10 + 0.5f));
    SetDw(k, L"FontBold", s.fontBold); SetDw(k, L"FontItalic", s.fontItalic);
    SetDw(k, L"WordWrap", s.wordWrap); SetDw(k, L"StatusBar", s.statusBar); SetDw(k, L"LineNumbers", s.lineNumbers);
    SetDw(k, L"Folding", s.folding); SetDw(k, L"Syntax", s.syntax); SetDw(k, L"Spell", s.spell);
    SetDw(k, L"AutoIndent", s.autoIndent); SetDw(k, L"InsertSpaces", s.insertSpaces);
    SetDw(k, L"RestoreSession", s.restoreSession); SetDw(k, L"AutoSaveDisk", s.autoSaveDisk);
    SetDw(k, L"TabSize", s.tabSize); SetDw(k, L"Zoom", s.zoom);
    SetDw(k, L"AiProvider", s.aiProvider); SetSz(k, L"AiEndpoint", s.aiEndpoint); SetSz(k, L"AiModel", s.aiModel);
    SetDw(k, L"MdSplit", (DWORD)(s.mdSplit * 1000));
    SetDw(k, L"Online", s.online); SetDw(k, L"ModernMenus", s.modernMenus); SetDw(k, L"Transparency", s.transparency);
    SetDw(k, L"FormatBar", s.formatBar); SetDw(k, L"CsvAsTable", s.csvAsTable); SetDw(k, L"Formatted", s.formatted); SetDw(k, L"BgPattern", s.bgPattern); SetDw(k, L"HighContrast", s.highContrast); SetDw(k, L"DefaultDialect", s.defaultDialect); SetDw(k, L"SettingsVersion", 2);
    for (int i = 0; i < 10; ++i) { wchar_t n[16]; swprintf(n, 16, L"Recent%d", i); if (i < (int)s.recent.size()) SetSz(k, n, s.recent[i]); else RegDeleteValueW(k, n); }
    if (s.aiKey.empty()) RegDeleteValueW(k, L"AiKey");
    else {
        DATA_BLOB in{ (DWORD)(s.aiKey.size() * sizeof(wchar_t)), (BYTE*)s.aiKey.data() }, out{};
        if (CryptProtectData(&in, L"ModernNotepad AI key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            RegSetValueExW(k, L"AiKey", 0, REG_BINARY, out.pbData, out.cbData);
            LocalFree(out.pbData);
        }
    }
    RegCloseKey(k);
}

// ------------------------------------------------------------------ session
std::wstring AppDataDir() {
    PWSTR p = nullptr; std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) { dir = p; CoTaskMemFree(p); }
    else { wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t); dir = t; }
    dir += L"\\ModernNotepad";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\Backup").c_str(), nullptr);
    return dir;
}
static bool WriteAll(const std::wstring& path, const void* data, size_t n) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; BOOL ok = WriteFile(h, data, (DWORD)n, &w, nullptr) && w == n;
    FlushFileBuffers(h); CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
static bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD r = 0; BOOL ok = out.empty() || ReadFile(h, out.data(), (DWORD)out.size(), &r, nullptr);
    CloseHandle(h);
    return ok;
}
static std::wstring NewBackupName() {
    GUID g; CoCreateGuid(&g);
    wchar_t b[64]; swprintf(b, 64, L"%08lX%04X%04X%02X%02X.bak", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[7]);
    return b;
}
static bool NeedsBackup(Doc* d) { return !d->loading && d->Modified() && !(d->path.empty() && !d->binaryOnly && d->text.Length() == 0); }
static bool s_finalSave = false;

static void WriteBackup(Doc* d) {
    if (d->backupName.empty()) d->backupName = NewBackupName();
    std::wstring p = AppDataDir() + L"\\Backup\\" + d->backupName;
    bool raw = d->binaryOnly || d->hex, ok;
    if (raw) ok = WriteAll(p, d->bytes.data(), d->bytes.size());
    else { auto b = np::EncodeText(d->text.Text(), np::Enc::UTF16LE, np::Eol::LF); ok = WriteAll(p, b.data(), b.size()); }
    if (ok) { d->backupVer = d->text.Version() ^ (d->hexUndo.size() << 48); d->backupRaw = raw; }   // failed -> retried next tick
}
void SessionDeleteBackup(Doc* d) {
    if (d->backupName.empty()) return;
    DeleteFileW((AppDataDir() + L"\\Backup\\" + d->backupName).c_str());
    d->backupName.clear(); d->backupVer = UINT64_MAX;
}

static std::string s_lastSession;
static void WriteSessionFile() {
    std::wstring s = L"ModernNotepadSession\t1\n";
    for (auto& up : g_docs) {
        Doc* d = up.get();
        bool mod = NeedsBackup(d);
        if (!mod && d->path.empty()) continue;
        wchar_t b[256];
        // last field: backup payload format (0 = UTF-16LE text, 1 = raw file bytes from the hex editor)
        swprintf(b, 256, L"\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", (int)d->text.enc, (int)d->text.eol, mod ? 1 : 0,
                 d->caret.line, d->caret.col, d->topRow, (int)d->lang, d->langAuto ? 1 : 0, d->binaryOnly ? 1 : 0, d->mdPreview ? 1 : 0, d->backupRaw ? 1 : 0);
        s += L"tab\t" + d->path + L"\t" + (mod ? d->backupName : L"") + b;
    }
    s += L"cur\t" + std::to_wstring(g_cur) + L"\n";
    std::string u = np::WideToUtf8(s);
    if (u == s_lastSession) return;
    if (WriteAll(AppDataDir() + L"\\session.txt", u.data(), u.size())) s_lastSession = u;
}

void SessionAutosaveTick() {
    for (auto& up : g_docs) {
        Doc* d = up.get();
        if (NeedsBackup(d)) {
            uint64_t v = d->text.Version() ^ (d->hexUndo.size() << 48);
            // very large documents: back up at most once a minute (and always on exit)
            static std::map<uint64_t, ULONGLONG> lastBig;
            bool big = d->text.Length() > (8u << 20) || d->bytes.size() > (16u << 20);
            if (big && !s_finalSave && GetTickCount64() - lastBig[d->id] < 60000) continue;
            if (v != d->backupVer || d->backupName.empty()) { WriteBackup(d); if (big) lastBig[d->id] = GetTickCount64(); }
            static std::map<uint64_t, ULONGLONG> lastFail;
            if (g_set.autoSaveDisk && !d->path.empty() && !d->forceDirty && !d->hex && d->savedDiskVer != d->text.Version() &&
                GetTickCount64() - lastFail[d->id] > 30000) {
                if (!SaveDocQuiet(d)) lastFail[d->id] = GetTickCount64();   // SaveDoc sets savedDiskVer only on success; retry in 30 s
            }
        } else if (!d->backupName.empty()) SessionDeleteBackup(d);
    }
    WriteSessionFile();
}

void SessionSave() {
    s_finalSave = true;
    SessionAutosaveTick();
    s_finalSave = false;
    // remove orphaned backups
    std::set<std::wstring> keep;
    for (auto& d : g_docs) if (!d->backupName.empty()) keep.insert(d->backupName);
    std::wstring dir = AppDataDir() + L"\\Backup\\";
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"*.bak").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!keep.count(fd.cFileName)) DeleteFileW((dir + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static std::vector<std::wstring> Split(const std::wstring& s, wchar_t c) {
    std::vector<std::wstring> o; size_t a = 0;
    for (;;) { size_t e = s.find(c, a); o.push_back(s.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a)); if (e == std::wstring::npos) break; a = e + 1; }
    return o;
}

bool SessionRestore() {
    std::vector<uint8_t> raw;
    if (!ReadAll(AppDataDir() + L"\\session.txt", raw)) return false;
    std::wstring s = np::Utf8ToWide(std::string(raw.begin(), raw.end()));
    auto lines = Split(s, L'\n');
    if (lines.empty() || lines[0].rfind(L"ModernNotepadSession", 0) != 0) return false;
    int cur = 0; bool any = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        auto f = Split(lines[i], L'\t');
        if (f.size() >= 2 && f[0] == L"cur") { cur = _wtoi(f[1].c_str()); continue; }
        if (f.size() < 13 || f[0] != L"tab") continue;
        std::wstring path = f[1], bak = f[2];
        int enc = _wtoi(f[3].c_str()), eol = _wtoi(f[4].c_str()), mod = _wtoi(f[5].c_str());
        np::Pos caret{ _wtoi(f[6].c_str()), _wtoi(f[7].c_str()) };
        int top = _wtoi(f[8].c_str()), lang = _wtoi(f[9].c_str()), langAuto = _wtoi(f[10].c_str()), bin = _wtoi(f[11].c_str()), md = _wtoi(f[12].c_str());
        Doc* d = nullptr;
        // already open from the command line: keep that tab, but let unsaved session changes win
        Doc* open = nullptr;
        if (!path.empty()) for (auto& x : g_docs) if (!_wcsicmp(x->path.c_str(), path.c_str())) open = x.get();
        if (open && (!mod || open->loading || open->Modified())) continue;
        std::vector<uint8_t> bytes;
        if (mod && !bak.empty() && ReadAll(AppDataDir() + L"\\Backup\\" + bak, bytes)) {
            d = open ? open : NewDoc();
            if (open) { d->hex = false; d->grid = false; }
            d->path = path; d->backupName = bak;
            bool raw = f.size() > 13 && _wtoi(f[13].c_str()) == 1;
            if (bin) { d->binaryOnly = true; d->bytes = bytes; d->hex = true; }
            else if (raw) {   // a text file that was being hex-edited: the backup is its raw bytes in the file's encoding
                std::wstring t = np::DecodeAs(bytes, (np::Enc)std::min(enc, 4));
                np::NormalizeNewlinesInPlace(t);
                d->text.SetText(std::move(t));
            }
            else d->text.SetText(np::DecodeAs(bytes, np::Enc::UTF16LE));
            d->text.enc = (np::Enc)std::min(enc, 4); d->text.eol = (np::Eol)std::min(eol, 2);
            d->forceDirty = true;
        } else if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!OpenPath(path, true)) continue;
            d = Cur();
        } else continue;
        d->lang = (np::Lang)std::min(lang, (int)np::L_COUNT - 1); d->langAuto = langAuto != 0;
        if (!d->loading) d->fmtView = g_set.formatted && !d->binaryOnly && (d->lang == np::L_MD || d->path.empty());
        if (d->loading) { d->restoreCaret = caret; d->restoreTop = top; }
        else { d->caret = d->anchor = d->text.Clamp(caret); d->topRow = std::max(0, top); }
        d->mdPreview = md != 0;
        if (!d->loading && !d->binaryOnly && !d->grid && g_set.csvAsTable && d->lang == np::L_CSV && !d->large) { d->mdPreview = false; GridEnter(d); }
        any = true;
    }
    if (any) SelectTab(std::max(0, std::min(cur, (int)g_docs.size() - 1)));
    return any;
}

} // namespace app
