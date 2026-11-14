#include "search.h"
#include <algorithm>
#include <cwctype>
#include <chrono>

namespace np {

static long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
long long Searcher::Deadline() const { return budgetMs > 0 ? NowMs() + budgetMs : 0; }

bool Searcher::Prepare(const SearchOpts& o, std::wstring* error) {
    o_ = o; ready_ = false; hayVer_ = UINT64_MAX;
    if (o.pattern.empty()) return false;
    if (o.regex) {
        std::wstring p = o.pattern, err;
        if (o.wholeWord) p = L"\\b(?:" + p + L")\\b";
        if (!rx_.Compile(p, !o.matchCase, &err)) { if (error) *error = L"Invalid regex: " + err; return false; }
    } else {
        foldPat_ = o.pattern;
        if (!o.matchCase) for (auto& c : foldPat_) c = (wchar_t)towlower(c);
    }
    ready_ = true;
    return true;
}

const std::wstring& Searcher::Hay(const TextDoc& doc) {
    if (hayDoc_ != &doc || hayVer_ != doc.Version()) {
        hay_ = doc.Text(); hayVer_ = doc.Version(); hayDoc_ = &doc;
        if (!o_.regex && !o_.matchCase) { foldHay_ = hay_; for (auto& c : foldHay_) c = (wchar_t)towlower(c); }
    }
    return (!o_.regex && !o_.matchCase) ? foldHay_ : hay_;
}

bool Searcher::MatchAt(const std::wstring& hay, size_t i) const {
    if (!o_.wholeWord) return true;
    size_t e = i + foldPat_.size();
    if (i > 0 && IsWordChar(hay[i - 1])) return false;
    if (e < hay.size() && IsWordChar(hay[e])) return false;
    return true;
}

const std::wstring& Searcher::Fold(const std::wstring& line) {
    if (o_.matchCase) return line;
    fold_.resize(line.size());
    for (size_t i = 0; i < line.size(); ++i) fold_[i] = (wchar_t)towlower(line[i]);
    return fold_;
}
// plain search inside one line: forward = first match at >= from, backward = last match starting <= from
bool Searcher::InLine(const std::wstring& line, size_t from, bool forward, size_t& at) {
    if (line.size() < foldPat_.size()) return false;
    const std::wstring& h = Fold(line);
    if (forward) {
        for (size_t i = from; (i = h.find(foldPat_, i)) != std::wstring::npos; ++i) if (MatchAt(h, i)) { at = i; return true; }
        return false;
    }
    size_t i = std::min(from, h.size() - foldPat_.size());
    for (;;) {
        i = h.rfind(foldPat_, i);
        if (i == std::wstring::npos) return false;
        if (MatchAt(h, i)) { at = i; return true; }
        if (i == 0) return false;
        --i;
    }
}

bool Searcher::Find(const TextDoc& doc, size_t from, bool forward, Match& m) {
    if (!ready_) return false;
    if (LineWise()) {
        Pos p = doc.PosAt(std::min(from, doc.Length()));
        int n = doc.LineCount();
        size_t at;
        if (forward) {
            for (int k = 0; k <= n; ++k) {
                int l = (p.line + k) % n;
                if (k > 0 && l == 0 && !o_.wrap) return false;
                size_t start = k == 0 ? (size_t)p.col : 0;
                if (k == n) { if (InLine(doc.Line(l), 0, true, at) && at < (size_t)p.col) { m = { doc.Offset({ l, (int)at }), foldPat_.size() }; return true; } return false; }
                if (InLine(doc.Line(l), start, true, at)) { m = { doc.Offset({ l, (int)at }), foldPat_.size() }; return true; }
            }
            return false;
        }
        for (int k = 0; k <= n; ++k) {
            int l = ((p.line - k) % n + n) % n;
            if (k > 0 && l == n - 1 && !o_.wrap) return false;
            const std::wstring& line = doc.Line(l);
            if (k == 0) {
                if ((size_t)p.col >= foldPat_.size() && InLine(line, p.col - foldPat_.size(), false, at)) { m = { doc.Offset({ l, (int)at }), foldPat_.size() }; return true; }
                continue;
            }
            if (InLine(line, line.size(), false, at)) { m = { doc.Offset({ l, (int)at }), foldPat_.size() }; return true; }
        }
        return false;
    }
    const std::wstring& h = Hay(doc);
    from = std::min(from, h.size());
    if (o_.regex) {
        timedOut_ = false;
        long long dl = Deadline();
        ReMatch rm;
        if (forward) {
            for (size_t at = from;;) {   // first non-empty match starting >= from, then wrap
                int rc = rx_.Search(h.c_str(), h.size(), at, rm, dl);
                if (rc < 0) { timedOut_ = true; return false; }
                if (rc == 0) break;
                if (rm.len) { m = { rm.off, rm.len }; return true; }
                at = rm.off + 1; if (at > h.size()) break;
            }
            if (!o_.wrap || from == 0) return false;
            for (size_t at = 0;;) {
                int rc = rx_.Search(h.c_str(), h.size(), at, rm, dl);
                if (rc < 0) { timedOut_ = true; return false; }
                if (rc == 0 || rm.off >= from) return false;
                if (rm.len) { m = { rm.off, rm.len }; return true; }
                at = rm.off + 1;
            }
        }
        // backward: last match ending <= from, else (wrap) the last match overall
        Match last, lastAll; bool got = false, any = false;
        for (size_t at = 0; at <= h.size();) {
            int rc = rx_.Search(h.c_str(), h.size(), at, rm, dl);
            if (rc < 0) { timedOut_ = true; break; }
            if (rc == 0) break;
            if (rm.len) { Match x{ rm.off, rm.len }; if (x.off + x.len <= from) { last = x; got = true; } lastAll = x; any = true; }
            at = rm.off + std::max<size_t>(rm.len, 1);
        }
        if (got) { m = last; return true; }
        if (o_.wrap && any) { m = lastAll; return true; }
        return false;
    }
    const std::wstring& p = foldPat_;
    if (forward) {
        size_t i = from;
        for (int pass = 0; pass < 2; ++pass) {
            while ((i = h.find(p, i)) != std::wstring::npos) {
                if (pass == 1 && i >= from) return false;
                if (MatchAt(h, i)) { m = { i, p.size() }; return true; }
                ++i;
            }
            if (!o_.wrap) return false;
            i = 0;
        }
        return false;
    }
    if (from < p.size()) { if (!o_.wrap) return false; from = h.size(); }
    size_t i = from - p.size();
    for (int pass = 0; pass < 2; ++pass) {
        for (;;) {
            i = h.rfind(p, i);
            if (i == std::wstring::npos) break;
            if (MatchAt(h, i)) { m = { i, p.size() }; return true; }
            if (i == 0) break;
            --i;
        }
        if (!o_.wrap || h.size() < p.size()) return false;
        i = h.size() - p.size();
    }
    return false;
}

void Searcher::FindAll(const TextDoc& doc, std::vector<Match>& out, size_t cap) {
    out.clear();
    if (!ready_) return;
    if (LineWise()) {
        size_t base = 0;
        for (int l = 0; l < doc.LineCount() && out.size() < cap; ++l) {
            const std::wstring& line = doc.Line(l);
            if (line.size() >= foldPat_.size()) {
                const std::wstring& h = Fold(line);
                for (size_t i = 0; out.size() < cap && (i = h.find(foldPat_, i)) != std::wstring::npos;) {
                    if (MatchAt(h, i)) { out.push_back({ base + i, foldPat_.size() }); i += foldPat_.size(); } else ++i;
                }
            }
            base += line.size() + 1;
        }
        return;
    }
    const std::wstring& h = Hay(doc);
    if (o_.regex) {
        timedOut_ = false;
        long long dl = Deadline();
        ReMatch rm;
        for (size_t at = 0; at <= h.size() && out.size() < cap;) {
            int rc = rx_.Search(h.c_str(), h.size(), at, rm, dl);
            if (rc < 0) { timedOut_ = true; break; }
            if (rc == 0) break;
            if (rm.len) out.push_back({ rm.off, rm.len });
            at = rm.off + std::max<size_t>(rm.len, 1);
        }
        return;
    }
    size_t i = 0;
    while (out.size() < cap && (i = h.find(foldPat_, i)) != std::wstring::npos) {
        if (MatchAt(h, i)) { out.push_back({ i, foldPat_.size() }); i += foldPat_.size(); }
        else ++i;
    }
}

std::wstring Searcher::Expand(const TextDoc& doc, const Match& m, const std::wstring& repl) {
    if (!o_.regex) return repl;
    const std::wstring& h = Hay(doc);
    ReMatch rm;
    if (rx_.MatchExact(h.c_str(), h.size(), m.off, m.off + m.len, rm)) return Regex::Format(repl, h.c_str(), h.size(), rm);
    return repl;
}

int Searcher::ReplaceAll(TextDoc& doc, const std::wstring& repl, Pos caret) {
    std::vector<Match> all;
    long long keep = budgetMs; budgetMs = budgetMs > 0 ? budgetMs * 4 : 0;   // replace-all gets a bigger budget...
    FindAll(doc, all, SIZE_MAX);
    budgetMs = keep;
    if (timedOut_) return -1;   // ...but never replaces only part of the document
    if (all.empty()) return 0;
    // build new text in one pass, apply as one edit (single undo step, fast)
    Hay(doc);
    const std::wstring h = hay_;
    std::wstring out; out.reserve(h.size());
    size_t last = 0;
    for (auto& m : all) {
        out.append(h, last, m.off - last);
        out += Expand(doc, m, repl);
        last = m.off + m.len;
    }
    out.append(h, last, std::wstring::npos);
    doc.BeginGroup();
    doc.Replace({ 0, 0 }, doc.End(), out, caret, caret);
    doc.EndGroup();
    return (int)all.size();
}

} // namespace np
