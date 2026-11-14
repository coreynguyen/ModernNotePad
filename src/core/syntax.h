// syntax.h - line-incremental lexers (state carried line to line), language
// detection, fold-region computation. Portable C++17.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace np {

class TextDoc;

enum Tok : uint8_t {
    T_TEXT, T_KEYWORD, T_TYPE, T_STRING, T_NUMBER, T_COMMENT, T_PREPROC, T_OPERATOR,
    T_FUNC, T_TAG, T_ATTR, T_KEY, T_HEADING, T_EMPH, T_STRONG, T_CODE, T_LINK, T_QUOTE, T_LIST,
    T_COUNT
};

struct Span { int start, len; Tok tok; };

enum Lang : uint8_t {
    L_TEXT, L_C, L_CPP, L_CS, L_JAVA, L_JS, L_TS, L_RUST, L_GO, L_PY, L_JSON, L_XML, L_HTML, L_CSS,
    L_MD, L_INI, L_YAML, L_SH, L_PS, L_BAT, L_SQL, L_LUA, L_ASM, L_BBCODE, L_CSV, L_COUNT
};

const wchar_t* LangName(Lang l);
Lang LangFromPath(const std::wstring& path);
Lang LangFromContent(const std::wstring& firstLine);

// Lex one line. stateIn/return value: 0 = normal, else language-specific continuation.
int LexLine(Lang lang, const std::wstring& line, int stateIn, std::vector<Span>& out);

// True when spans at col are "prose" (spell-checkable): comments/strings in code, everything in text/markdown.
bool IsProseTok(Lang lang, Tok t);

// Incremental per-line state cache for a document.
class LexCache {
public:
    void Reset() { states_.clear(); validTo_ = 0; }
    void Invalidate(int fromLine) { if (fromLine < validTo_) validTo_ = fromLine < 0 ? 0 : fromLine; }
    int StateAt(const TextDoc& doc, Lang lang, int line);   // state at start of `line`
private:
    std::vector<int> states_;   // states_[i] = state entering line i
    int validTo_ = 0;           // states_[0..validTo_] valid
    std::vector<Span> tmp_;
};

struct FoldRegion { int start, end; };   // header line `start`, hidden lines start+1..end
// Computes fold regions for the whole document (brace, indentation or heading based by language).
void ComputeFolds(const TextDoc& doc, Lang lang, LexCache& lex, std::vector<FoldRegion>& out);

// Block comment / line comment markers for toggle-comment.
const wchar_t* LineCommentPrefix(Lang l);

} // namespace np
