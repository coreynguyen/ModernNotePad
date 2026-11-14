// uia.cpp - UI Automation for the custom-drawn editor, so Narrator / NVDA / JAWS / Magnifier can read
// and follow it. The main window answers WM_GETOBJECT with a Document element that implements the
// Text pattern (ITextProvider2: document, selection, caret, visible ranges, point -> position) and
// text ranges (ITextRangeProvider: units character / word / line / paragraph / document, text,
// bounding rectangles, select, scroll, find). Selection, text and focus changes are raised as UIA events.
// Positions are linear offsets into the TextDoc (lines joined with '\n'), so ranges survive scrolling.
#include "app.h"
#include <uiautomation.h>
#include <cwctype>
#include <cmath>
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "oleaut32.lib")

namespace app {

namespace {
Doc* DocById(uint64_t id) { for (auto& d : g_docs) if (d->id == id) return d.get(); return nullptr; }
size_t Len(Doc* d) { return d->text.Length(); }
wchar_t CharAt(Doc* d, size_t off) {   // '\n' between lines
    np::Pos p = d->text.PosAt(off);
    const std::wstring& l = d->text.Line(p.line);
    return p.col < (int)l.size() ? l[p.col] : L'\n';
}
size_t LineStart(Doc* d, size_t off) { return d->text.Offset({ d->text.PosAt(off).line, 0 }); }
size_t NextLine(Doc* d, size_t off) {
    np::Pos p = d->text.PosAt(off);
    return p.line + 1 < d->text.LineCount() ? d->text.Offset({ p.line + 1, 0 }) : Len(d);
}
bool IsWordStart(Doc* d, size_t o) {   // boundary before a run of word chars or punctuation, after whitespace / line start
    if (o == 0) return true;
    if (o >= Len(d)) return true;
    wchar_t c = CharAt(d, o), p = CharAt(d, o - 1);
    if (p == L'\n') return true;
    if (iswspace(c)) return false;
    if (iswspace(p)) return true;
    return np::IsWordChar(c) != np::IsWordChar(p);
}
size_t Boundary(Doc* d, size_t o, TextUnit u, int dir) {   // next (dir>0) / previous (dir<0) boundary strictly beyond o
    size_t n = Len(d);
    switch (u) {
    case TextUnit_Character:
        if (dir > 0) { if (o >= n) return n; ++o; if (o < n && CharAt(d, o) >= 0xDC00 && CharAt(d, o) <= 0xDFFF) ++o; return o; }
        if (o == 0) return 0; --o; if (o > 0 && CharAt(d, o) >= 0xDC00 && CharAt(d, o) <= 0xDFFF) --o; return o;
    case TextUnit_Format: case TextUnit_Word:
        if (dir > 0) { if (o >= n) return n; do ++o; while (o < n && !IsWordStart(d, o)); return o; }
        if (o == 0) return 0; do --o; while (o > 0 && !IsWordStart(d, o)); return o;
    case TextUnit_Line: case TextUnit_Paragraph:
        if (dir > 0) return o >= n ? n : NextLine(d, o);
        if (o == 0) return 0;
        return LineStart(d, o - 1) == LineStart(d, o) ? LineStart(d, o) : LineStart(d, o - 1) == o ? o : LineStart(d, o - 1);
    default:   // Page, Document
        return dir > 0 ? n : 0;
    }
}
size_t UnitStart(Doc* d, size_t o, TextUnit u) {
    switch (u) {
    case TextUnit_Character: return o;
    case TextUnit_Format: case TextUnit_Word: while (o > 0 && !IsWordStart(d, o)) --o; return o;
    case TextUnit_Line: case TextUnit_Paragraph: return LineStart(d, o);
    default: return 0;
    }
}
BSTR Bstr(const std::wstring& s) { return SysAllocStringLen(s.data(), (UINT)s.size()); }

class Root;
Root* s_root = nullptr;

class Range : public ITextRangeProvider {
public:
    Range(uint64_t doc, size_t a, size_t b) : doc_(doc), a_(a), b_(std::max(a, b)) {}
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(ITextRangeProvider)) { *p = static_cast<ITextRangeProvider*>(this); AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }
    // ITextRangeProvider
    HRESULT STDMETHODCALLTYPE Clone(ITextRangeProvider** out) override { *out = new Range(doc_, a_, b_); return S_OK; }
    HRESULT STDMETHODCALLTYPE Compare(ITextRangeProvider* o, BOOL* same) override {
        Range* r = static_cast<Range*>(o); *same = r && r->doc_ == doc_ && r->a_ == a_ && r->b_ == b_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CompareEndpoints(TextPatternRangeEndpoint ep, ITextRangeProvider* o, TextPatternRangeEndpoint tep, int* res) override {
        Range* r = static_cast<Range*>(o); if (!r) return E_INVALIDARG;
        long long x = ep == TextPatternRangeEndpoint_Start ? a_ : b_, y = tep == TextPatternRangeEndpoint_Start ? r->a_ : r->b_;
        *res = x < y ? -1 : x > y ? 1 : 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ExpandToEnclosingUnit(TextUnit u) override {
        Doc* d = Get(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        a_ = UnitStart(d, std::min(a_, Len(d)), u);
        b_ = u == TextUnit_Page || u == TextUnit_Document ? Len(d) : Boundary(d, a_, u, 1);
        if (b_ < a_) b_ = a_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindAttribute(TEXTATTRIBUTEID, VARIANT, BOOL, ITextRangeProvider** out) override { *out = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE FindText(BSTR text, BOOL backward, BOOL ignoreCase, ITextRangeProvider** out) override {
        *out = nullptr; Doc* d = Get(); if (!d || !text) return S_OK;
        std::wstring hay = Text(d), nd = text;
        if (ignoreCase) { for (auto& c : hay) c = towlower(c); for (auto& c : nd) c = towlower(c); }
        size_t p = backward ? hay.rfind(nd) : hay.find(nd);
        if (p != std::wstring::npos && !nd.empty()) *out = new Range(doc_, a_ + p, a_ + p + nd.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAttributeValue(TEXTATTRIBUTEID id, VARIANT* v) override {
        VariantInit(v);
        if (id == UIA_IsReadOnlyAttributeId) { v->vt = VT_BOOL; v->boolVal = VARIANT_FALSE; return S_OK; }
        if (id == UIA_FontNameAttributeId) { v->vt = VT_BSTR; v->bstrVal = Bstr(g_set.fontFace); return S_OK; }
        if (id == UIA_FontSizeAttributeId) { v->vt = VT_R8; v->dblVal = g_set.fontSize * g_set.zoom / 100.0; return S_OK; }
        return UiaGetReservedNotSupportedValue(&v->punkVal) == S_OK ? (v->vt = VT_UNKNOWN, S_OK) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBoundingRectangles(SAFEARRAY** out) override {
        *out = nullptr; Doc* d = Get(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        std::vector<RectF> rs;
        if (d == Cur()) EditorRangeRects(d, d->text.PosAt(a_), d->text.PosAt(b_), rs);
        *out = SafeArrayCreateVector(VT_R8, 0, (ULONG)(rs.size() * 4));
        if (!*out) return E_OUTOFMEMORY;
        LONG i = 0;
        for (auto& r : rs) {
            POINT p{ (LONG)std::lround(r.left * g_scale), (LONG)std::lround(r.top * g_scale) }; ClientToScreen(g_hwnd, &p);
            double vals[4] = { (double)p.x, (double)p.y, RW(r) * g_scale, RH(r) * g_scale };
            for (double x : vals) { SafeArrayPutElement(*out, &i, &x); ++i; }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEnclosingElement(IRawElementProviderSimple** out) override;
    HRESULT STDMETHODCALLTYPE GetText(int maxLen, BSTR* out) override {
        Doc* d = Get(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        std::wstring t = Text(d);
        if (maxLen >= 0 && (int)t.size() > maxLen) t.resize(maxLen);
        *out = Bstr(t); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Move(TextUnit u, int count, int* moved) override {
        Doc* d = Get(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        bool degenerate = a_ == b_;
        size_t n = Len(d), start = std::min(a_, n), pos = UnitStart(d, start, u);
        int m = 0;
        if (count > 0) {
            while (m < count) {
                size_t nx = Boundary(d, pos, u, 1);
                if (nx == pos || (nx >= n && !degenerate)) break;
                pos = nx; ++m;
            }
        } else if (count < 0) {
            if (pos < start) m = -1;   // collapsing back to the unit start is the first move
            while (m > count) {
                size_t pv = Boundary(d, pos, u, -1);
                if (pv == pos) break;
                pos = pv; --m;
            }
        }
        a_ = pos;
        b_ = degenerate ? pos : Boundary(d, pos, u, 1);
        *moved = m; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByUnit(TextPatternRangeEndpoint ep, TextUnit u, int count, int* moved) override {
        Doc* d = Get(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        size_t& e = ep == TextPatternRangeEndpoint_Start ? a_ : b_;
        int m = 0;
        while (m != count) {
            int dir = count > 0 ? 1 : -1;
            size_t np_ = Boundary(d, e, u, dir);
            if (np_ == e) break;
            e = np_; m += dir;
        }
        if (a_ > b_) { if (ep == TextPatternRangeEndpoint_Start) b_ = a_; else a_ = b_; }
        *moved = m; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByRange(TextPatternRangeEndpoint ep, ITextRangeProvider* o, TextPatternRangeEndpoint tep) override {
        Range* r = static_cast<Range*>(o); if (!r) return E_INVALIDARG;
        size_t v = tep == TextPatternRangeEndpoint_Start ? r->a_ : r->b_;
        if (ep == TextPatternRangeEndpoint_Start) { a_ = v; if (b_ < a_) b_ = a_; } else { b_ = v; if (a_ > b_) a_ = b_; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Select() override {
        Doc* d = Get(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        EditorSelect(d, d->text.PosAt(a_), d->text.PosAt(b_)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE ScrollIntoView(BOOL) override {
        Doc* d = Get(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        np::Pos keepC = d->caret, keepA = d->anchor;
        d->caret = d->text.PosAt(a_); EditorEnsureCaret(d); d->caret = keepC; d->anchor = keepA;
        Invalidate(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetChildren(SAFEARRAY** out) override { *out = SafeArrayCreateVector(VT_UNKNOWN, 0, 0); return S_OK; }
private:
    Doc* Get() { Doc* d = DocById(doc_); if (d) { size_t n = Len(d); a_ = std::min(a_, n); b_ = std::min(b_, n); } return d; }
    std::wstring Text(Doc* d) { return d->text.GetText(d->text.PosAt(a_), d->text.PosAt(b_)); }
    LONG ref_ = 1; uint64_t doc_; size_t a_, b_;
};

class Root : public IRawElementProviderSimple, public ITextProvider2 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IRawElementProviderSimple)) { *p = static_cast<IRawElementProviderSimple*>(this); AddRef(); return S_OK; }
        if (r == __uuidof(ITextProvider) || r == __uuidof(ITextProvider2)) { *p = static_cast<ITextProvider2*>(this); AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }
    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* o) override { *o = (ProviderOptions)(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** out) override {
        *out = nullptr;
        if ((id == UIA_TextPatternId || id == UIA_TextPattern2Id) && Usable()) { *out = static_cast<ITextProvider2*>(this); AddRef(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* v) override {
        VariantInit(v);
        Doc* d = Cur();
        switch (id) {
        case UIA_ControlTypePropertyId: v->vt = VT_I4; v->lVal = UIA_DocumentControlTypeId; break;
        case UIA_NamePropertyId: v->vt = VT_BSTR; v->bstrVal = Bstr((d ? d->Name() : std::wstring(L"Untitled")) + L" - ModernNotepad"); break;
        case UIA_LocalizedControlTypePropertyId: v->vt = VT_BSTR; v->bstrVal = Bstr(L"document"); break;
        case UIA_AutomationIdPropertyId: v->vt = VT_BSTR; v->bstrVal = Bstr(L"Editor"); break;
        case UIA_IsKeyboardFocusablePropertyId: v->vt = VT_BOOL; v->boolVal = VARIANT_TRUE; break;
        case UIA_HasKeyboardFocusPropertyId: v->vt = VT_BOOL; v->boolVal = GetFocus() == g_hwnd ? VARIANT_TRUE : VARIANT_FALSE; break;
        case UIA_IsTextPatternAvailablePropertyId: v->vt = VT_BOOL; v->boolVal = Usable() ? VARIANT_TRUE : VARIANT_FALSE; break;
        case UIA_HelpTextPropertyId: v->vt = VT_BSTR; v->bstrVal = Bstr(d && d->grid ? L"Table view - press Control+Shift+T for text" : d && d->hex ? L"Hex editor" : L""); break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override { return UiaHostProviderFromHwnd(g_hwnd, out); }
    // ITextProvider
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** out) override {
        Doc* d = Cur(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        size_t a = d->text.Offset(std::min(d->caret, d->anchor)), b = d->text.Offset(std::max(d->caret, d->anchor));
        return OneRange(new Range(d->id, a, b), out);
    }
    HRESULT STDMETHODCALLTYPE GetVisibleRanges(SAFEARRAY** out) override {
        Doc* d = Cur(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        int f = 0, l = 0;
        if (!EditorVisibleLines(d, f, l)) { f = l = 0; }
        size_t a = d->text.Offset({ f, 0 }), b = d->text.Offset({ l, (int)d->text.Line(l).size() });
        return OneRange(new Range(d->id, a, b), out);
    }
    HRESULT STDMETHODCALLTYPE RangeFromChild(IRawElementProviderSimple*, ITextRangeProvider** out) override { *out = nullptr; return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE RangeFromPoint(UiaPoint pt, ITextRangeProvider** out) override {
        Doc* d = Cur(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        POINT p{ (LONG)pt.x, (LONG)pt.y }; ScreenToClient(g_hwnd, &p);
        size_t o = d->text.Offset(EditorHitPublic(d, p.x / g_scale, p.y / g_scale));
        *out = new Range(d->id, o, o); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_DocumentRange(ITextRangeProvider** out) override {
        Doc* d = Cur(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        *out = new Range(d->id, 0, Len(d)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SupportedTextSelection(SupportedTextSelection* s) override { *s = SupportedTextSelection_Single; return S_OK; }
    // ITextProvider2
    HRESULT STDMETHODCALLTYPE RangeFromAnnotation(IRawElementProviderSimple*, ITextRangeProvider** out) override { *out = nullptr; return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE GetCaretRange(BOOL* active, ITextRangeProvider** out) override {
        Doc* d = Cur(); if (!d) return UIA_E_ELEMENTNOTAVAILABLE;
        *active = GetFocus() == g_hwnd;
        size_t o = d->text.Offset(d->caret);
        *out = new Range(d->id, o, o); return S_OK;
    }
private:
    static bool Usable() { Doc* d = Cur(); return d && !d->loading && !d->binaryOnly; }
    static HRESULT OneRange(Range* r, SAFEARRAY** out) {
        *out = SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
        if (!*out) { r->Release(); return E_OUTOFMEMORY; }
        LONG i = 0; IUnknown* u = static_cast<ITextRangeProvider*>(r);
        HRESULT hr = SafeArrayPutElement(*out, &i, u);   // AddRefs
        r->Release();
        return hr;
    }
    LONG ref_ = 1;
};

HRESULT STDMETHODCALLTYPE Range::GetEnclosingElement(IRawElementProviderSimple** out) {
    *out = s_root; if (s_root) s_root->AddRef(); return S_OK;
}

// change tracking for events
uint64_t s_lastDoc = 0, s_lastVer = UINT64_MAX; np::Pos s_lastC, s_lastA; bool s_lastFocus = false;
} // namespace

LRESULT UiaHandleGetObject(HWND h, WPARAM wp, LPARAM lp, bool& handled) {
    handled = false;
    if ((LONG)lp != UiaRootObjectId) return 0;
    if (!s_root) s_root = new Root;
    handled = true;
    return UiaReturnRawElementProvider(h, wp, lp, s_root);
}

void UiaNotify() {
    if (!s_root || !UiaClientsAreListening()) return;
    Doc* d = Cur(); if (!d) return;
    bool focus = GetFocus() == g_hwnd;
    if (focus && (!s_lastFocus || d->id != s_lastDoc)) UiaRaiseAutomationEvent(s_root, UIA_AutomationFocusChangedEventId);
    if (d->id != s_lastDoc || d->text.Version() != s_lastVer) UiaRaiseAutomationEvent(s_root, UIA_Text_TextChangedEventId);
    if (d->id != s_lastDoc || d->caret != s_lastC || d->anchor != s_lastA) UiaRaiseAutomationEvent(s_root, UIA_Text_TextSelectionChangedEventId);
    s_lastDoc = d->id; s_lastVer = d->text.Version(); s_lastC = d->caret; s_lastA = d->anchor; s_lastFocus = focus;
}

// Developer self-test (ModernNotepad.exe /uiaselftest out.txt): exercises the providers directly, the way a
// UIA client would, and writes what they return. Lets the text pattern be verified where no UIA client
// with Text pattern support exists (e.g. Wine).
void UiaSelfTest(const std::wstring& outPath) {
    if (!s_root) s_root = new Root;
    std::wstring o;
    auto txt = [&](ITextRangeProvider* r) { BSTR b = nullptr; r->GetText(-1, &b); std::wstring w = b ? std::wstring(b, SysStringLen(b)) : L"(null)"; SysFreeString(b); for (auto& c : w) if (c == L'\n') c = L'|'; return w; };
    ITextProvider2* tp = static_cast<ITextProvider2*>(s_root);
    ITextRangeProvider* dr = nullptr; tp->get_DocumentRange(&dr);
    o += L"document: [" + txt(dr) + L"]\n";
    SAFEARRAY* sa = nullptr; tp->GetSelection(&sa); LONG i = 0; IUnknown* u = nullptr; SafeArrayGetElement(sa, &i, &u);
    ITextRangeProvider* sel = static_cast<ITextRangeProvider*>(static_cast<Range*>(static_cast<ITextRangeProvider*>((void*)u)));
    sel->ExpandToEnclosingUnit(TextUnit_Line); o += L"caret line: [" + txt(sel) + L"]\n";
    ITextRangeProvider* r = nullptr; dr->Clone(&r); r->MoveEndpointByRange(TextPatternRangeEndpoint_End, r, TextPatternRangeEndpoint_Start);
    int mv = 0;
    for (int k = 0; k < 6; ++k) { ITextRangeProvider* w = nullptr; r->Clone(&w); w->ExpandToEnclosingUnit(TextUnit_Word); o += L"word " + std::to_wstring(k) + L": [" + txt(w) + L"]\n"; w->Release(); r->Move(TextUnit_Word, 1, &mv); }
    r->Move(TextUnit_Line, 1, &mv); r->ExpandToEnclosingUnit(TextUnit_Line); o += L"line after (moved " + std::to_wstring(mv) + L"): [" + txt(r) + L"]\n";
    r->Move(TextUnit_Line, -2, &mv); o += L"line back -2 (moved " + std::to_wstring(mv) + L"): [" + txt(r) + L"]\n";
    r->Move(TextUnit_Character, 4, &mv); o += L"char +4 (moved " + std::to_wstring(mv) + L"): [" + txt(r) + L"]\n";
    ITextRangeProvider* f = nullptr; BSTR q = SysAllocString(L"LAZY"); dr->FindText(q, FALSE, TRUE, &f); SysFreeString(q);
    o += L"find LAZY (icase): [" + (f ? txt(f) : std::wstring(L"none")) + L"]\n";
    if (f) { f->Select(); SAFEARRAY* s2 = nullptr; tp->GetSelection(&s2); IUnknown* u2 = nullptr; LONG z = 0; SafeArrayGetElement(s2, &z, &u2);
             o += L"selection after Select: [" + txt(static_cast<ITextRangeProvider*>((void*)u2)) + L"]\n";
             SAFEARRAY* rc = nullptr; static_cast<ITextRangeProvider*>((void*)u2)->GetBoundingRectangles(&rc);
             o += L"bounding values: " + std::to_wstring(rc ? rc->rgsabound[0].cElements : 0) + L"\n"; }
    BOOL act; ITextRangeProvider* cr = nullptr; tp->GetCaretRange(&act, &cr); cr->ExpandToEnclosingUnit(TextUnit_Word); o += L"caret word: [" + txt(cr) + L"]\n";
    ITextRangeProvider* vr = nullptr; SAFEARRAY* vs = nullptr; tp->GetVisibleRanges(&vs); LONG vz = 0; IUnknown* vu = nullptr; SafeArrayGetElement(vs, &vz, &vu); vr = static_cast<ITextRangeProvider*>((void*)vu);
    o += L"visible: [" + txt(vr) + L"]\n";
    std::string u8 = np::WideToUtf8(o);
    HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr); CloseHandle(h); }
}

void UiaShutdown() {
    if (!s_root) return;
    UiaReturnRawElementProvider(g_hwnd, 0, 0, nullptr);
    UiaDisconnectProvider(s_root);
    s_root->Release(); s_root = nullptr;
}

} // namespace app
