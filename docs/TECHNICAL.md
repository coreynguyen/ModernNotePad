# ModernNotepad — technical notes

A dependency-free Windows text editor, written from scratch, modelled on the look and behaviour of the
Windows 11 Notepad, extended with the features the XP-era Notepad never had.

* **Single exe, no redistributables.** Win32 + Direct2D + DirectWrite. Everything it links is part
  of Windows 10/11 (d2d1, dwrite, dwmapi, comdlg32, shell32, ole32, imm32, winhttp, crypt32).
  The Release build uses the static CRT (`/MT`).
* **Original artwork.** Every icon is an SVG-style path on a 16×16 grid (`src/app/gfx.cpp`), rendered
  through cached `ID2D1PathGeometry`. The app icon (`res/app.ico`) was generated for this project.
* **Own editor engine.** Text storage, layout, undo, search and rendering are all implemented here rather than
  wrapping a system edit control.

## Build

Open `ModernNotepad.sln` in Visual Studio 2026 (toolset v145; accept *Retarget* if prompted) and build
`Release | x64` (ARM64 is also configured). Output: `bin\x64\Release\ModernNotepad.exe`.
`CoreTests` is a console project that runs the portable editor-core tests.

## Features vs. XP Notepad

| Missing in XP | Here | Where |
|---|---|---|
| Multiple document tabs | Tabs in the title bar, drag to reorder, middle-click close, context menu, unsaved dot, Ctrl+Tab / Ctrl+1..9 | `main.cpp` |
| Syntax highlighting | 22 languages, incremental per-line lexer state (C/C++/C#/Java/JS/TS/Rust/Go/Python/JSON/XML/HTML/CSS/Markdown/INI/TOML/YAML/Shell/PowerShell/Batch/SQL/Lua/ASM) | `core/syntax.cpp` |
| Code folding | Brace-, indentation-, heading- and section-based. Gutter chevrons, Ctrl+Shift+[ / ], fold/unfold all | `core/syntax.cpp`, `editor.cpp` |
| Line numbers | Gutter with active-line emphasis, click/drag to select lines | `editor.cpp` |
| Dark mode | Light / Dark / follow system (live), dark title bar, dark popup menus, uses the system accent colour | `gfx.cpp` |
| Zoom | Ctrl +/−/0, Ctrl+wheel, status-bar presets. Scales editor, hex view and preview | `main.cpp` |
| Spell checking | Windows Spell Checking API (`ISpellChecker`). Squiggles, suggestions, add/ignore. In code only comments and strings are checked | `spell.cpp` |
| Markdown preview | Side-by-side rendered view (headings, emphasis, code blocks, quotes, nested/task lists, tables, links), scroll-synced, draggable splitter | `core/markdown.cpp`, `mdview.cpp` |
| Auto-save / session recovery | Every 5 s unsaved tabs are written atomically to `%LOCALAPPDATA%\ModernNotepad\Backup`. Tabs, caret, scroll and preview state come back after closing or a crash. Optional auto-save to disk | `session.cpp` |
| Multi-level undo | Unlimited grouped undo/redo, typing coalesced at word boundaries, Replace All is one step | `core/text_doc.cpp` |
| Line endings | Detects CRLF/LF/CR (warns on mixed), convert via status bar or Edit menu | `core/text_doc.cpp` |
| Regex search | Floating find/replace card: match case, whole word, ECMAScript regex with `$1` replacements, "n of N", match ticks on the scrollbar | `core/search.cpp` |
| Hex/binary editor | Ctrl+Shift+H on any tab. Binary files open straight into it. Nibble edit, insert/overwrite, ASCII column, hex copy/paste, own undo | `hexview.cpp` |
| AI / Copilot | **Off by default** (Settings › Online features). When enabled, the AI assist pane offers: Explain, Summarize, Rewrite, Fix grammar, Continue, free-form Ask. Replace/Insert/Copy result. Anthropic Messages API or any OpenAI-compatible endpoint (Ollama, LM Studio, llama.cpp). Key stored with DPAPI | `ai.cpp` |

Also: encodings (UTF-8, UTF-8 BOM, UTF-16 LE/BE, ANSI) with save-as / reopen-with; word wrap; per-monitor
DPI v2; Snap Layouts on the maximize button; IME composition placement; drag & drop; recent files;
print with header/footer; Go to line/offset; Time/Date (F5); duplicate / move / comment lines;
case conversion; trim whitespace; sort lines; atomic saves via `ReplaceFileW`.

## Keyboard

| Keys | Action |
|---|---|
| Ctrl+N / Ctrl+Shift+N | New tab / new window |
| Ctrl+O, Ctrl+S, Ctrl+Shift+S, Ctrl+Alt+S | Open, save, save as, save all |
| Ctrl+W / Ctrl+Shift+W | Close tab / window |
| Ctrl+F, Ctrl+H, F3 / Shift+F3 | Find, replace, next/previous (Alt+C/W/R toggle options in the find box) |
| Ctrl+G, F5 | Go to line, insert time/date |
| Ctrl+D, Alt+Up/Down, Ctrl+/ | Duplicate, move, toggle comment |
| Ctrl+Shift+U / Ctrl+U | Upper / lower case |
| Ctrl+Shift+[ / ] | Fold / unfold |
| Ctrl+Shift+M, Ctrl+Shift+H, Ctrl+Shift+I | Markdown preview, hex editor, AI pane |
| Ctrl+Shift+T | Table view on/off (CSV/TSV) |
| Ctrl+Shift+F | Formatted view / Markdown source |
| Ctrl+K | Link dialog |
| Alt+Z | Word wrap |
| Ctrl+Plus / Minus / 0 | Zoom |
| Ctrl+, | Settings |
| Insert | Overwrite mode (hex: insert mode) |

## Layout

```
src/core/   portable C++17 (no Win32): text buffer + undo, codecs, lexers, folding, search, markdown,
            markup (Markdown/BBCode/HTML ops, converters, charts), csv (RFC 4180 table model),
            mdlive (live-preview analysis: line kinds, marker/style spans, pipe tables)
src/app/    Win32 shell: main (window/tabs/menus/find/settings/toolbar), gfx (D2D, theme, icons), flyout (menus),
            editor, hexview, gridview (CSV table), livefmt (formatted view), game (credits), mdview, spell, ai, session
tests/      core_tests.cpp (209 checks), bench_large.cpp, CoreTests.vcxproj
res/        app.ico, app.manifest (PerMonitorV2, long paths), app.rc
```

## Verification done

* The core tests (`tests/core_tests.cpp`) pass on Linux g++ (209/209).
* The full app cross-compiles warning-free with clang (MinGW target). Its imports are OS DLLs only.
* Smoke-tested under Wine 9 on Xvfb. These all worked:
  * tabs and session restore
  * syntax highlighting and folding
  * Markdown preview
  * the find card
  * the settings page
  * dark theme
  * hex editing
  * crash recovery after `kill -9`
  * the AI pane against a mock server, on both the OpenAI-compatible and Anthropic request formats
* Two things couldn't be checked under Wine: the selection highlight (Wine stubs DirectWrite hit-testing) and spell check (no `ISpellChecker` in Wine). Check both on real Windows.

## Known limits

* The buffer is a vector of lines. It is comfortable to roughly 100 MB. Above 200k lines folding is disabled.
* `std::wregex` is slow on huge files and has no lookbehind.

