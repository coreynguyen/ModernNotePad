# Changelog

Newest last. Version 1.0 was the initial release.

## 1.1 changes

* **Mica Alt backdrop** (revised in 1.2, see below). The tab strip and status bar sit on `DWMSBT_TABBEDWINDOW`. That is the
  wallpaper-tinted blue/purple gradient shown while the window has focus; DWM turns it flat grey when the window
  is inactive, matching Win11 Notepad. The frame is extended over the client area and the D2D target uses
  premultiplied alpha, so transparent pixels show the backdrop. It needs Windows 11 22H2+ and the system
  *Transparency effects* setting, otherwise it falls back to solid colours. Toggle: Settings › Transparency effects.
* **Modern menus.** Owner-drawn flyouts with an acrylic backdrop (accent-policy blur since 1.2), rounded corners,
  icons, shortcuts, submenus and keyboard navigation (arrows, Enter, Esc, first-letter mnemonics). The context
  menu has a Cut/Copy/Paste/Select all/Delete command bar. Settings › Menu style switches back to classic Win32
  menus, which are built from the same menu model.
* **Formatting toolbar** in the menu bar:
  * dialect (MD/BB/HTML), heading ▾, list ▾
  * bold, italic, strikethrough, link
  * table ▾ (presets or a custom size), insert ▾ (chart, image, code block, rule, time/date), clear formatting
  * Shortcuts: Ctrl+B / I / K / E, Ctrl+Space, Ctrl+Alt+1..6 (headings), Ctrl+Alt+0 (body text).
* **Markup engine** (`core/markup.cpp`):
  * The same operations emit **Markdown**, **BBCode** (Steam/phpBB style) or **HTML**, and toggle off when applied twice.
  * File › Export as converts between the three into a new tab. HTML export is a complete styled document.
  * The BBCode reader follows the usual shortcode grammar (`[x=v k="q"]…[/x]`, self-closing `[x /]`, `[*]`
    list items, verbatim `[code]`/`[noparse]`, unmatched closers kept as text).
  * The HTML reader decodes entities and skips script/style.
