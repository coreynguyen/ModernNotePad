// Portable unit tests for the editor core: g++ -std=c++17 -I../src tests/core_tests.cpp src/core/*.cpp
#include "core/text_doc.h"
#include "core/syntax.h"
#include "core/search.h"
#include "core/markdown.h"
#include "core/markup.h"
#include "core/csv.h"
#include "core/mdlive.h"
#include <cstdio>
using namespace np;
static int fails = 0, total = 0;
#define CHECK(c) do { ++total; if (!(c)) { ++fails; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t fakeNow = 0; static uint64_t FakeClock() { return fakeNow; }
int main() {
    TextDoc::Clock = FakeClock;
    { TextDoc d; d.SetText(L"hello\r\nworld\rx\ny");
      CHECK(d.LineCount() == 4); CHECK(d.Line(1) == L"world"); CHECK(!d.Modified());
      Pos e = d.Replace({0,5}, {0,5}, L" there\nnew", {0,5}, {0,5});
      CHECK(e.line == 1 && e.col == 3); CHECK(d.Line(0) == L"hello there"); CHECK(d.Line(1) == L"new"); CHECK(d.Modified());
      Pos c, a; CHECK(d.Undo(c, a)); CHECK(d.Text() == L"hello\nworld\nx\ny"); CHECK(!d.Modified());
      CHECK(d.Redo(c, a)); CHECK(d.Line(1) == L"new"); CHECK(d.Modified());
      CHECK(d.Offset({1,1}) == 13); CHECK(d.PosAt(13) == (Pos{1,1}));
      CHECK(d.GetText({0,6},{1,2}) == L"there\nne"); }
    { // typing coalescing: "abc def" -> two undo steps (word boundary at space)
      TextDoc d; Pos p{0,0};
      for (wchar_t ch : std::wstring(L"abc def")) { fakeNow += 50; p = d.Replace(p, p, std::wstring(1, ch), p, p, true); }
      CHECK(d.Line(0) == L"abc def");
      Pos c, a; d.Undo(c, a); CHECK(d.Line(0) == L"abc");
      d.Undo(c, a); CHECK(d.Line(0) == L"");
      fakeNow += 5000; }
    { // groups
      TextDoc d; d.SetText(L"a b a"); Pos z{0,0};
      d.BeginGroup(); d.Replace({0,0},{0,1},L"X",z,z); d.Replace({0,4},{0,5},L"X",z,z); d.EndGroup();
      CHECK(d.Line(0) == L"X b X"); Pos c, a; d.Undo(c, a); CHECK(d.Line(0) == L"a b a"); }
    { // stamps & flags travel with lines
      TextDoc d; d.SetText(L"1\n2\n3"); uint32_t s3 = d.Stamp(2); d.Flags(2) = LF_FOLDED;
      d.Replace({0,0},{0,0},L"0\n",{0,0},{0,0});
      CHECK(d.Stamp(3) == s3); CHECK(d.Flags(3) == LF_FOLDED); CHECK(d.Flags(2) == 0); }
    { // codecs
      std::wstring t = sizeof(wchar_t) == 2 ? L"h\u00e9llo \u4e16\u754c \U0001F600\nline2" : L"h\u00e9llo \u4e16\u754c\nline2";
      for (Enc e : {Enc::UTF8, Enc::UTF8BOM, Enc::UTF16LE, Enc::UTF16BE}) for (Eol l : {Eol::CRLF, Eol::LF, Eol::CR}) {
          auto b = EncodeText(t, e, l); Decoded d = DecodeBytes(b);
          CHECK(d.text == t); CHECK(d.enc == e); CHECK(d.eol == l); CHECK(!d.binary); }
      std::vector<uint8_t> bin = {0x4D,0x5A,0x90,0,3,0,0,0,4,0,0,0,0xFF,0xFF,0,0};
      CHECK(DecodeBytes(bin).binary);
      std::vector<uint8_t> latin = {'c','a','f',0xE9}; CHECK(DecodeBytes(latin).enc == Enc::ANSI); }
    { // lexer
      std::vector<Span> sp;
      int st = LexLine(L_CPP, L"int x = 42; // hi", 0, sp);
      CHECK(st == 0); CHECK(sp[0].tok == T_TYPE); CHECK(sp.back().tok == T_COMMENT);
      st = LexLine(L_CPP, L"a /* open", 0, sp); CHECK(st == 1);
      st = LexLine(L_CPP, L"still */ b", st, sp); CHECK(st == 0 && sp[0].tok == T_COMMENT);
      st = LexLine(L_PY, L"x = \"\"\"doc", 0, sp); CHECK(st == 2);
      LexLine(L_JSON, L"  \"key\": \"v\", \"n\": 1", 0, sp);
      bool key = false; for (auto& s : sp) if (s.tok == T_KEY) key = true; CHECK(key);
      st = LexLine(L_XML, L"<a href=\"x\"><!-- c", 0, sp); CHECK(st == 1);
      LexLine(L_MD, L"# Title", 0, sp); CHECK(sp[0].tok == T_HEADING);
      CHECK(LangFromPath(L"C:\\x\\Foo.CPP") == L_CPP); CHECK(LangFromPath(L"readme.md") == L_MD); }
    { // folding
      TextDoc d; d.SetText(L"int f() {\n  if (x) {\n    y();\n  }\n}\nint g;"); LexCache lc; std::vector<FoldRegion> f;
      ComputeFolds(d, L_CPP, lc, f); CHECK(f.size() == 2); CHECK(f[0].start == 0 && f[0].end == 4); CHECK(f[1].start == 1 && f[1].end == 3);
      d.SetText(L"def a():\n    x=1\n    if x:\n        y\nz=2"); lc.Reset(); ComputeFolds(d, L_PY, lc, f);
            CHECK(f.size() == 2); CHECK(f[0].start == 0 && f[0].end == 3);
      d.SetText(L"# A\ntext\n## B\nmore\n# C\nend"); lc.Reset(); ComputeFolds(d, L_MD, lc, f);
      CHECK(f.size() == 3); CHECK(f[0].start == 0 && f[0].end == 3); }
    { // search
      TextDoc d; d.SetText(L"Foo bar foo\nfoobar FOO");
      Searcher s; SearchOpts o; o.pattern = L"foo"; s.Prepare(o);
      std::vector<Match> all; s.FindAll(d, all); CHECK(all.size() == 4);
      o.wholeWord = true; s.Prepare(o); s.FindAll(d, all); CHECK(all.size() == 3);
      o.matchCase = true; s.Prepare(o); s.FindAll(d, all); CHECK(all.size() == 1);
      Match m; o = SearchOpts(); o.pattern = L"foo"; s.Prepare(o);
      CHECK(s.Find(d, 1, true, m) && m.off == 8);
      CHECK(s.Find(d, 8, false, m) && m.off == 0);
      CHECK(s.Find(d, 21, true, m) && m.off == 0);   // wrap
      o.regex = true; o.pattern = L"(\\w+)bar"; s.Prepare(o);
      CHECK(s.Find(d, 0, true, m) && m.off == 12 && m.len == 6);
      CHECK(s.Expand(d, m, L"[$1]") == L"[foo]");
      CHECK(s.ReplaceAll(d, L"<$1>", {0,0}) == 1); CHECK(d.Line(1) == L"<foo> FOO");
      Pos c, a; d.Undo(c, a); CHECK(d.Line(1) == L"foobar FOO");
      o.pattern = L"(unclosed"; std::wstring err; CHECK(!s.Prepare(o, &err)); CHECK(!err.empty()); }
    { // markdown
      auto b = ParseMarkdown(L"# Title\n\nSome **bold** and *it* `code` [link](http://x.y).\n\n- a\n  - b\n1. one\n- [x] done\n\n> quote\n\n```c\nint x;\n```\n\n| a | b |\n|:-|-:|\n| 1 | 2 |\n\n---\nSetext\n===");
            CHECK(b.size() == 11); CHECK(b[10].kind == MdKind::Heading && b[10].level == 1);
      CHECK(b[0].kind == MdKind::Heading && b[0].level == 1);
      CHECK(b[1].kind == MdKind::Para); bool bold = false, link = false;
      for (auto& r : b[1].runs) { if (r.style & MS_STRONG) bold = r.text == L"bold"; if (r.style & MS_LINK) link = r.url == L"http://x.y"; }
      CHECK(bold); CHECK(link);
      CHECK(b[3].kind == MdKind::ListItem && b[3].level == 1);
      CHECK(b[4].ordered && b[4].number == 1); CHECK(b[5].task == 1);
      CHECK(b[6].kind == MdKind::Quote); CHECK(b[7].kind == MdKind::Code && b[7].code == L"int x;" && b[7].lang == L"c");
      CHECK(b[8].kind == MdKind::Table && b[8].rows.size() == 2 && b[8].align[1] == 2);
      CHECK(b[9].kind == MdKind::Rule); }
    { // markup: inline wrap / unwrap per dialect
      auto e = WrapInline(Dialect::Markdown, Inline::Bold, L"hi"); CHECK(e.text == L"**hi**" && e.selStart == 2 && e.selEnd == 4);
      CHECK(WrapInline(Dialect::Markdown, Inline::Bold, L"**hi**").text == L"hi");
      CHECK(WrapInline(Dialect::BBCode, Inline::Italic, L"x").text == L"[i]x[/i]");
      CHECK(WrapInline(Dialect::Html, Inline::Strike, L"<s>x</s>").text == L"x");
      e = WrapInline(Dialect::Markdown, Inline::Link, L"site", L"https://"); CHECK(e.text == L"[site](https://)" && e.text.substr(e.selStart, e.selEnd - e.selStart) == L"https://");
      e = WrapInline(Dialect::Markdown, Inline::Bold, L""); CHECK(e.text == L"****" && e.selStart == 2);
      // line ops + toggle
      CHECK(ApplyLineOp(Dialect::Markdown, LineOp::Heading, L"Title", 2) == L"## Title");
      CHECK(ApplyLineOp(Dialect::Markdown, LineOp::Heading, L"## Title", 2) == L"Title");
      CHECK(ApplyLineOp(Dialect::Markdown, LineOp::Heading, L"# Title", 3) == L"### Title");
      CHECK(ApplyLineOp(Dialect::Markdown, LineOp::Number, L"a\nb") == L"1. a\n2. b");
      CHECK(ApplyLineOp(Dialect::Markdown, LineOp::Bullet, L"- a\n- b") == L"a\nb");
      CHECK(ApplyLineOp(Dialect::BBCode, LineOp::Bullet, L"a\nb") == L"[list]\n[*]a\n[*]b\n[/list]");
      CHECK(ApplyLineOp(Dialect::Html, LineOp::Quote, L"q") == L"<blockquote>q</blockquote>");
      CHECK(ClearFormatting(Dialect::Markdown, L"# **Bold** and [link](u)") == L"Bold and link");
      CHECK(MakeTable(Dialect::Markdown, 1, 2).find(L"| -------- | -------- |") != std::wstring::npos);
      // converters
      std::wstring html = MarkdownToHtml(L"# T\n\nA **b** [l](http://x)\n\n- one\n- two\n", false);
      CHECK(html.find(L"<h1>T</h1>") != std::wstring::npos); CHECK(html.find(L"<strong>b</strong>") != std::wstring::npos);
      CHECK(html.find(L"<a href=\"http://x\">l</a>") != std::wstring::npos); CHECK(html.find(L"<ul>\n<li>one</li>\n<li>two</li>\n</ul>") != std::wstring::npos);
      std::wstring bb = MarkdownToBBCode(L"## H\n\n*it* text\n\n1. a\n2. b\n");
      CHECK(bb.find(L"[h2]H[/h2]") != std::wstring::npos); CHECK(bb.find(L"[i]it[/i]") != std::wstring::npos); CHECK(bb.find(L"[olist]\n[*]a\n[*]b\n[/olist]") != std::wstring::npos);
      std::wstring md = BBCodeToMarkdown(L"[h1]Title[/h1]\n[b]bold[/b] and [url=http://a.b]link[/url]\n[list][*]x[*]y[/list]\n[quote=\"Bob\"]hey[/quote]\n[code][b]raw[/b][/code] [/stray]");
      CHECK(md.find(L"# Title") != std::wstring::npos); CHECK(md.find(L"**bold**") != std::wstring::npos); CHECK(md.find(L"[link](http://a.b)") != std::wstring::npos);
      CHECK(md.find(L"- x\n- y") != std::wstring::npos); CHECK(md.find(L"> hey") != std::wstring::npos); CHECK(md.find(L"```\n[b]raw[/b]\n```") != std::wstring::npos);
      CHECK(md.find(L"[/stray]") != std::wstring::npos);
      std::wstring md2 = HtmlToMarkdown(L"<html><body><h2>Hi &amp; bye</h2><p>Some <b>bold</b>\n text<br>next</p><ul><li>a<ul><li>b</li></ul></li></ul><table><tr><th>x</th></tr><tr><td>1</td></tr></table><script>bad()</script></body></html>");
      CHECK(md2.find(L"## Hi & bye") != std::wstring::npos); CHECK(md2.find(L"Some **bold** text") != std::wstring::npos);
      CHECK(md2.find(L"- a\n  - b") != std::wstring::npos); CHECK(md2.find(L"| x |\n| --- |\n| 1 |") != std::wstring::npos); CHECK(md2.find(L"bad") == std::wstring::npos);
      // round trip MD -> BB -> MD keeps structure
      std::wstring rt = BBCodeToMarkdown(MarkdownToBBCode(L"# A\n\n**b** c\n"));
      CHECK(rt.find(L"# A") != std::wstring::npos && rt.find(L"**b** c") != std::wstring::npos);
      // charts
      ChartSpec cs; CHECK(ParseChart(L"type: pie\ntitle: T\nA: 1\nB: 3.5\n", cs)); CHECK(cs.type == L"pie" && cs.data.size() == 2 && cs.data[1].second == 3.5);
      CHECK(ChartSvg(cs).find(L"<path") != std::wstring::npos);
      CHECK(MarkdownToHtml(L"```chart\nQ1: 3\nQ2: 5\n```\n", false).find(L"<svg") != std::wstring::npos);
      std::vector<Span> sp; int st = LexLine(L_BBCODE, L"[b]x[/b] [code]a", 0, sp); CHECK(st == 1 && sp[0].tok == T_TAG); }
    { // large-ish doc: length bookkeeping stays exact through edits
      TextDoc d; std::wstring big; for (int i = 0; i < 20000; ++i) big += L"line " + std::to_wstring(i) + L"\r\n";
      d.SetText(std::move(big)); size_t L = d.Length(); CHECK(L == d.Text().size());
      Pos z{0,0}; d.Replace({5,2},{900,3}, L"abc\ndef", z, z); CHECK(d.Length() == d.Text().size());
      CHECK(d.Offset({d.LineCount()-1, 0}) == d.Text().rfind(L'\n') + 1);
      Pos c, a; d.Undo(c, a); CHECK(d.Length() == L); CHECK(d.Offset({19999, 0}) == d.Text().find(L"line 19999"));
      Searcher s; SearchOpts o; o.pattern = L"LINE 1999"; s.Prepare(o); std::vector<Match> m; s.FindAll(d, m); CHECK(m.size() == 11);
      Match mm; CHECK(s.Find(d, d.Length(), true, mm) && mm.off == d.Text().find(L"line 1999"));   // wrap forward
      CHECK(s.Find(d, 0, false, mm) && mm.off == d.Text().rfind(L"line 1999")); }  // wrap backward
    { // CSV
      TextDoc d; d.SetText(L"name,qty,note\r\nR1,10,\"multi\r\nline, with comma\"\r\n\"Q \"\"x\"\"\",3,\r\nC5,,last\r\n");
      CHECK(DetectDelimiter(d) == L',');
      CsvTable t; t.Build(d, L',');
      CHECK(t.Rows() == 4); CHECK(t.Cols() == 3);
      CHECK(t.Value(d, 1, 2) == L"multi\nline, with comma"); CHECK(t.Value(d, 2, 0) == L"Q \"x\""); CHECK(t.Value(d, 2, 2) == L"");
      CHECK(t.Value(d, 3, 1) == L"");
      Pos a, b; std::wstring r; t.CellEdit(d, 1, 1, L"1,000", a, b, r); CHECK(r == L"\"1,000\"");
      d.Replace(a, b, r, a, a); t.Build(d, L','); CHECK(t.Value(d, 1, 1) == L"1,000"); CHECK(t.Value(d, 1, 2) == L"multi\nline, with comma");
      t.CellEdit(d, 3, 4, L"z", a, b, r); d.Replace(a, b, r, a, a); t.Build(d, L','); CHECK(t.Value(d, 3, 4) == L"z"); CHECK(t.Cols() == 5);
      t.CellEdit(d, 5, 1, L"new", a, b, r); d.Replace(a, b, r, a, a); t.Build(d, L','); CHECK(t.Rows() == 6); CHECK(t.Value(d, 5, 1) == L"new"); CHECK(t.Value(d, 4, 0) == L"");
      auto all = t.All(d); std::swap(all[1], all[2]);
      std::wstring ser = CsvTable::Serialize(all, L',');
      TextDoc d2; d2.SetText(ser); CsvTable t2; t2.Build(d2, L','); CHECK(t2.Value(d2, 2, 2) == L"multi\nline, with comma"); CHECK(t2.Value(d2, 1, 0) == L"Q \"x\"");
      TextDoc d3; d3.SetText(L"a;b;c\n1;2;3\n4;5;6"); CHECK(DetectDelimiter(d3) == L';');
      TextDoc d4; d4.SetText(L"a\tb\n1\t2"); CHECK(DetectDelimiter(d4) == L'\t');
      double x; CHECK(CsvIsNumber(L"$1,234.5", &x) && x == 1234.5); CHECK(!CsvIsNumber(L"R1"));
      std::vector<Span> sp; LexLine(L_CSV, L"a,b,c", 0, sp); CHECK(sp.size() == 5 && sp[0].tok != sp[2].tok); }
    { // live (formatted view) analysis
      TextDoc d; d.SetText(L"# Title #\n\n- item **bold** and *em*\n  - [x] done\n> quote\n---\n| A | B |\n|:--|--:|\n| 1 | 2 |\n| 3 | `a|b` |\n\n```c\nint x;\n```\n1. one [link](http://x.y/z)\ntext ![cat](file:///c:/a%20b.png) end");
      std::vector<LiveLine> L; ScanLive(d, L);
      CHECK(L[0].kind == LiveKind::Heading && L[0].level == 1 && L[0].prefix == 2);
      CHECK(L[1].kind == LiveKind::Blank); CHECK(L[2].kind == LiveKind::Bullet && L[2].markerAt == 0);
      CHECK(L[3].kind == LiveKind::Task && L[3].checked == 1 && L[3].markerAt == 2 && L[3].prefix == 8);
      CHECK(L[4].kind == LiveKind::Quote); CHECK(L[5].kind == LiveKind::Rule);
      CHECK(L[6].kind == LiveKind::TableHead && L[6].tableEnd == 9); CHECK(L[7].kind == LiveKind::TableHidden && L[9].kind == LiveKind::TableHidden);
      CHECK(L[11].kind == LiveKind::Fence && L[12].kind == LiveKind::Code && L[13].kind == LiveKind::Fence);
      CHECK(L[14].kind == LiveKind::Ordered);
      std::vector<LiveSpan> sp; LiveSpans(d.Line(0), L[0], sp);
      CHECK(sp.size() == 2 && sp[0].kind == LiveSpanKind::Hide && sp[0].len == 2 && sp[1].kind == LiveSpanKind::Hide && sp[1].a == 7);
      LiveSpans(d.Line(2), L[2], sp);
      int strong = -1, em = -1, hides = 0; for (auto& x : sp) { if (x.kind == LiveSpanKind::Strong) strong = x.a; if (x.kind == LiveSpanKind::Em) em = x.a; if (x.kind == LiveSpanKind::Hide) ++hides; }
      CHECK(sp[0].kind == LiveSpanKind::Bullet); CHECK(strong == 9 && em == 21 && hides == 4);
      LiveSpans(d.Line(14), L[14], sp);
      CHECK(sp.size() == 3 && sp[1].kind == LiveSpanKind::Link && sp[1].url == L"http://x.y/z" && d.Line(14).substr(sp[1].a, sp[1].len) == L"link");
      LiveSpans(d.Line(15), L[15], sp);
      CHECK(sp.size() == 1 && sp[0].kind == LiveSpanKind::Image && sp[0].alt == L"cat" && sp[0].url == L"file:///c:/a%20b.png");
      LiveTable t; CHECK(ParseLiveTable(d, 6, 9, t)); CHECK(t.Cols() == 2 && t.rows.size() == 3 && t.align[1] == 2 && t.rows[2][1] == L"`a|b`");
      t.rows[1][0] = L"longer"; std::wstring ser = SerializeLiveTable(t);
      CHECK(ser == L"| A      |     B |\n| :----- | ----: |\n| longer |     2 |\n| 3      | `a|b` |");
      TextDoc d2; d2.SetText(ser); std::vector<LiveLine> L2; ScanLive(d2, L2); CHECK(L2[0].kind == LiveKind::TableHead && L2[0].tableEnd == 3);
      CHECK(StripInline(L"**b** [x](y)") == L"b x");
      LiveSpans(L"a \\*not em\\* b", LiveLine{}, sp); CHECK(sp.size() == 2 && sp[0].kind == LiveSpanKind::Escape); }
    { // strict UTF-8 (RFC 3629)
      auto v = [](std::initializer_list<int> b) { std::vector<uint8_t> x; for (int c : b) x.push_back((uint8_t)c); return IsValidUtf8(x.data(), x.size()); };
      CHECK(v({ 0x41, 0xC3, 0xA9 })); CHECK(v({ 0xE2, 0x82, 0xAC })); CHECK(v({ 0xF0, 0x9F, 0x98, 0x80 })); CHECK(v({ 0xF4, 0x8F, 0xBF, 0xBF }));
      CHECK(!v({ 0xC0, 0xAF })); CHECK(!v({ 0xE0, 0x80, 0xAF })); CHECK(!v({ 0xF0, 0x80, 0x80, 0xAF }));   // overlong
      CHECK(!v({ 0xED, 0xA0, 0x80 })); CHECK(!v({ 0xED, 0xBF, 0xBF }));                                     // surrogates
      CHECK(!v({ 0xF4, 0x90, 0x80, 0x80 })); CHECK(!v({ 0xF5, 0x80, 0x80, 0x80 }));                         // > U+10FFFF
      CHECK(!AnsiCanEncode(L"\u4E2D")); CHECK(AnsiCanEncode(L"abc")); }
    { // own regex engine via Searcher
      TextDoc d; d.SetText(L"Name: Joe Smith\nName: Ann Lee\nAge: 42");
      Searcher s; SearchOpts o; o.regex = true; o.matchCase = true; o.pattern = L"^Name: (\\w+) (\\w+)$"; CHECK(s.Prepare(o));
      std::vector<Match> m; s.FindAll(d, m); CHECK(m.size() == 2);
      CHECK(s.ReplaceAll(d, L"$2, $1", {0,0}) == 2); CHECK(d.Line(0) == L"Smith, Joe" && d.Line(1) == L"Lee, Ann");
      o.pattern = L"(a|b"; std::wstring err; CHECK(!s.Prepare(o, &err) && !err.empty());
      o.pattern = L"(x)\\1"; CHECK(!s.Prepare(o, &err));
      TextDoc big; std::wstring t(300000, L'a'); big.SetText(t + L"!");
      o.pattern = L"(a+)+$"; CHECK(s.Prepare(o)); s.FindAll(big, m); CHECK(!s.TimedOut() && m.size() == 0);   // no catastrophic backtracking
      o.pattern = L".*"; CHECK(s.Prepare(o)); s.FindAll(big, m); CHECK(m.size() == 1 && m[0].len == 300001);       // no stack overflow
      o.pattern = L"cat"; o.wholeWord = true; o.matchCase = false; CHECK(s.Prepare(o)); TextDoc w; w.SetText(L"Cat concat cat."); s.FindAll(w, m); CHECK(m.size() == 2); }
    printf("%d/%d passed\n", total - fails, total);
    return fails != 0;
}
