// spell.cpp - spell checking via the Windows Spell Checking API (ISpellChecker, a system
// component since Windows 8; no external dictionaries or DLLs shipped).
#include "app.h"
#include <spellcheck.h>
#include <unordered_set>

namespace app {

static const GUID kClsidFactory = { 0x7ab36653, 0x1796, 0x484b, { 0xbd, 0xfa, 0xe7, 0x4f, 0x1d, 0xb7, 0xc1, 0xdc } };
static const GUID kIidFactory = { 0x8e018a9d, 0x2415, 0x4677, { 0xbf, 0x08, 0x79, 0x4e, 0xa6, 0x1f, 0x94, 0xbb } };
static ISpellChecker* s_sc = nullptr;
static bool s_tried = false;
static std::unordered_set<std::wstring> s_ignored;

bool SpellInit() {
    if (s_tried) return s_sc != nullptr;
    s_tried = true;
    ISpellCheckerFactory* f = nullptr;
    if (FAILED(CoCreateInstance(kClsidFactory, nullptr, CLSCTX_INPROC_SERVER, kIidFactory, (void**)&f)) || !f) return false;
    wchar_t loc[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH);
    BOOL ok = FALSE;
    if (FAILED(f->IsSupported(loc, &ok)) || !ok) { wcscpy_s(loc, L"en-US"); f->IsSupported(loc, &ok); }
    if (ok) f->CreateSpellChecker(loc, &s_sc);
    f->Release();
    return s_sc != nullptr;
}
bool SpellAvailable() { return SpellInit(); }

void SpellCheckLine(const std::wstring& line, std::vector<std::pair<int, int>>& errs) {
    errs.clear();
    if (!SpellInit()) return;
    IEnumSpellingError* e = nullptr;
    if (FAILED(s_sc->Check(line.c_str(), &e)) || !e) return;
    ISpellingError* er = nullptr;
    while (e->Next(&er) == S_OK) {
        ULONG st = 0, len = 0; CORRECTIVE_ACTION act;
        er->get_StartIndex(&st); er->get_Length(&len); er->get_CorrectiveAction(&act);
        er->Release();
        if (act == CORRECTIVE_ACTION_NONE || len == 0 || st + len > line.size()) continue;
        std::wstring w = line.substr(st, len);
        bool skip = s_ignored.count(w) > 0;
        // skip identifiers / things that look like code: digits, underscores, camelCase, ALLCAPS
        int upper = 0;
        for (size_t i = 0; i < w.size(); ++i) {
            wchar_t c = w[i];
            if (iswdigit(c) || c == L'_') skip = true;
            if (iswupper(c)) { ++upper; if (i > 0) skip = true; }
        }
        if (upper == (int)w.size() && w.size() > 1) skip = true;
        if (!skip) errs.push_back({ (int)st, (int)len });
    }
    e->Release();
}

std::vector<std::wstring> SpellSuggest(const std::wstring& word) {
    std::vector<std::wstring> out;
    if (!SpellInit()) return out;
    IEnumString* es = nullptr;
    if (FAILED(s_sc->Suggest(word.c_str(), &es)) || !es) return out;
    LPOLESTR s = nullptr; ULONG got = 0;
    while (out.size() < 8 && es->Next(1, &s, &got) == S_OK && got) { out.push_back(s); CoTaskMemFree(s); }
    es->Release();
    return out;
}
void SpellAdd(const std::wstring& word) { if (SpellInit()) s_sc->Add(word.c_str()); EditorClearCache(); }
void SpellIgnore(const std::wstring& word) { s_ignored.insert(word); if (SpellInit()) s_sc->Ignore(word.c_str()); EditorClearCache(); }

} // namespace app
