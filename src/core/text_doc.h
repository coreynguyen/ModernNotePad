// text_doc.h - line-based text buffer with grouped multi-level undo/redo,
// per-line stamps (cache keys that travel with the line) and per-line flags.
// Portable C++17: no Win32 here, unit-tested on Linux.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace np {

struct Pos {
    int line = 0, col = 0;
    bool operator==(const Pos& o) const { return line == o.line && col == o.col; }
    bool operator!=(const Pos& o) const { return !(*this == o); }
    bool operator<(const Pos& o) const { return line < o.line || (line == o.line && col < o.col); }
    bool operator<=(const Pos& o) const { return !(o < *this); }
};

enum class Eol : uint8_t { CRLF, LF, CR };
enum class Enc : uint8_t { UTF8, UTF8BOM, UTF16LE, UTF16BE, ANSI };

const wchar_t* EolName(Eol e);   // "Windows (CRLF)" ...
const wchar_t* EncName(Enc e);   // "UTF-8" ...

// Line flag bits (travel with lines across inserts/deletes)
enum : uint8_t { LF_FOLDED = 1, LF_BOOKMARK = 2 };

struct EditRec {
    Pos start;
    std::wstring removed, inserted;  // '\n' separated
    Pos caretBefore, anchorBefore, caretAfter;
    uint32_t group = 0;
    uint64_t time = 0;               // ms tick, for typing coalescing
};

class TextDoc {
public:
    TextDoc();

    // ---- content
    int LineCount() const { return (int)lines_.size(); }
    const std::wstring& Line(int i) const { return lines_[i]; }
    uint32_t Stamp(int i) const { return stamp_[i]; }
    uint8_t& Flags(int i) { return flags_[i]; }
    uint8_t Flags(int i) const { return flags_[i]; }
    uint64_t Version() const { return version_; }
    int DirtyFrom() const { return dirtyFrom_; }        // lowest line touched since ClearDirty()
    void ClearDirty() { dirtyFrom_ = 1 << 30; }

    std::wstring Text() const;                           // '\n' joined
    std::wstring GetText(Pos a, Pos b) const;
    size_t Length() const;                               // chars incl. '\n' between lines
    size_t Offset(Pos p) const;
    Pos PosAt(size_t off) const;
    Pos Clamp(Pos p) const;
    Pos End() const { return { LineCount() - 1, (int)lines_.back().size() }; }

    // ---- editing (all undoable). Text may contain \r\n, \r or \n; normalized.
    Pos Replace(Pos a, Pos b, const std::wstring& text, Pos caretBefore, Pos anchorBefore, bool coalesceTyping = false);
    void SetText(const std::wstring& text);              // resets undo, marks saved
    void SetText(std::wstring&& text);                   // same, consumes/frees the buffer early (large files)
    void BeginGroup() { if (groupDepth_++ == 0) curGroup_ = ++groupSeq_; }
    void EndGroup() { if (groupDepth_ > 0) --groupDepth_; }

    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    bool Undo(Pos& caret, Pos& anchor);
    bool Redo(Pos& caret, Pos& anchor);
    size_t UndoDepth() const { return undo_.size(); }
    void BreakCoalesce() { breakCoalesce_ = true; }

    // ---- save tracking
    bool Modified() const { return savedSeq_ != CurSeq(); }
    void MarkSaved() { savedSeq_ = CurSeq(); }
    void MarkModified() { savedSeq_ = UINT64_MAX; }

    Eol eol = Eol::CRLF;
    Enc enc = Enc::UTF8;

    static uint64_t (*Clock)();                          // injectable ms clock

private:
    Pos RawInsert(Pos p, const std::wstring& s);         // s normalized
    void RawErase(Pos a, Pos b);
    void Touch(int line) { if (line < dirtyFrom_) dirtyFrom_ = line; if (line < lineStartValid_) lineStartValid_ = line; ++version_; }
    uint64_t CurSeq() const { return undo_.empty() ? 0 : undoSeq_.back(); }

    std::vector<std::wstring> lines_;
    std::vector<uint32_t> stamp_;
    std::vector<uint8_t> flags_;
    std::vector<EditRec> undo_, redo_;
    std::vector<uint64_t> undoSeq_, redoSeq_;            // identity of each undo state
    uint64_t seqCounter_ = 0, savedSeq_ = 0;
    uint64_t version_ = 0;
    uint32_t stampCounter_ = 0;
    int dirtyFrom_ = 0;
    uint32_t groupSeq_ = 0, curGroup_ = 0;
    int groupDepth_ = 0;
    bool breakCoalesce_ = false;
    mutable std::vector<size_t> lineStart_;
    mutable int lineStartValid_ = 0;                     // lineStart_[0..valid) correct
    size_t totalChars_ = 0;                              // sum of line lengths (no separators)
};

std::wstring NormalizeNewlines(const std::wstring& s);
void NormalizeNewlinesInPlace(std::wstring& s);

// ---- codecs
struct Decoded {
    std::wstring text;   // '\n' line breaks
    Enc enc = Enc::UTF8;
    Eol eol = Eol::CRLF;
    bool binary = false; // looks like non-text: open in hex editor
    bool mixedEol = false;
};
Decoded DecodeBytes(const std::vector<uint8_t>& bytes);
std::wstring DecodeAs(const std::vector<uint8_t>& bytes, Enc enc);   // no EOL normalization
std::vector<uint8_t> EncodeText(const std::wstring& textLF, Enc enc, Eol eol);
bool IsValidUtf8(const uint8_t* p, size_t n);   // strict RFC 3629
bool AnsiCanEncode(const std::wstring& s);       // false if saving as ANSI would replace characters
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// word / char classes
bool IsWordChar(wchar_t c);

} // namespace np
