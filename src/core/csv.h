// csv.h - CSV/TSV model over a TextDoc (RFC 4180: quoted fields, "" escapes, newlines inside quotes).
// The TextDoc stays the single source of truth: the table is an index of record start lines plus
// on-demand row parsing; every edit becomes a TextDoc range replacement (so undo/redo/save just work).
#pragma once
#include "text_doc.h"
#include <map>
#include <string>
#include <vector>

namespace np {

struct CsvCell { Pos a, b; std::wstring v; };   // [a,b) = raw source range (quotes included)

wchar_t DetectDelimiter(const TextDoc& d);       // ',' ';' '\t' '|' by consistency over the first lines
std::wstring CsvQuote(const std::wstring& v, wchar_t delim);
bool CsvIsNumber(const std::wstring& v, double* out = nullptr);

class CsvTable {
public:
    wchar_t delim = L',';
    void Build(const TextDoc& d, wchar_t delim);            // O(lines)
    bool Stale(const TextDoc& d) const { return ver_ != d.Version(); }
    int Rows() const { return (int)recLine_.size(); }
    int Cols() const { return cols_; }
    const std::vector<CsvCell>& Row(const TextDoc& d, int r);   // parsed + cached
    std::wstring Value(const TextDoc& d, int r, int c);
    int RecordLine(int r) const { return recLine_[r]; }
    Pos RecordEnd(const TextDoc& d, int r) const;

    // Edits: compute the TextDoc replacement for setting one cell (row r may equal Rows() to append)
    void CellEdit(const TextDoc& d, int r, int c, const std::wstring& v, Pos& a, Pos& b, std::wstring& repl);
    // Whole-table helpers for structural ops (sort, insert/delete column)
    std::vector<std::vector<std::wstring>> All(const TextDoc& d);
    static std::wstring Serialize(const std::vector<std::vector<std::wstring>>& rows, wchar_t delim);

private:
    void Parse(const TextDoc& d, int r, std::vector<CsvCell>& out) const;
    std::vector<int> recLine_;
    int cols_ = 0;
    uint64_t ver_ = UINT64_MAX;
    std::map<int, std::vector<CsvCell>> cache_;
};

} // namespace np
