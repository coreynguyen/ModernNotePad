// search.h - plain / whole-word / regex find & replace over a TextDoc. Regex uses np::Regex (own Pike-VM
// engine: linear time, no recursion, time-budgeted) instead of std::wregex.
#pragma once
#include "text_doc.h"
#include "regex.h"
#include <string>
#include <vector>

namespace np {

struct SearchOpts {
    std::wstring pattern;
    bool matchCase = false, wholeWord = false, regex = false, wrap = true;
};

struct Match { size_t off = 0, len = 0; };

class Searcher {
public:
    // Returns false and fills error when the regex fails to compile.
    bool Prepare(const SearchOpts& o, std::wstring* error = nullptr);
    // Search in the doc text. `from` is an offset; forward finds first match starting >= from,
    // backward finds last match ending <= from.
    bool Find(const TextDoc& doc, size_t from, bool forward, Match& m);
    // All matches (capped), used for highlight + "n of N".
    void FindAll(const TextDoc& doc, std::vector<Match>& out, size_t cap = 100000);
    // Replacement text for a match found at m (supports $1, $&, $$ in regex mode).
    std::wstring Expand(const TextDoc& doc, const Match& m, const std::wstring& repl);
    // Replace all; returns count. One undo group.
    int ReplaceAll(TextDoc& doc, const std::wstring& repl, Pos caret);
    const SearchOpts& Opts() const { return o_; }
    // true when the last Find/FindAll gave up on its time budget (regex only; the UI says so)
    bool TimedOut() const { return timedOut_; }
    long long budgetMs = 1500;   // per Find / FindAll call
    bool Ready() const { return ready_; }
private:
    const std::wstring& Hay(const TextDoc& doc);
    long long Deadline() const;
    bool MatchAt(const std::wstring& hay, size_t i) const;
    bool LineWise() const { return !o_.regex && o_.pattern.find(L'\n') == std::wstring::npos; }
    const std::wstring& Fold(const std::wstring& line);
    bool InLine(const std::wstring& line, size_t from, bool forward, size_t& at);
    std::wstring fold_;
    SearchOpts o_;
    Regex rx_;
    bool timedOut_ = false;
    bool ready_ = false;
    std::wstring hay_, foldPat_, foldHay_;
    uint64_t hayVer_ = UINT64_MAX;
    const TextDoc* hayDoc_ = nullptr;
};

} // namespace np
