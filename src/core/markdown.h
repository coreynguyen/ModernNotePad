// markdown.h - small CommonMark-ish parser producing a render model for the preview pane.
// Blocks: ATX/setext headings, paragraphs, bullet/ordered/task lists (nested by indent),
// blockquotes, fenced/indented code, thematic breaks, pipe tables.
// Inline: **strong**, *em*, ~~strike~~, `code`, [links](url), autolinks, ![img](url) (as link).
#pragma once
#include <string>
#include <vector>

namespace np {

enum MdStyle : unsigned { MS_STRONG = 1, MS_EM = 2, MS_CODE = 4, MS_LINK = 8, MS_STRIKE = 16 };

struct MdRun { std::wstring text; unsigned style = 0; std::wstring url; };

enum class MdKind { Heading, Para, ListItem, Quote, Code, Rule, Table };

struct MdBlock {
    MdKind kind = MdKind::Para;
    int level = 0;            // heading level / list depth / quote depth
    bool ordered = false;
    int number = 0;
    int task = -1;            // -1 not a task, 0 unchecked, 1 checked
    std::vector<MdRun> runs;  // inline content (Heading/Para/ListItem/Quote)
    std::wstring code, lang;  // Code
    std::vector<std::vector<std::vector<MdRun>>> rows;  // Table: rows -> cells -> runs (row 0 = header)
    std::vector<int> align;   // per column: 0 left, 1 center, 2 right
    int srcLine = 0;          // first source line (for scroll sync)
};

std::vector<MdBlock> ParseMarkdown(const std::wstring& text);
std::vector<MdRun> ParseInline(const std::wstring& s);
std::wstring RunsText(const std::vector<MdRun>& r);

} // namespace np
