// mdlive.h - "live preview" analysis of Markdown source for the Formatted view.
// The source text stays untouched: these functions only classify lines and report which source ranges
// are markers to hide, which carry styles and which are replaced by drawn objects (bullets, checkboxes,
// rules, images, tables). Columns are source columns, so caret/selection mapping stays 1:1.
#pragma once
#include "text_doc.h"
#include <string>
#include <vector>

namespace np {

enum class LiveKind : uint8_t { Blank, Para, Heading, Bullet, Ordered, Task, Quote, Rule, Fence, Code, TableHead, TableHidden };

struct LiveLine {
    LiveKind kind = LiveKind::Para;
    uint8_t level = 0;        // heading level (1..6)
    uint8_t checked = 0;      // task state
    int prefix = 0;           // source columns covered by the block prefix ("## ", "  - ", "> ", "- [ ] ")
    int markerAt = 0;         // column where the list/quote marker starts (after indentation)
    int tableEnd = -1;        // TableHead: last line of the table block
};

// Classifies every line. Tables need a header row + a delimiter row (| --- | :-: |).
void ScanLive(const TextDoc& doc, std::vector<LiveLine>& out);

enum class LiveSpanKind : uint8_t { Hide, Strong, Em, Strike, Code, Link, Image, Bullet, Task, Quote, Rule, Escape };
struct LiveSpan {
    int a = 0, len = 0;
    LiveSpanKind kind = LiveSpanKind::Hide;
    std::wstring url, alt;   // Link / Image
    int arg = 0;             // Bullet depth, Task checked
};
// Inline + prefix spans for one (non-table, non-code) line.
void LiveSpans(const std::wstring& line, const LiveLine& info, std::vector<LiveSpan>& out);

// ---- tables
struct LiveTable {
    int first = 0, last = 0;
    std::vector<std::vector<std::wstring>> rows;   // rows[0] = header; cells are raw (inline markdown kept)
    std::vector<int> align;                        // 0 left, 1 center, 2 right, 3 explicit left (:--)
    int Cols() const { return rows.empty() ? 0 : (int)rows[0].size(); }
};
bool ParseLiveTable(const TextDoc& doc, int first, int last, LiveTable& t);
// Aligned pipe table text (no trailing newline). Column widths follow the longest cell.
std::wstring SerializeLiveTable(const LiveTable& t);
std::vector<std::wstring> SplitTableRow(const std::wstring& line);
std::wstring StripInline(const std::wstring& cell);   // display text for a cell (markers removed)
// Tab title for an untitled document: first line without Markdown/BBCode/HTML markup.
std::wstring PlainTitle(const std::wstring& line);

} // namespace np
