#include "text_doc.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace np {

static uint64_t DefaultClock() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
uint64_t (*TextDoc::Clock)() = DefaultClock;

const wchar_t* EolName(Eol e) {
    switch (e) { case Eol::CRLF: return L"Windows (CRLF)"; case Eol::LF: return L"Unix (LF)"; default: return L"Macintosh (CR)"; }
}
const wchar_t* EncName(Enc e) {
    switch (e) {
    case Enc::UTF8: return L"UTF-8"; case Enc::UTF8BOM: return L"UTF-8 with BOM";
    case Enc::UTF16LE: return L"UTF-16 LE"; case Enc::UTF16BE: return L"UTF-16 BE";
    default: return L"ANSI";
    }
}

bool IsWordChar(wchar_t c) { return c == L'_' || iswalnum(c) || (c >= 0x00C0 && c != 0x00D7 && c != 0x00F7 && iswalpha(c)); }

std::wstring NormalizeNewlines(const std::wstring& s) {
    if (s.find(L'\r') == std::wstring::npos) return s;
    std::wstring o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\r') { o += L'\n'; if (i + 1 < s.size() && s[i + 1] == L'\n') ++i; }
        else o += s[i];
    }
    return o;
}

void NormalizeNewlinesInPlace(std::wstring& s) {
    size_t r = s.find(L'\r');
    if (r == std::wstring::npos) return;
    size_t w = r;
    for (size_t i = r; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'\r') { s[w++] = L'\n'; if (i + 1 < s.size() && s[i + 1] == L'\n') ++i; }
        else s[w++] = c;
    }
    s.resize(w);
}

TextDoc::TextDoc() { SetText(L""); }

void TextDoc::SetText(const std::wstring& text) { SetText(std::wstring(text)); }
void TextDoc::SetText(std::wstring&& t) {
    NormalizeNewlinesInPlace(t);
    size_t n = 1; for (wchar_t c : t) n += c == L'\n';
    std::vector<std::wstring> ls; ls.reserve(n);
    const wchar_t* p = t.data(); const wchar_t* e = p + t.size();
    for (;;) {
        const wchar_t* q = std::find(p, e, L'\n');
        ls.emplace_back(p, q);
        if (q == e) break;
        p = q + 1;
    }
    std::wstring().swap(t);   // release the big buffer before allocating stamps
    lines_.swap(ls);
    stamp_.resize(lines_.size()); for (auto& x : stamp_) x = ++stampCounter_;
    flags_.assign(lines_.size(), 0);
    totalChars_ = 0; for (auto& l : lines_) totalChars_ += l.size();
    lineStartValid_ = 0;
    undo_.clear(); redo_.clear(); undoSeq_.clear(); redoSeq_.clear();
    savedSeq_ = 0; dirtyFrom_ = 0; ++version_;
}

std::wstring TextDoc::Text() const {
    std::wstring o; o.reserve(Length());
    for (size_t i = 0; i < lines_.size(); ++i) { if (i) o += L'\n'; o += lines_[i]; }
    return o;
}
size_t TextDoc::Length() const { return totalChars_ + lines_.size() - 1; }
size_t TextDoc::Offset(Pos p) const {
    // incremental: only rebuild prefix sums from the first line touched since the last query
    if (lineStart_.size() != lines_.size()) { lineStart_.resize(lines_.size()); if (lineStartValid_ > (int)lines_.size()) lineStartValid_ = 0; }
    if (lineStartValid_ < (int)lines_.size()) {
        int i = lineStartValid_;
        size_t o = i == 0 ? 0 : lineStart_[i - 1] + lines_[i - 1].size() + 1;
        for (; i < (int)lines_.size(); ++i) { lineStart_[i] = o; o += lines_[i].size() + 1; }
        lineStartValid_ = (int)lines_.size();
    }
    p = Clamp(p);
    return lineStart_[p.line] + p.col;
}
Pos TextDoc::PosAt(size_t off) const {
    Offset(Pos{});   // refresh table
    auto it = std::upper_bound(lineStart_.begin(), lineStart_.end(), off);
    int line = (int)(it - lineStart_.begin()) - 1;
    if (line < 0) line = 0;
    return Clamp({ line, (int)(off - lineStart_[line]) });
}
Pos TextDoc::Clamp(Pos p) const {
    if (p.line < 0) return { 0, 0 };
    if (p.line >= LineCount()) return End();
    p.col = std::max(0, std::min(p.col, (int)lines_[p.line].size()));
    return p;
}
std::wstring TextDoc::GetText(Pos a, Pos b) const {
    a = Clamp(a); b = Clamp(b);
    if (b < a) std::swap(a, b);
    if (a.line == b.line) return lines_[a.line].substr(a.col, b.col - a.col);
    std::wstring o = lines_[a.line].substr(a.col);
    for (int i = a.line + 1; i < b.line; ++i) { o += L'\n'; o += lines_[i]; }
    o += L'\n'; o += lines_[b.line].substr(0, b.col);
    return o;
}