* **Preview** renders Markdown, BBCode and HTML files.
* **Charts.** A ` ```chart ` block (`type: bar|line|pie`, `title:`, `label: value` lines) is drawn natively in the
  preview and becomes inline SVG in HTML export.
* **Tabs.** Untitled tabs take their name from the first line, as in Win11, and Save suggests that name. A modified
  tab shows a grey dot in place of the X until hovered. The app icon sits at the left of the tab strip.
* **System text size** (Settings › Accessibility › Text size, `TextScaleFactor`) scales UI text, bar heights,
  the menus and the editor. It updates live.
* **Defaults.** Line numbers are now off, and so are online features. No "What's new" dialog.
* **Icons.** About 45 new vector icons (menus, toolbar, settings rows). New Vista-style notepad app icon
  (spiral binding, glossy cover band, ruled paper) with simplified 16–24 px variants.
* **Large files.**
  * Files over 8 MB load on a worker thread; the tab shows a placeholder and the UI stays responsive.
  * Over 48 MB or 1M lines, *large file mode* switches off syntax colouring, folding and spell check for that tab.
  * `TextDoc` now has O(1) length, incremental offset tables, in-place newline normalisation, an ASCII fast-path
    UTF-8 decoder and move-in `SetText`. Plain search runs line by line, with no joined copy of the document.
  * Rows map 1:1 to lines unless something is folded, and caret scrolling jumps across far distances instead of
    measuring every line in between.
  * Results on a 300 MB / 4.5M-line file (`tests/bench_large.cpp`, Linux):

    | | Before | After |
    |---|---|---|
    | Find | 6.6 s | 42 ms |
    | `Length()` | 8 ms | ~0 ms |
    | Decode | 6.4 s | 3.1 s |
    | Peak RSS | 5.0 GB | 2.5 GB |

    The RSS figures use Linux's 4-byte `wchar_t`, so expect roughly half that on Windows.
  * Under Wine, a 120 MB / 1.8M-line file opened in about 2.3 s, and Ctrl+End plus typing and Find
    ("1 of 100000+") stayed responsive.
  * Very large unsaved documents are backed up at most once a minute (and always on exit).
* **Robustness.**
  * SVG arcs in icons and chart wedges are flattened by our own code instead of `ID2D1GeometrySink::AddArc`.
  * Text layouts are created on demand, which fixes a crash when the caret timer fired before the first paint.

Not verifiable here (Wine has no DWM backdrops): Mica and acrylic. Both can be switched off in Settings if they
look wrong on a particular machine.

## 1.2 changes

* **CSV/TSV table view** (`core/csv.cpp`, `app/gridview.cpp`).
  * `.csv`, `.tsv` and `.tab` files open as a table. Turn this off with Settings › *Open CSV files as a table*.
    View › Table view, the grid button in the toolbar or Ctrl+Shift+T switch any document between table and text.
  * The parser follows RFC 4180: quoted fields, `""` escapes, delimiters and newlines inside quotes, ragged rows.
    It detects the delimiter (`,` `;` tab `|`), and the context menu › Delimiter overrides it.
  * The first row is used as column headers (toggle). Numbers are right-aligned. In text view the CSV lexer
    colours columns in rotating colours.
  * Selection works by click, drag or Shift, and by clicking row or column headers. The corner button selects all.
    Navigation: arrows, Ctrl+arrows, PgUp/PgDn, Home/End, Tab/Enter.
  * Editing: type, double-click or F2 to edit a cell; Enter/Tab commit and move, Esc cancels. Delete clears the
    selection. The `+` row and the blank last column append data.
  * Clipboard: Ctrl+C/X copies TSV, so it pastes straight into Excel. Ctrl+V pastes a block from Excel or from
    CSV text.
  * Context menu: sort A→Z / Z→A (numeric-aware, blanks last), insert/delete rows and columns, auto-fit columns
    (or double-click a header border), drag a header border to resize.
  * The status bar shows Count / Sum / Average / Min / Max for multi-cell selections, plus row × column counts.
  * The text buffer stays the source of truth. A single-cell edit is a minimal range replacement, which keeps
    the rest of the file byte-identical. Structural edits re-serialise the table in one undo step. Ctrl+Z/Y work
    the same as in text.
  * Tested with 300k rows / 14 MB: opening, Ctrl+End and full-column stats are instant. Files in large-file mode
    (over 48 MB) stay text-only.
* **Title bar fixes.**
  * The menu bar and active tab are opaque surface again, and the active tab flows into the menu bar through the
    concave "feet" (as in 1.0). Only the strip around the tabs (and the status bar) shows the Mica gradient. The
    feet are now real geometry, so they also work over the transparent strip.
  * Duplicate caption buttons fixed. With the frame extended for Mica, DWM drew its own min/max/close, sized for
    the system caption, under ours. DWM only draws those for `WS_SYSMENU` windows, so that style is dropped while
    Mica is on. Our buttons (with Snap Layouts) remain, and Alt+Space and right-clicking the title bar still open
    the system menu.
* **Frosted menus.** Flyouts are `WS_EX_NOACTIVATE`, and DWM system backdrops render their solid fallback for
    inactive windows, which is why the 1.1 menus never looked frosted. 1.2 menus use the accent-policy acrylic blur
    (`SetWindowCompositionAttribute`, `ACCENT_ENABLE_ACRYLICBLURBEHIND`), which works without activation. The menu
    colour is laid on top at about 60 % alpha. With Settings › Transparency effects off, menus are solid.
* Session restore no longer duplicates a tab that was also passed on the command line.

## 1.3 changes

* **Formatted view.** Win11 Notepad's formatting is Markdown underneath (it saves `.md`), so ModernNotepad works
  the same way: the file stays plain Markdown and the editor renders it in place, in the style of Obsidian's
  Live Preview and Typora. You edit the rendered text directly.
  * Every line keeps its DirectWrite layout over the source text, so caret, selection, find, undo and spell check
    are unchanged.
  * Markers (`**`, `#`, `[..](url)`, backticks) shrink to zero width. The caret line shows them dimmed, so you
    can always see and edit the syntax you are on.
  * Headings render larger with a rule under H1/H2. Bold, italic, strikethrough, inline code and links are styled.
  * Bullets, checkboxes (click to toggle), quote bars, horizontal rules and **images** are drawn as inline
    objects. Images load from local paths or `file:///` URLs relative to the document; web images are never
    downloaded.
  * **Tables are real grids** (Word / Win11 Notepad style). Click a cell to type in it:
    * Tab / Shift+Tab move between cells, and Tab in the last cell adds a row.
    * Enter moves down, and leaves the table after the last row. Esc cancels the edit.
    * Right-click gives insert/delete row and column, align, sort and delete table.
    * Every commit rewrites the Markdown as an aligned pipe table, so the source never gets ragged.
  * Enter continues bullets, numbers, checklists and quotes. Enter on an empty item ends the list.
  * Ctrl+click opens a link.
  * New tabs and `.md` files open formatted (Settings › Formatted view). Ctrl+Shift+F, View › Formatted view or
    the status-bar *Formatted / Source* button switch to the Markdown source. View › Split with preview gives
    source + preview side by side.
  * An untitled tab that uses formatting suggests `.md` on save.
* **Markup dropdown removed** from the toolbar. The file type decides the dialect: `.md`, `.txt` and untitled use
  Markdown; `.bbcode` uses BBCode; `.html` uses HTML. File › Export converts between them, and View › Markup
  remains as an override for oddly named files.
