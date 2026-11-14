// app.h - shared state for the ModernNotepad shell (Win32 + Direct2D/DirectWrite).
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "../core/text_doc.h"
#include "../core/syntax.h"
#include "../core/search.h"
#include "../core/markdown.h"
#include "../core/markup.h"
#include "../core/csv.h"
#include "../core/mdlive.h"

namespace app {

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com& o) : p(o.p) { if (p) p->AddRef(); }
    Com& operator=(const Com& o) { if (o.p) o.p->AddRef(); if (p) p->Release(); p = o.p; return *this; }
    ~Com() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** operator&() { if (p) { p->Release(); p = nullptr; } return &p; }
    operator T*() const { return p; }
    void Reset() { if (p) { p->Release(); p = nullptr; } }
};

using Color = D2D1_COLOR_F;
inline Color Hex(uint32_t rgb, float a = 1.f) { return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a); }
inline Color Alpha(Color c, float a) { c.a *= a; return c; }
inline Color Mix(Color a, Color b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t }; }
using RectF = D2D1_RECT_F;
inline RectF RF(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
inline bool In(const RectF& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
inline float RW(const RectF& r) { return r.right - r.left; }
inline float RH(const RectF& r) { return r.bottom - r.top; }

struct Theme {
    bool dark = false, pink = false;
    Color menuHot;   // menu item hover
    bool hc = false; // effective high contrast (setting or Windows high-contrast mode)
    Color chrome, surface, editor, text, dim, faint, border, hover, press, accent, accentText, selection, selectionInactive,
          lineHi, gutterText, gutterTextActive, findHi, findCur, danger, squiggle, codeBg, link, scrollThumb;
    Color tok[np::T_COUNT];
};

// --------------------------------------------------------------- document
struct HexEdit { size_t off; std::vector<uint8_t> oldB, newB; size_t caretBefore; };

struct Doc {
    np::TextDoc text;
    std::wstring path;
    np::Lang lang = np::L_TEXT;
    bool langAuto = true;
    np::LexCache lex;
    std::vector<np::FoldRegion> folds;
    uint64_t foldsVer = UINT64_MAX;
    std::vector<int> rows;            // visible logical lines (folding applied); unused when rowsIdentity
    bool rowsIdentity = true, anyFolded = false;
    uint64_t rowsVer = UINT64_MAX;
    bool rowsDirty = true;
    np::Pos caret, anchor;
    float desiredX = -1;
    int topRow = 0;
    float scrollX = 0;
    bool overwrite = false;
    // hex editor
    bool hex = false, binaryOnly = false;
    std::vector<uint8_t> bytes;
    std::vector<HexEdit> hexUndo, hexRedo;
    size_t hexSaved = 0;
    size_t hexCaret = 0, hexAnchor = 0;
    bool hexLow = false, hexAscii = false, hexInsert = false;
    int hexTop = 0;
    // markdown
    bool mdPreview = false;
    float mdScroll = 0;
    // session
    std::wstring backupName;
    uint64_t backupVer = UINT64_MAX;
    bool forceDirty = false;          // restored unsaved doc
    uint64_t savedDiskVer = 0;        // for auto-save to disk
    uint64_t diskTime = 0, diskSize = UINT64_MAX;   // last-write time + size when loaded/saved (external change detection)
    bool backupRaw = false;           // the current backup file holds raw bytes (hex/binary), not UTF-16 text
    int dialect = -1;                 // markup dialect override (-1 auto from language)
    bool loading = false, large = false;   // async load in progress / large-file mode (no syntax, folding, spell)
    uint64_t id = 0;
    double loadMB = 0;
    np::Pos restoreCaret; int restoreTop = -1;   // applied when an async load completes
    // table (CSV/TSV) view
    bool grid = false, gHeader = true;
    wchar_t delim = 0;                // 0 = auto-detect on entry
    np::CsvTable table;
    int gr = 1, gc = 0, ga = 1, gca = 0, gTop = 0;   // cursor row/col, selection anchor, first visible row
    float gScrollX = 0;
    std::vector<float> colW;
    // formatted (live Markdown) view
    bool fmtView = false;
    std::vector<np::LiveLine> live;
    uint64_t liveVer = UINT64_MAX;
    bool liveTables = false;

    bool Modified() const {
        if (binaryOnly) return hexUndo.size() != hexSaved || forceDirty;
        return text.Modified() || forceDirty || (hex && hexUndo.size() != hexSaved);
    }
    std::wstring Name() const {
        if (path.empty()) {   // Win11 behaviour: an untitled tab is named after its first line
            if (binaryOnly || loading) return L"Untitled";
            std::wstring t = np::PlainTitle(text.Line(0).substr(0, 400));
            if (t.empty()) return L"Untitled";
            if (t.size() > 40) { t.resize(40); while (!t.empty() && t.back() == L' ') t.pop_back(); }
            return t;
        }
        size_t s = path.find_last_of(L"\\/");
        return s == std::wstring::npos ? path : path.substr(s + 1);
    }
};

// --------------------------------------------------------------- settings
struct Settings {
    int theme = 0;                // 0 system, 1 light, 2 dark, 3 pink
    bool highContrast = false;    // stronger hover/selection/borders (also forced on by Windows high-contrast mode)
    int bgPattern = 0;            // editor background: 0 auto (hearts on Pink), 1 none, 2 dots, 3 hearts, 4 stars, 5 grid paper
    std::wstring fontFace = L"Consolas";
    float fontSize = 11.f;        // points
    bool fontBold = false, fontItalic = false;
    bool wordWrap = true, statusBar = true, lineNumbers = false, folding = true, syntax = true, spell = true;
    bool autoIndent = true, insertSpaces = false, restoreSession = true, autoSaveDisk = false;
    int tabSize = 4;
    int zoom = 100;               // percent
    int aiProvider = 0;           // 0 Anthropic Messages API, 1 OpenAI-compatible chat/completions
    std::wstring aiEndpoint = L"https://api.anthropic.com/v1/messages";
    std::wstring aiModel = L"claude-sonnet-5-5";
    std::wstring aiKey;           // DPAPI-protected in registry
    float mdSplit = 0.5f;
    bool online = false;          // gate for every network feature (AI); off by default
    bool modernMenus = true, transparency = true, formatBar = true;
    int defaultDialect = 0;       // for plain text / unknown: 0 Markdown, 1 BBCode, 2 HTML
    bool csvAsTable = true;       // open .csv/.tsv files in the table view
    bool formatted = true;        // formatted (live Markdown) view for new tabs and .md files
    std::vector<std::wstring> recent;
};

// --------------------------------------------------------------- commands
enum Cmd {
    C_NONE = 0,
    C_NEW = 100, C_NEWWIN, C_OPEN, C_SAVE, C_SAVEAS, C_SAVEALL, C_PRINT, C_CLOSETAB, C_CLOSEWIN, C_EXIT,
    C_RECENT0 = 150,                                            // .. +9
    C_UNDO = 200, C_REDO, C_CUT, C_COPY, C_PASTE, C_DELETE, C_SELALL, C_FIND, C_FINDNEXT, C_FINDPREV, C_REPLACE, C_GOTO,
    C_TIMEDATE, C_FONT, C_DUPLINE, C_TOGGLECOMMENT, C_MOVEUP, C_MOVEDOWN, C_UPPER, C_LOWER, C_TRIMWS, C_SORTLINES,
    C_EOL_CRLF = 250, C_EOL_LF, C_EOL_CR,
    C_ENC0 = 260,                                               // .. +4 (save-as encoding)
    C_REOPEN0 = 270,                                            // .. +4 (reopen with encoding)
    C_ZOOMIN = 300, C_ZOOMOUT, C_ZOOMRESET, C_STATUSBAR, C_WORDWRAP, C_LINENUMS, C_FOLDING, C_FOLDALL, C_UNFOLDALL,
    C_FOLDCUR, C_UNFOLDCUR, C_SYNTAX, C_MDPREVIEW, C_HEX, C_SPELL, C_AIPANE, C_SETTINGS, C_THEME_SYS, C_THEME_LIGHT, C_THEME_DARK, C_THEME_PINK,
    C_LANG0 = 400,                                              // .. + L_COUNT
    C_AI_EXPLAIN = 500, C_AI_SUMMARIZE, C_AI_REWRITE, C_AI_GRAMMAR, C_AI_CUSTOM, C_AI_CONTINUE,
    C_SPELL_SUG0 = 600,                                         // .. +15
    C_SPELL_ADD = 620, C_SPELL_IGNORE,
    C_TAB_CLOSEOTHERS = 700, C_TAB_CLOSERIGHT, C_TAB_COPYPATH, C_TAB_OPENFOLDER,
    C_ABOUT = 800,
    C_NEWMD = 820, C_EXPORT_HTML, C_EXPORT_BB, C_EXPORT_MD,
    C_FMT_BOLD = 840, C_FMT_ITALIC, C_FMT_STRIKE, C_FMT_UNDERLINE, C_FMT_CODE, C_FMT_LINK,
    C_FMT_H1, C_FMT_H2, C_FMT_H3, C_FMT_H4, C_FMT_H5, C_FMT_H6, C_FMT_PARA,
    C_FMT_BULLET, C_FMT_NUMBER, C_FMT_TASK, C_FMT_QUOTE, C_FMT_CODEBLOCK, C_FMT_CLEAR,
    C_INS_TABLE0 = 880,                                         // .. +5 presets
    C_INS_TABLECUSTOM = 890, C_INS_CHART_BAR, C_INS_CHART_LINE, C_INS_CHART_PIE, C_INS_IMAGE, C_INS_RULE,
    C_DIALECT_AUTO = 900, C_DIALECT_MD, C_DIALECT_BB, C_DIALECT_HTML,
    C_FORMATBAR = 910, C_MENUSTYLE, C_TRANSPARENCY, C_ONLINE,
    C_UNI0 = 920,                                               // .. +7 unicode control chars
    C_GRID = 940, C_GRID_SORTASC, C_GRID_SORTDESC, C_GRID_INSROWABOVE, C_GRID_INSROWBELOW, C_GRID_DELROW,
    C_GRID_INSCOLLEFT, C_GRID_INSCOLRIGHT, C_GRID_DELCOL, C_GRID_AUTOFIT, C_GRID_HEADER, C_GRID_EDIT,
    C_GRID_DELIM0 = 960,                                        // .. +3  , ; tab |
    C_LT_EDIT = 970, C_LT_ROWABOVE, C_LT_ROWBELOW, C_LT_DELROW, C_LT_COLLEFT, C_LT_COLRIGHT, C_LT_DELCOL,
    C_LT_ALIGNL, C_LT_ALIGNC, C_LT_ALIGNR, C_LT_SORT, C_LT_DELTABLE,
    C_FMTVIEW = 990, C_INS_LINKDLG, C_ABOUT_GAME, C_EVAL, C_EVAL_REPLACE, C_EVAL_RESET,
};

// --------------------------------------------------------------- globals
extern HWND g_hwnd;
extern HINSTANCE g_inst;
extern float g_scale;                       // dpi / 96
extern Theme g_th;
extern Settings g_set;
extern std::vector<std::unique_ptr<Doc>> g_docs;
extern int g_cur;
extern ID2D1Factory* g_d2d;
extern IDWriteFactory* g_dw;
extern ID2D1HwndRenderTarget* g_rt;

inline Doc* Cur() { return (g_cur >= 0 && g_cur < (int)g_docs.size()) ? g_docs[g_cur].get() : nullptr; }

// Cursor: the main window class has no cursor; WM_SETCURSOR re-applies the last one our mouse code chose.
// (With a class cursor Windows resets it to the arrow before every WM_MOUSEMOVE, which flickers -
// very visibly with large / inverted accessibility cursors whose hotspots differ.)
HCURSOR AppSetCursor(HCURSOR c);
HCURSOR AppCursor();
#define SetCursor AppSetCursor

// gfx.cpp
ID2D1SolidColorBrush* Brush(Color c);
void FillRR(const RectF& r, float rad, Color c);
void StrokeRR(const RectF& r, float rad, Color c, float w = 1.f);
void FillR(const RectF& r, Color c);
void Line(float x0, float y0, float x1, float y1, Color c, float w = 1.f);
IDWriteTextFormat* Fmt(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE s = DWRITE_FONT_STYLE_NORMAL);
IDWriteTextFormat* UiFmt(float size = 12.f, bool bold = false);
float TextW(const std::wstring& s, IDWriteTextFormat* f);
void Text(const std::wstring& s, const RectF& r, IDWriteTextFormat* f, Color c, int halign = 0 /*0 l,1 c,2 r*/, bool vcenter = true);
enum Icon { I_ADD, I_CLOSE, I_CHEV_DOWN, I_CHEV_RIGHT, I_CHEV_UP, I_SEARCH, I_GEAR, I_MIN, I_MAX, I_RESTORE, I_SPARKLE,
            I_MARKDOWN, I_HEX, I_ARROW_UP, I_ARROW_DOWN, I_BACK, I_DOT, I_REPLACE, I_REPLACEALL, I_COPY, I_INSERT, I_CHECK, I_FILE,
            I_CUT, I_PASTE, I_SELECTALL, I_DELETE, I_UNDO, I_REDO, I_TABLE, I_LINK, I_LIST, I_LISTNUM, I_TASK, I_QUOTE, I_CODE,
            I_CHART, I_IMAGE, I_HR, I_CLEARFMT, I_PRINT, I_SAVE, I_OPEN, I_NEWWIN, I_RECENT, I_EXIT, I_WRAP, I_ZOOM, I_FONT, I_THEME,
            I_LINENUM, I_FOLD, I_SPELL, I_EYE, I_COMMENT, I_SORT, I_EOL, I_GLOBE, I_MENUSTYLE, I_TRANSPARENCY, I_TOOLBAR, I_TAB,
            I_KEY, I_SERVER, I_MODEL, I_INFO, I_EXPORT, I_GOTO, I_UNICODE, I_SESSION, I_HEADING, I_BOLD, I_ITALIC, I_STRIKE,
            I_UNDERLINE, I_STATUSBAR, I_INDENT, I_LANG, I_ENCODING, I_HEART, I_HEARTF, I_FLOWER, I_BOW, I_CALC, I_COUNT, I_NONE = -1 };
void DrawIcon(Icon i, float cx, float cy, float size, Color c, float stroke = 1.25f);
void ReleaseBrushesFor(ID2D1RenderTarget* rt);
void DrawAppIcon(float x, float y, float size);
void PaintPattern(const RectF& area, float scrollY);   // themed editor background pattern
int ThemedMessageBox(HWND owner, LPCWSTR text, LPCWSTR caption, UINT flags);
extern float g_textScale;          // Settings > Accessibility > Text size (1.0 .. 2.25)
extern bool g_mica;                // Mica/acrylic active for this session
void ReadTextScale();
bool TransparencySupported();      // Win11 22H2+ and "Transparency effects" on
void BuildTheme();
void ResetDeviceResources();
std::wstring UiFont();
extern std::wstring g_uiFont, g_monoFont;
std::wstring ResolveFamily(const std::wstring& want, std::initializer_list<const wchar_t*> fallbacks);
std::wstring MonoFont(const std::wstring& want);

// editor.cpp
struct EditorRects { RectF area, gutter, text, vbar, hbar; };
void EditorPaint(Doc* d, const RectF& area);
bool EditorMouse(Doc* d, UINT msg, float x, float y, WPARAM wp);
bool EditorKey(Doc* d, WPARAM vk);
void EditorChar(Doc* d, wchar_t ch);
void EditorWheel(Doc* d, int delta, bool horizontal);
void EditorInsert(Doc* d, const std::wstring& s, bool typing = false);
void EditorEnsureCaret(Doc* d);
void EditorClearCache();
void EditorUpdateRows(Doc* d);
inline int RowCount(Doc* d) { return d->rowsIdentity ? d->text.LineCount() : (int)d->rows.size(); }
inline int RowLine(Doc* d, int r) { return d->rowsIdentity ? r : d->rows[r]; }
void EditorSetCaret(Doc* d, np::Pos p, bool extend);
void EditorSelect(Doc* d, np::Pos a, np::Pos b);
std::wstring EditorSelection(Doc* d);
bool EditorHasSel(Doc* d);
void EditorDeleteSel(Doc* d);
void EditorRecomputeFolds(Doc* d);
void EditorToggleFold(Doc* d, int line, int force = -1);   // force: 1 fold, 0 unfold
void EditorFoldAll(Doc* d, bool fold);
void EditorUnfoldFor(Doc* d, int line);
void EditorTick();                                          // caret blink
void EditorCommand(Doc* d, int cmd);
void EditorAfterEdit(Doc* d);
bool EditorVisibleLines(Doc* d, int& first, int& last);
void EditorRangeRects(Doc* d, np::Pos a, np::Pos b, std::vector<RectF>& out);   // client DIPs
np::Pos EditorHitPublic(Doc* d, float x, float y);
LRESULT UiaHandleGetObject(HWND h, WPARAM wp, LPARAM lp, bool& handled);
void UiaNotify();     // call after paint: raises selection/text/focus events when they changed
void UiaShutdown();
void UiaSelfTest(const std::wstring& outPath);
float EditorLineHeight();
bool EditorWordAt(Doc* d, float x, float y, np::Pos& a, np::Pos& b);
np::Pos EditorHit(Doc* d, float x, float y);
RectF EditorCaretRect(Doc* d);
extern bool g_caretOn;
extern std::vector<np::Match> g_findMatches;
extern np::Searcher g_searcher;
extern bool g_findActive;

// hexview.cpp
void HexPaint(Doc* d, const RectF& area);
bool HexMouse(Doc* d, UINT msg, float x, float y, WPARAM wp);
bool HexKey(Doc* d, WPARAM vk);
void HexChar(Doc* d, wchar_t c);
void HexWheel(Doc* d, int delta);
void HexEnter(Doc* d);
bool HexLeave(Doc* d);
void HexUndo(Doc* d, bool redo);
void HexCopy(Doc* d);
void HexPaste(Doc* d);

// mdview.cpp
void MdPaint(Doc* d, const RectF& area);
void MdWheel(Doc* d, int delta);
bool MdClick(Doc* d, float x, float y);
void MdSyncTo(Doc* d, int topLine);
void MdClearCache();

// evaluate.cpp - expression evaluation (MAXScript engine)
void EvaluateCommand(Doc* d, bool replace);
void EvaluateReset();
void PaintPinkEmpty(const RectF& area);

// game.cpp - credits panel + Glyph Invaders
void GamePaint(const RectF& r);
bool GameMouse(UINT msg, float x, float y, WPARAM wp);
void GameTick();
void GamePause();
void GameShutdown();
bool GameRunning();

// livefmt.cpp - Formatted (live Markdown) view
bool LiveOn(Doc* d);
void LiveSync(Doc* d);
uint64_t LiveKey(Doc* d, int line, bool reveal, float avail);
bool LiveStyle(Doc* d, int line, bool reveal, IDWriteTextLayout* L, IDWriteTextFormat* base, float px, float avail);
void LiveDecor(Doc* d, int line, IDWriteTextLayout* L, float x0, float y, float h, const RectF& textRect);
void LiveBeginFrame();
void LiveAfterPaint(Doc* d);
np::Pos LiveFixCaret(Doc* d, np::Pos p, np::Pos old);
bool LiveMouseDown(Doc* d, float x, float y, np::Pos p, WPARAM wp);
bool LiveHoverLink(Doc* d, np::Pos p);
bool LiveOverTable(float x, float y);
bool LiveKey(Doc* d, WPARAM vk);
bool LiveChar(Doc* d, wchar_t ch);
bool LiveContextMenu(Doc* d, float x, float y, int sx, int sy);
void LiveTableCommand(Doc* d, int cmd);
bool LiveEditing();
void LiveCommitEdit();
void LiveCancelEdit();
void LiveEditTableNear(Doc* d, int line);
#define WM_APP_LIVECOMMIT (WM_APP + 13)

// gridview.cpp - CSV/TSV table view
void GridEnter(Doc* d);
void GridPaint(Doc* d, const RectF& area);
bool GridMouse(Doc* d, UINT msg, float x, float y, WPARAM wp);
bool GridKey(Doc* d, WPARAM vk);
void GridChar(Doc* d, wchar_t ch);
void GridWheel(Doc* d, int delta, bool horizontal);
void GridCommand(Doc* d, int cmd);
void GridContextMenu(Doc* d, int sx, int sy);
std::wstring GridStatus(Doc* d, std::wstring& stats);
void GridCopy(Doc* d, bool cut);
void GridPaste(Doc* d);
bool GridEditing();
void GridCommit();
#define WM_APP_GRIDCOMMIT (WM_APP + 12)
void GridCancelEdit();
HFONT UiEditFont();

// spell.cpp
bool SpellInit();
void SpellCheckLine(const std::wstring& line, std::vector<std::pair<int, int>>& errs);
std::vector<std::wstring> SpellSuggest(const std::wstring& word);
void SpellAdd(const std::wstring& word);
void SpellIgnore(const std::wstring& word);
bool SpellAvailable();

// ai.cpp
void AiPaint(const RectF& area);
bool AiMouse(UINT msg, float x, float y);
void AiWheel(int delta);
void AiRun(int cmd, const std::wstring& custom = L"");
void AiOnResult(LPARAM lp);
bool AiBusy();
extern bool g_aiPane;
#define WM_APP_AI (WM_APP + 10)
#define WM_APP_LOADED (WM_APP + 11)
void OnLoaded(LPARAM lp);

// session.cpp
void SessionSave();
bool SessionRestore();
void SessionAutosaveTick();
void SessionDeleteBackup(Doc* d);
void SettingsLoad();
void SettingsSave();

// flyout.cpp - one menu model, two renderers (modern acrylic flyout / classic Win32 menu)
struct MenuItem {
    int id = 0; std::wstring text, keys; Icon icon = I_NONE;
    bool check = false, enabled = true, sep = false;
    std::vector<MenuItem> sub;
};
struct MenuModel { std::vector<MenuItem> bar; std::vector<MenuItem> items; };   // bar = icon command row (modern) / plain items (classic)
inline MenuItem MI(int id, const std::wstring& t, const std::wstring& k = L"", Icon ic = I_NONE, bool check = false, bool en = true) {
    MenuItem m; m.id = id; m.text = t; m.keys = k; m.icon = ic; m.check = check; m.enabled = en; return m;
}
inline MenuItem MSep() { MenuItem m; m.sep = true; return m; }
inline MenuItem MSub(const std::wstring& t, Icon ic, std::vector<MenuItem> sub, bool en = true) { MenuItem m; m.text = t; m.icon = ic; m.sub = std::move(sub); m.enabled = en; return m; }
enum { MF_ANCHOR_ABOVE = 1 };
int RunMenu(const MenuModel& m, int screenX, int screenY, int flags = 0);   // returns command id or 0
bool MenuActive();

// main.cpp services
void Invalidate();
void SetStatus(const std::wstring& s);
void RunCommand(int cmd);
void UpdateTitle();
Doc* NewDoc();
bool OpenPath(const std::wstring& path, bool newTab = true, int forceEnc = -1);
bool SaveDocQuiet(Doc* d);   // auto-save: no dialogs, status-bar messages instead
void CheckExternalChange();
bool SaveDoc(Doc* d, bool saveAs, int forceEnc = -1);
void SelectTab(int i);
void Prompt(const std::wstring& title, const std::wstring& initial, void (*done)(const std::wstring&), bool password = false);
void ShowContextMenu(Doc* d, int sx, int sy);
np::Dialect DocDialect(Doc* d);
std::wstring PreviewMarkdown(Doc* d);
bool PreviewSupported(Doc* d);
std::wstring ClipboardGet();
void ClipboardSet(const std::wstring& s);
void ApplyDarkMenus();
float EditorFontPx();   // editor font size in DIPs incl. zoom
std::wstring AppDataDir();

} // namespace app