Pos TextDoc::RawInsert(Pos p, const std::wstring& s) {
    p = Clamp(p);
    Touch(p.line);
    size_t nl = s.find(L'\n');
    totalChars_ += s.size();
    for (wchar_t c : s) if (c == L'\n') --totalChars_;
    if (nl == std::wstring::npos) {
        lines_[p.line].insert(p.col, s);
        stamp_[p.line] = ++stampCounter_;
        return { p.line, p.col + (int)s.size() };
    }
    std::wstring tail = lines_[p.line].substr(p.col);
    lines_[p.line].resize(p.col);
    lines_[p.line] += s.substr(0, nl);
    stamp_[p.line] = ++stampCounter_;
    std::vector<std::wstring> add;
    size_t st = nl + 1;
    for (;;) {
        size_t e = s.find(L'\n', st);
        add.push_back(s.substr(st, e == std::wstring::npos ? std::wstring::npos : e - st));
        if (e == std::wstring::npos) break;
        st = e + 1;
    }
    int endCol = (int)add.back().size();
    add.back() += tail;
    int at = p.line + 1;
    lines_.insert(lines_.begin() + at, add.begin(), add.end());
    std::vector<uint32_t> st2(add.size());
    for (auto& x : st2) x = ++stampCounter_;
    stamp_.insert(stamp_.begin() + at, st2.begin(), st2.end());
    flags_.insert(flags_.begin() + at, add.size(), (uint8_t)0);
    return { p.line + (int)add.size(), endCol };
}
void TextDoc::RawErase(Pos a, Pos b) {
    a = Clamp(a); b = Clamp(b);
    if (b < a) std::swap(a, b);
    if (a == b) return;
    Touch(a.line);
    if (a.line == b.line) { lines_[a.line].erase(a.col, b.col - a.col); totalChars_ -= b.col - a.col; stamp_[a.line] = ++stampCounter_; return; }
    totalChars_ -= lines_[a.line].size() - a.col;
    for (int i = a.line + 1; i < b.line; ++i) totalChars_ -= lines_[i].size();
    totalChars_ -= b.col;
    lines_[a.line] = lines_[a.line].substr(0, a.col) + lines_[b.line].substr(b.col);
    stamp_[a.line] = ++stampCounter_;
    lines_.erase(lines_.begin() + a.line + 1, lines_.begin() + b.line + 1);
    stamp_.erase(stamp_.begin() + a.line + 1, stamp_.begin() + b.line + 1);
    flags_.erase(flags_.begin() + a.line + 1, flags_.begin() + b.line + 1);
}

static Pos EndOf(Pos start, const std::wstring& s) {
    size_t nl = s.rfind(L'\n');
    if (nl == std::wstring::npos) return { start.line, start.col + (int)s.size() };
    int n = 0; for (wchar_t c : s) if (c == L'\n') ++n;
    return { start.line + n, (int)(s.size() - nl - 1) };
}

