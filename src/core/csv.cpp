#include "csv.h"
#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace np {

wchar_t DetectDelimiter(const TextDoc& d) {
    static const wchar_t cand[] = { L',', L';', L'\t', L'|' };
    int n = std::min(d.LineCount(), 50);
    wchar_t best = L','; double bestScore = -1;
    for (wchar_t c : cand) {
        std::vector<int> counts;
        for (int i = 0; i < n; ++i) {
            const std::wstring& l = d.Line(i);
            if (l.empty()) continue;
            int k = 0; bool q = false;
            for (wchar_t ch : l) { if (ch == L'"') q = !q; else if (ch == c && !q) ++k; }
            counts.push_back(k);
        }
        if (counts.empty()) continue;
        int mode = 0, freq = 0;
        for (int v : counts) { int f = (int)std::count(counts.begin(), counts.end(), v); if (f > freq || (f == freq && v > mode)) { freq = f; mode = v; } }
        if (mode == 0) continue;
        double score = (double)freq / counts.size() * 10 + std::min(mode, 50) * 0.01;
        if (score > bestScore) { bestScore = score; best = c; }
    }
    return best;
}

std::wstring CsvQuote(const std::wstring& v, wchar_t delim) {
    bool need = v.find_first_of(std::wstring(L"\"\n\r") + delim) != std::wstring::npos ||
                (!v.empty() && (v.front() == L' ' || v.back() == L' '));
    if (!need) return v;
    std::wstring o = L"\"";
    for (wchar_t c : v) { if (c == L'"') o += L"\"\""; else o += c; }
    return o + L"\"";
}

bool CsvIsNumber(const std::wstring& raw, double* out) {
    std::wstring v;
    for (wchar_t c : raw) if (c != L',' && c != L' ' && c != L'$' && c != L'%') v += c;   // 1,234 / $5 / 7%
    if (v.empty()) return false;
    wchar_t* e = nullptr;
    double x = wcstod(v.c_str(), &e);
    if (e == v.c_str() || *e) return false;
    if (out) *out = x;
    return true;
}

void CsvTable::Build(const TextDoc& d, wchar_t dl) {
    delim = dl;
    recLine_.clear(); cache_.clear();
    bool q = false;
    int n = d.LineCount();
    recLine_.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (!q) recLine_.push_back(i);
        for (wchar_t c : d.Line(i)) if (c == L'"') q = !q;
    }
    // trailing empty line (file ends with a newline) is not a record
    if (!recLine_.empty() && recLine_.back() == n - 1 && d.Line(n - 1).empty() && recLine_.size() > 1) recLine_.pop_back();
    if (n == 1 && d.Line(0).empty()) recLine_.clear();
    cols_ = 0;
    std::vector<CsvCell> tmp;
    int probe = std::min((int)recLine_.size(), 5000);
    for (int r = 0; r < probe; ++r) { Parse(d, r, tmp); cols_ = std::max(cols_, (int)tmp.size()); }
    if (probe < (int)recLine_.size()) {   // cheap pass for the rest: count delimiters on single-line records
        for (int r = probe; r < (int)recLine_.size(); ++r) {
            const std::wstring& l = d.Line(recLine_[r]);
            int k = 1; bool qq = false;
            for (wchar_t c : l) { if (c == L'"') qq = !qq; else if (c == delim && !qq) ++k; }
            cols_ = std::max(cols_, k);
        }
    }
    ver_ = d.Version();
}

Pos CsvTable::RecordEnd(const TextDoc& d, int r) const {
    int last = (r + 1 < (int)recLine_.size()) ? recLine_[r + 1] - 1 : d.LineCount() - 1;
    if (r + 1 >= (int)recLine_.size()) {   // last record: skip a trailing empty line
        while (last > recLine_[r] && d.Line(last).empty()) --last;
    }
    return { last, (int)d.Line(last).size() };
}

void CsvTable::Parse(const TextDoc& d, int r, std::vector<CsvCell>& out) const {
    out.clear();
    Pos end = RecordEnd(d, r);
    Pos p{ recLine_[r], 0 };
    auto at = [&](Pos x) -> wchar_t {
        const std::wstring& l = d.Line(x.line);
        return x.col < (int)l.size() ? l[x.col] : L'\n';
    };
    auto next = [&](Pos x) -> Pos {
        if (x.col < (int)d.Line(x.line).size()) return { x.line, x.col + 1 };
        return { x.line + 1, 0 };
    };
    for (;;) {
        CsvCell c; c.a = p;
        if (!(p < end) ) { c.b = p; out.push_back(c); break; }
        if (at(p) == L'"') {
            p = next(p);
            while (p < end) {
                wchar_t ch = at(p);
                if (ch == L'"') {
                    Pos n2 = next(p);
                    if (n2 < end && at(n2) == L'"') { c.v += L'"'; p = next(n2); continue; }
                    p = n2; break;
                }
                c.v += ch; p = next(p);
            }
            while (p < end && at(p) != delim) { c.v += at(p); p = next(p); }   // junk after the closing quote: keep
        } else {
            while (p < end && at(p) != delim) { c.v += at(p); p = next(p); }
        }
        c.b = p;
        out.push_back(c);
        if (!(p < end)) break;
        p = next(p);   // skip delimiter
        if (!(p < end)) { CsvCell e; e.a = e.b = p; out.push_back(e); break; }   // trailing delimiter -> empty last field
    }
}

const std::vector<CsvCell>& CsvTable::Row(const TextDoc& d, int r) {
    auto it = cache_.find(r);
    if (it != cache_.end()) return it->second;
    if (cache_.size() > 4000) cache_.clear();
    auto& v = cache_[r];
    Parse(d, r, v);
    return v;
}
std::wstring CsvTable::Value(const TextDoc& d, int r, int c) {
    if (r < 0 || r >= Rows()) return L"";
    const auto& row = Row(d, r);
    return c < (int)row.size() ? row[c].v : L"";
}

void CsvTable::CellEdit(const TextDoc& d, int r, int c, const std::wstring& v, Pos& a, Pos& b, std::wstring& repl) {
    std::wstring q = CsvQuote(v, delim);
    if (r >= Rows()) {   // append new record(s)
        a = b = d.End();
        repl.clear();
        int add = r - Rows() + 1;
        bool needNl = !(d.LineCount() == 1 && d.Line(0).empty()) && !d.Line(d.LineCount() - 1).empty();
        for (int i = 0; i < add; ++i) { if (needNl || i > 0) repl += L'\n'; needNl = true; if (i == add - 1) repl += std::wstring(c, delim) + q; }
        return;
    }
    const auto& row = Row(d, r);
    if (c < (int)row.size()) { a = row[c].a; b = row[c].b; repl = q; return; }
    a = b = RecordEnd(d, r);
    repl = std::wstring(c - row.size() + 1, delim) + q;
    if (row.size() == 1 && row[0].a == row[0].b) repl = std::wstring(c, delim) + q;   // empty record
}

std::vector<std::vector<std::wstring>> CsvTable::All(const TextDoc& d) {
    std::vector<std::vector<std::wstring>> o(Rows());
    std::vector<CsvCell> tmp;
    for (int r = 0; r < Rows(); ++r) { Parse(d, r, tmp); for (auto& c : tmp) o[r].push_back(c.v); }
    return o;
}
std::wstring CsvTable::Serialize(const std::vector<std::vector<std::wstring>>& rows, wchar_t delim) {
    std::wstring o;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r) o += L'\n';
        for (size_t c = 0; c < rows[r].size(); ++c) { if (c) o += delim; o += CsvQuote(rows[r][c], delim); }
    }
    return o;
}

} // namespace np
