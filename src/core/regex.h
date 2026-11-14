// regex.h - own regular-expression engine for Find/Replace (replaces std::wregex).
//
// Why: std::wregex is a recursive backtracker. On large documents it overflows the stack (a crash, not
// just slowness - e.g. ".*" or "(a|b)*" over a few MB) and pathological patterns ("(a+)+$") take
// exponential time with no way to cancel. This is a Pike VM (Thompson NFA simulation with capture
// slots): time is O(text x pattern), memory O(pattern), nothing recursive, and a deadline is checked
// while scanning, so a search always returns.
//
// Syntax (ECMAScript subset): literals, . [] [^] ranges, \d \D \w \W \s \S \b \B, \t \n \r \f \v \0
// \xHH \uHHHH, escaped metachars, ^ $ (line anchors), groups ( ), non-capturing (?: ), alternation |,
// * + ? {n} {n,} {n,m} and their lazy forms (*? +? ?? {n,m}?). Not supported (compile error with a clear
// message): backreferences \1 and lookaround (?= (?! (?<= (?<! - they need backtracking.
// Match semantics follow backtracking engines: leftmost, then greedy/lazy priority (same results as
// std::regex for the supported syntax).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace np {

struct ReMatch {
    size_t off = 0, len = 0;
    std::vector<std::pair<long long, long long>> groups;   // [0] = whole match; -1 = group did not participate
};

class Regex {
public:
    // false + message on syntax error
    bool Compile(const std::wstring& pattern, bool ignoreCase, std::wstring* error = nullptr);
    bool Ok() const { return !prog_.empty(); }
    int Groups() const { return ncap_ / 2; }
    // Leftmost match starting at or after `start` in [text, text+n). Matches of length zero are reported.
    // deadlineMs: 0 = none; otherwise steady-clock milliseconds at which to give up.
    // Returns 1 match, 0 no match, -1 gave up (deadline).
    int Search(const wchar_t* text, size_t n, size_t start, ReMatch& m, long long deadlineMs = 0) const;
    // Exact match of the whole [b, e) (used to expand replacements)
    bool MatchExact(const wchar_t* text, size_t n, size_t b, size_t e, ReMatch& m) const;
    // $& whole match, $1..$99 groups, $$ dollar, $` prefix, $' suffix
    static std::wstring Format(const std::wstring& repl, const wchar_t* text, size_t n, const ReMatch& m);

    struct Inst { uint8_t op; int32_t x = 0, y = 0; };
private:
    std::vector<Inst> prog_;
    std::vector<std::vector<std::pair<wchar_t, wchar_t>>> classes_;
    std::vector<bool> classNeg_;
    int ncap_ = 2;
    bool icase_ = false;
    bool anchoredStart_ = false;   // pattern begins with ^ : only line starts can match
    friend struct ReCompiler;
    int Run(const wchar_t* text, size_t n, size_t start, size_t limitStart, bool anchoredEnd, size_t endAt, ReMatch& m, long long deadlineMs) const;
};

} // namespace np