Pos TextDoc::Replace(Pos a, Pos b, const std::wstring& textIn, Pos caretBefore, Pos anchorBefore, bool coalesce) {
    a = Clamp(a); b = Clamp(b);
    if (b < a) std::swap(a, b);
    std::wstring text = NormalizeNewlines(textIn);
    std::wstring removed = GetText(a, b);
    if (removed.empty() && text.empty()) return a;
    RawErase(a, b);
    Pos end = RawInsert(a, text);
    uint64_t now = Clock();
    redo_.clear(); redoSeq_.clear();
    // typing coalescing: single-char insert continuing the previous insert, no word boundary crossing
    if (coalesce && !breakCoalesce_ && groupDepth_ == 0 && !undo_.empty() && removed.empty() && text.size() == 1 && text[0] != L'\n') {
        EditRec& p = undo_.back();
        Pos pEnd = EndOf(p.start, p.inserted);
        bool boundary = !p.inserted.empty() && IsWordChar(p.inserted.back()) != IsWordChar(text[0]) && !IsWordChar(text[0]);
        if (p.removed.empty() && pEnd == a && now - p.time < 1500 && !boundary && p.inserted.find(L'\n') == std::wstring::npos) {
            p.inserted += text; p.caretAfter = end; p.time = now;
            undoSeq_.back() = ++seqCounter_;
            return end;
        }
    }
    breakCoalesce_ = false;
    EditRec r;
    r.start = a; r.removed = std::move(removed); r.inserted = std::move(text);
    r.caretBefore = caretBefore; r.anchorBefore = anchorBefore; r.caretAfter = end;
    r.group = groupDepth_ ? curGroup_ : 0; r.time = now;
    undo_.push_back(std::move(r)); undoSeq_.push_back(++seqCounter_);
    if (undo_.size() > 100000) { undo_.erase(undo_.begin(), undo_.begin() + 1000); undoSeq_.erase(undoSeq_.begin(), undoSeq_.begin() + 1000); }
    return end;
}

bool TextDoc::Undo(Pos& caret, Pos& anchor) {
    if (undo_.empty()) return false;
    uint32_t g = undo_.back().group;
    do {
        EditRec r = std::move(undo_.back()); undo_.pop_back();
        uint64_t seq = undoSeq_.back(); undoSeq_.pop_back();
        RawErase(r.start, EndOf(r.start, r.inserted));
        RawInsert(r.start, r.removed);
        caret = r.caretBefore; anchor = r.anchorBefore;
        redo_.push_back(std::move(r)); redoSeq_.push_back(seq);
    } while (g != 0 && !undo_.empty() && undo_.back().group == g);
    breakCoalesce_ = true;
    return true;
}
bool TextDoc::Redo(Pos& caret, Pos& anchor) {
    if (redo_.empty()) return false;
    uint32_t g = redo_.back().group;
    do {
        EditRec r = std::move(redo_.back()); redo_.pop_back();
        uint64_t seq = redoSeq_.back(); redoSeq_.pop_back();
        RawErase(r.start, EndOf(r.start, r.removed));
        RawInsert(r.start, r.inserted);
        caret = anchor = r.caretAfter;
        undo_.push_back(std::move(r)); undoSeq_.push_back(seq);
    } while (g != 0 && !redo_.empty() && redo_.back().group == g);
    breakCoalesce_ = true;
    return true;
}