* **Link dialog** (Ctrl+K), as in Win11 Notepad: *Display text (optional)* + *Address*. Bare hosts get
  `https://`, and Windows paths become `file:///` URLs.
* **Image dialog:** description + address + **Browse…**, which picks a file from disk and turns it into a
  `file:///` URL.
* **Right-click menu:** Link…, Image…, Table ▸ and Insert ▸ added to the context menu.
* **Open / Save filters:** All supported files, Markdown, HTML/XML, JSON, INI/config, YAML, CSV/TSV, BBCode and
  source code. Each has the matching syntax highlighting.
* **Untitled tab names** strip Markdown / BBCode / HTML markup (`ds[url=…][/url]` → `ds`).
* **Frosted menus, for real this time.**
  * DWM system backdrops draw their solid fallback for inactive windows, and menus are never activated. The
    accent-policy blur is unreliable across builds.
  * So flyouts now do what MPP does: snapshot the screen behind the menu, downsample 4×, three box-blur passes,
    a saturation lift, the menu tint and a faint grain.
  * Works on Windows 10 and 11. Settings › Transparency effects (now always available) turns it off.
* **Credits + Glyph Invaders** on the Settings page: a sticky panel on wide windows, a card at the bottom otherwise.
  * Stray characters (`{ } ; # @`, then `NULL` / `0xDEAD` words and a `SEGFAULT` boss every 5th wave) attack. You
    are the caret.
  * Mouse moves, left click (hold) fires the laser, right click fires homing missiles (3, recharging).
  * A 4-voice chiptune (A minor, 140 BPM, two sections) and synthesised SFX (laser, missile, explosions, hits,
    jingles) are mixed live into a `waveOut` stream. There are no audio files.
  * Music plays only while a game runs. It pauses when you leave Settings or switch windows, and Music / Sound
    toggles mute it.

## 1.3.1 changes

* **Themed message boxes** (`app/msgbox.cpp`). Every `MessageBoxW` now goes through a Direct2D dialog in the Win11
  content-dialog style that follows Light, Dark and Pink: a tinted title band, a round icon, a footer band and an
  accent default button. The save prompt reads Save / Don't save / Cancel, as in Win11 Notepad.
  * It is modal like the original: the owner window is disabled.
  * Keys: Enter takes the default button; Esc maps to Cancel, then No, then OK; Tab and the arrows move focus;
    first-letter mnemonics work; Ctrl+C copies the text. It plays the system sound and has a DWM shadow and
    rounded corners.
* **Pink theme** (Settings › App theme, or View › Theme): blush chrome, rose accent, plum text, its own syntax
  palette, and a pink tint over Mica.
* **Editor background patterns** (Settings › Editor background): Auto (hearts on Pink), None, Dots, Hearts, Stars,
  Grid paper.
  * They are subtle, scroll with the text, and work in every theme.
  * One tile is rendered once and tiled with a wrapping bitmap brush, so they cost almost nothing to draw.

## 1.4 changes

* **Evaluate expression** (right-click › *Evaluate expression*, Ctrl+Shift+Enter, Edit › Evaluate). It acts on the
  selection, or on the current line when nothing is selected. The engine is the MAXScript
  interpreter from MAX2Modern, vendored as `third_party/mxs`; it is portable C++17 with no outside dependencies.

  | Line | Result |
  |---|---|
  | `4+6` | `4+6=10` (spacing follows the expression: `4 + 6` → `4 + 6 = 10`) |
  | `4+6=` | `4+6=10` |
  | `Rent split 1850/3` | `Rent split 1850/3=616.666666667` (finds trailing arithmetic inside prose) |
  | `sqrt 2 * 10`, `pi*2^2`, `mod 10 3`, `toUpper "hi"` | all MAXScript functions and operators |
  | `for i = 1 to 10 do format "#%\n"` | inserts `#1` … `#10` below (the loop variable is supplied when the format has no arguments) |
  | selected multi-line script | runs it; `format` / `print` output is inserted below and selected |

  * One-line expressions that divide use calculator semantics (`10/4 = 2.5`); scripts keep MAXScript's integer
    division.
  * Variables persist for the session (`x = 2^16`, then `x / 4`). Edit › Evaluate › Clear variables resets them.
  * *Replace with result* swaps the expression or script for its result or output.
  * Sandboxed: file and process natives are denied, each run has a 3 s watchdog, and output is capped at 4 MB.
  * Errors only appear in the status bar; the text is never changed.
* **Pink theme graphics:**
  * A vector pink notebook-and-heart app icon.
  * Hearts replace the unsaved-tab dot.
  * A flower replaces the gear, a bow the theme icon, and a heart the AI sparkle.
  * Heart bullets, and heart checkmarks for done tasks, in the formatted view.
  * An empty document shows a bow illustration with "Write something lovely ♡".

