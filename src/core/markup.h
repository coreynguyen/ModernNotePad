// markup.h - lightweight "rich text" over plain text: formatting operations that emit
// Markdown, BBCode (Steam/phpBB style) or HTML, converters between them, and chart blocks.
// Portable C++17. BBCode grammar follows the usual shortcode rules:
//   [name], [name=value], [name key=value key="quoted"], [/name], self-closing [name /],
//   nesting, unmatched closing tags are kept as literal text.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace np {

enum class Dialect { Markdown, BBCode, Html };
const wchar_t* DialectName(Dialect d);

enum class Inline { Bold, Italic, Strike, Underline, Code, Link };
enum class LineOp { Heading, Paragraph, Bullet, Number, Task, Quote, CodeBlock };

struct MarkupEdit { std::wstring text; int selStart = 0, selEnd = 0; };   // sel offsets into text

// Wrap (or unwrap, if already wrapped) a selection. Empty selection -> markers with caret inside.
MarkupEdit WrapInline(Dialect d, Inline kind, const std::wstring& sel, const std::wstring& url = L"https://");
// Transform a block of whole lines ('\n' separated). level used for headings (1..6).
std::wstring ApplyLineOp(Dialect d, LineOp op, const std::wstring& block, int level = 1);
std::wstring ClearFormatting(Dialect d, const std::wstring& s);
std::wstring MakeTable(Dialect d, int rows, int cols);
std::wstring MakeRule(Dialect d);
std::wstring MakeImage(Dialect d, const std::wstring& url, const std::wstring& alt);
std::wstring MakeChartBlock(Dialect d, const std::wstring& type);

// Converters
std::wstring MarkdownToHtml(const std::wstring& md, bool fullDocument, const std::wstring& title = L"Document");
std::wstring MarkdownToBBCode(const std::wstring& md);
std::wstring BBCodeToMarkdown(const std::wstring& bb);
std::wstring HtmlToMarkdown(const std::wstring& html);

// Charts: fenced ```chart blocks:  "type: bar|line|pie", "title: ...", then "label: value" lines.
struct ChartSpec { std::wstring type = L"bar", title; std::vector<std::pair<std::wstring, double>> data; };
bool ParseChart(const std::wstring& body, ChartSpec& out);
std::wstring ChartSvg(const ChartSpec& c, int w = 480, int h = 260);

// Minimal DOM used by the BBCode / HTML readers (exposed for tests)
struct MNode { std::wstring name; std::vector<std::pair<std::wstring, std::wstring>> attrs; std::wstring text; std::vector<MNode> kids;
               std::wstring Attr(const wchar_t* k) const; };
MNode ParseBBCode(const std::wstring& s);
MNode ParseHtml(const std::wstring& s);

} // namespace np