// ------------------------------------------------------------------ codecs
bool IsValidUtf8(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        while (i + 8 <= n) { uint64_t v; memcpy(&v, p + i, 8); if (v & 0x8080808080808080ULL) break; i += 8; }
        if (i >= n) break;
        uint8_t c = p[i];
        if (c < 0x80) { ++i; continue; }
        int len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len) return false;
        if (i + len > n) return false;
        for (int k = 1; k < len; ++k) if ((p[i + k] & 0xC0) != 0x80) return false;
        // RFC 3629: no overlong forms, no UTF-16 surrogates (U+D800..DFFF), nothing above U+10FFFF
        if (len == 2 && c < 0xC2) return false;
        if (len == 3 && c == 0xE0 && p[i + 1] < 0xA0) return false;
        if (len == 3 && c == 0xED && p[i + 1] >= 0xA0) return false;
        if (len == 4 && (c > 0xF4 || (c == 0xF0 && p[i + 1] < 0x90) || (c == 0xF4 && p[i + 1] >= 0x90))) return false;
        i += len;
    }
    return true;
}
static std::wstring Utf8Decode(const uint8_t* p, size_t n) {
    std::wstring o; o.resize(n);   // UTF-16 never needs more units than UTF-8 bytes
    wchar_t* w = &o[0]; size_t i = 0;
    while (i < n) {
        // ASCII fast path, 8 bytes at a time
        while (i + 8 <= n) {
            uint64_t v; memcpy(&v, p + i, 8);
            if (v & 0x8080808080808080ULL) break;
            for (int k = 0; k < 8; ++k) *w++ = (wchar_t)p[i + k];
            i += 8;
        }
        if (i >= n) break;
        uint32_t c = p[i], cp; int len;
        if (c < 0x80) { *w++ = (wchar_t)c; ++i; continue; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { *w++ = (wchar_t)0xFFFD; ++i; continue; }
        if (i + len > n) { *w++ = (wchar_t)0xFFFD; break; }
        bool ok = true;
        for (int k = 1; k < len; ++k) { if ((p[i + k] & 0xC0) != 0x80) { ok = false; break; } cp = (cp << 6) | (p[i + k] & 0x3F); }
        if (!ok) { *w++ = (wchar_t)0xFFFD; ++i; continue; }
        i += len;
        if (cp >= 0x10000 && sizeof(wchar_t) == 2) { cp -= 0x10000; *w++ = (wchar_t)(0xD800 + (cp >> 10)); *w++ = (wchar_t)(0xDC00 + (cp & 0x3FF)); }
        else *w++ = (wchar_t)cp;
    }
    o.resize(w - o.data());
    return o;
}
std::wstring Utf8ToWide(const std::string& s) { return Utf8Decode((const uint8_t*)s.data(), s.size()); }
std::string WideToUtf8(const std::wstring& s) {
    std::string o; o.reserve(s.size() * 3 / 2);
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00); ++i;
        }
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    }
    return o;
}
static std::wstring AnsiToWide(const uint8_t* p, size_t n) {
#ifdef _WIN32
    if (!n) return {};
    int len = MultiByteToWideChar(CP_ACP, 0, (const char*)p, (int)n, nullptr, 0);
    std::wstring o(len, 0);
    MultiByteToWideChar(CP_ACP, 0, (const char*)p, (int)n, &o[0], len);
    return o;
#else
    std::wstring o(n, 0); for (size_t i = 0; i < n; ++i) o[i] = p[i]; return o;   // latin-1 stand-in
#endif
}
bool AnsiCanEncode(const std::wstring& s) {
#ifdef _WIN32
    if (s.empty()) return true;
    BOOL used = FALSE;
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s.data(), (int)s.size(), nullptr, 0, nullptr, &used);
    return !used;
#else
    for (wchar_t c : s) if (c >= 256) return false;
    return true;
#endif
}
static std::vector<uint8_t> WideToAnsi(const std::wstring& s) {
#ifdef _WIN32
    if (s.empty()) return {};
    int len = WideCharToMultiByte(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::vector<uint8_t> o(len);
    WideCharToMultiByte(CP_ACP, 0, s.data(), (int)s.size(), (char*)o.data(), len, nullptr, nullptr);
    return o;
#else
    std::vector<uint8_t> o; for (wchar_t c : s) o.push_back(c < 256 ? (uint8_t)c : '?'); return o;
#endif
}
static std::wstring Utf16(const uint8_t* p, size_t n, bool be) {
    std::wstring o; o.reserve(n / 2);
    for (size_t i = 0; i + 1 < n; i += 2) o += (wchar_t)(be ? (p[i] << 8 | p[i + 1]) : (p[i] | p[i + 1] << 8));
    return o;
}

std::wstring DecodeAs(const std::vector<uint8_t>& b, Enc enc) {
    const uint8_t* p = b.data(); size_t n = b.size();
    switch (enc) {
    case Enc::UTF8BOM: if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; } return Utf8Decode(p, n);
    case Enc::UTF8: return Utf8Decode(p, n);
    case Enc::UTF16LE: if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { p += 2; n -= 2; } return Utf16(p, n, false);
    case Enc::UTF16BE: if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { p += 2; n -= 2; } return Utf16(p, n, true);
    default: return AnsiToWide(p, n);
    }
}