## 1.4.1: data-integrity fixes (from an external code review)

| Finding | Fix |
|---|---|
| Save fell back to `CREATE_ALWAYS` on the original (truncate first, unchecked byte count) | The fallback is removed. The normal path writes a temp file and verifies every byte with a write loop plus `FlushFileBuffers`, then `ReplaceFileW`. If the folder forbids a temp file, the original is first copied to `%TEMP%`, then rewritten **in place without truncation** (`OPEN_EXISTING` → write → `SetEndOfFile`), and the copy is restored on any failure. |
| Short reads reported success | A read error or byte count ≠ file size is now a load failure; nothing opens as a partial document. |
| Hex-edited text files were backed up as raw bytes but restored as UTF-16 | The session file has a payload-format field (0 = UTF-16 text, 1 = raw bytes). Raw backups of text files are decoded with the file's own encoding. Tested by killing the process mid hex-edit. |
| Backup / auto-save version markers advanced before success | The backup version only moves after `WriteAll` succeeds. Auto-save to disk uses `SaveDocQuiet` (status-bar messages, no dialog spam), marks saved only on success, and retries after 30 s. |
| Hex undo reset before the save result was known | `HexLeave/HexEnter` now run only after the bytes are on disk. |
| UTF-8 validator accepted overlong forms, surrogates, > U+10FFFF | Strict RFC 3629 validation, with 13 new test cases (222/222 core tests pass). |
| No protection against external modification | Last-write time and size are recorded on load and save. Save asks before overwriting a file another program changed (auto-save skips it). On window activation or tab switch, unmodified tabs reload silently and modified tabs ask. |
| ANSI save silently replaced characters | Checked with `WC_NO_BEST_FIT_CHARS`; the save offers UTF-8 instead. |
| AI over `http://` to remote hosts | Refused. Only `https://` or `http://localhost` are allowed. |
| Version metadata 1.0.0.0 | Now 1.4.1.0 in `app.rc`, the UI and the user agent. |

Still open after 1.4.1 (the first two were done in 1.5):

* **Memory.** The vector-of-lines buffer peaks at about 2.5 GB on a 300 MB file. Planned: a piece table over a memory-mapped original.
* **Test coverage.** No fault-injection tests yet (forced short writes or reads, `ReplaceFileW` failure), and no randomized edit/undo fuzzing.

## 1.5 changes

* **Screen readers / UI Automation** (`app/uia.cpp`).
  * The editor is now a UIA *Document* element served from `WM_GETOBJECT`, implementing `ITextProvider2` and
    `ITextRangeProvider`.
  * What it supports:
    * document range, selection, caret range (`GetCaretRange`) and visible ranges
    * `RangeFromPoint`
    * Move / MoveEndpointByUnit / ExpandToEnclosingUnit for character (surrogate-aware), word, line,
      paragraph and document
    * text, bounding rectangles, Select, ScrollIntoView, FindText, and the IsReadOnly / font attributes
  * Focus, text-changed and selection-changed events are raised after each paint, and only when a UIA client is
    listening.
  * Ranges are offsets into the document, so they stay valid while you scroll.
  * Narrator, NVDA and JAWS can read by character, word and line and follow the caret. The custom-drawn tabs,
    menus and settings page are not exposed as UIA elements yet (the find box and dialogs use real Edit controls).
  * Verification:
    * Wine's UIA client resolves the element across processes (name and control type come back correct), but it
      has no Text pattern client.
    * So the text pattern is checked with a built-in self-test, `ModernNotepad.exe file.txt /uiaselftest out.txt`,
      which drives the providers the way a client does.
    * Expected output: the word walk `The / quick / brown / fox`, line ±n, character moves, case-insensitive
      FindText, Select → GetSelection, the caret word and the visible range.
* **Own regex engine** (`core/regex.cpp`, about 450 lines), replacing `std::wregex`. It is a Pike VM: linear time,
  memory proportional to the pattern, no recursion, and a deadline.
  * Before: `.*` across a few MB could overflow the stack (a crash), and `(a+)+$` was exponential.
  * Now on 2 M characters: `(a+)+$` takes about 0.5 s with no match, `.*` matches in 0.14 s, a literal-prefixed
    pattern over 3.6 MB takes 4 ms.
  * Syntax: the ECMAScript subset listed in `regex.h`, with groups and `$1` replacements. `^`/`$` match at line
    boundaries. Backreferences and lookaround are rejected with a clear message.
  * A differential test against `std::wregex` (`/tmp`-style harness) agreed on 23 of 25 cases. The 2 differences
    are documented ECMAScript quirks (captures inside `(a*)*` and the empty class `[]`).
  * Budgets: highlighting stops after 0.7 s and says so, Find next stops after 3 s, and Replace all gets 12 s and
    changes nothing if it can't finish (never a partial replace).
* Core tests: 234/234.