Decoded DecodeBytes(const std::vector<uint8_t>& b) {
    Decoded d;
    size_t n = b.size();
    const uint8_t* p = b.data();
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) d.enc = Enc::UTF8BOM;
    else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) d.enc = Enc::UTF16LE;
    else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) d.enc = Enc::UTF16BE;
    else {
        // BOM-less UTF-16 heuristic: many zero bytes at odd (LE) or even (BE) offsets
        size_t probe = std::min<size_t>(n, 4096), z0 = 0, z1 = 0;
        for (size_t i = 0; i < probe; ++i) if (!p[i]) (i & 1 ? z1 : z0)++;
        if (probe >= 4 && z1 > probe / 4 && z0 < probe / 32) d.enc = Enc::UTF16LE;
        else if (probe >= 4 && z0 > probe / 4 && z1 < probe / 32) d.enc = Enc::UTF16BE;
        else {
            size_t zeros = z0 + z1, ctrl = 0;
            for (size_t i = 0; i < probe; ++i) if (p[i] < 8 || (p[i] > 13 && p[i] < 32 && p[i] != 27)) ++ctrl;
            if (zeros || (probe && ctrl * 10 > probe)) d.binary = true;
            d.enc = IsValidUtf8(p, n) ? Enc::UTF8 : Enc::ANSI;
        }
    }
    std::wstring raw = DecodeAs(b, d.enc);
    size_t crlf = 0, lf = 0, cr = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\r') { if (i + 1 < raw.size() && raw[i + 1] == L'\n') { ++crlf; ++i; } else ++cr; }
        else if (raw[i] == L'\n') ++lf;
    }
    if (lf > crlf && lf >= cr) d.eol = Eol::LF;
    else if (cr > crlf && cr > lf) d.eol = Eol::CR;
    else d.eol = Eol::CRLF;
    d.mixedEol = ((crlf > 0) + (lf > 0) + (cr > 0)) > 1;
    NormalizeNewlinesInPlace(raw);
    d.text = std::move(raw);
    return d;
}

std::vector<uint8_t> EncodeText(const std::wstring& t, Enc enc, Eol eol) {
    std::wstring s;
    if (eol == Eol::LF) s = t;
    else {
        s.reserve(t.size() + t.size() / 30);
        for (wchar_t c : t) { if (c == L'\n') { if (eol == Eol::CRLF) s += L"\r\n"; else s += L'\r'; } else s += c; }
    }
    std::vector<uint8_t> o;
    switch (enc) {
    case Enc::UTF8BOM: o = { 0xEF, 0xBB, 0xBF }; [[fallthrough]];
    case Enc::UTF8: { std::string u = WideToUtf8(s); o.insert(o.end(), u.begin(), u.end()); break; }
    case Enc::UTF16LE: o = { 0xFF, 0xFE }; for (wchar_t c : s) { o.push_back((uint8_t)c); o.push_back((uint8_t)(c >> 8)); } break;
    case Enc::UTF16BE: o = { 0xFE, 0xFF }; for (wchar_t c : s) { o.push_back((uint8_t)(c >> 8)); o.push_back((uint8_t)c); } break;
    case Enc::ANSI: o = WideToAnsi(s); break;
    }
    return o;
}

} // namespace np
