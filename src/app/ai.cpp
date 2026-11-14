// ai.cpp - AI assist side pane. Talks to an LLM over WinHTTP (system DLL):
//   provider 0: Anthropic Messages API   (x-api-key, anthropic-version)
//   provider 1: OpenAI-compatible /chat/completions (works with local servers: Ollama, LM Studio, llama.cpp)
// Request runs on a worker thread; the result is posted back as WM_APP_AI.
#include "app.h"
#include <winhttp.h>
#include <thread>
#include <cmath>
#include <cwctype>

namespace app {

bool g_aiPane = false;
static bool s_busy = false;
static std::wstring s_output, s_error, s_title = L"";
static float s_scroll = 0, s_contentH = 0;
static RectF s_area, s_outBox;
static std::vector<std::pair<RectF, int>> s_hits;
static int s_hover = -1;
static np::Pos s_selA, s_selB; static bool s_hadSel = false; static Doc* s_srcDoc = nullptr;

bool AiBusy() { return s_busy; }

// ------------------------------------------------------------------ tiny JSON
struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    std::wstring s; double n = 0; bool b = false;
    std::vector<JV> a; std::vector<std::pair<std::wstring, JV>> o;
    const JV* get(const wchar_t* k) const { for (auto& p : o) if (p.first == k) return &p.second; return nullptr; }
    const JV* at(size_t i) const { return i < a.size() ? &a[i] : nullptr; }
};
struct JP {
    const std::wstring& s; size_t i = 0;
    explicit JP(const std::wstring& x) : s(x) {}
    void ws() { while (i < s.size() && iswspace(s[i])) ++i; }
    bool str(std::wstring& out) {
        if (s[i] != L'"') return false; ++i;
        while (i < s.size() && s[i] != L'"') {
            wchar_t c = s[i++];
            if (c != L'\\') { out += c; continue; }
            if (i >= s.size()) return false;
            wchar_t e = s[i++];
            switch (e) {
            case L'n': out += L'\n'; break; case L't': out += L'\t'; break; case L'r': out += L'\r'; break;
            case L'b': out += L'\b'; break; case L'f': out += L'\f'; break;
            case L'u': { if (i + 4 > s.size()) return false; out += (wchar_t)wcstoul(s.substr(i, 4).c_str(), nullptr, 16); i += 4; break; }
            default: out += e;
            }
        }
        ++i; return true;
    }
    bool val(JV& v, int depth = 0) {
        if (depth > 64) return false;
        ws(); if (i >= s.size()) return false;
        wchar_t c = s[i];
        if (c == L'{') {
            v.t = JV::Obj; ++i; ws();
            if (s[i] == L'}') { ++i; return true; }
            for (;;) {
                ws(); std::wstring k; if (!str(k)) return false; ws();
                if (s[i++] != L':') return false;
                JV x; if (!val(x, depth + 1)) return false;
                v.o.push_back({ k, std::move(x) }); ws();
                if (s[i] == L',') { ++i; continue; }
                if (s[i] == L'}') { ++i; return true; }
                return false;
            }
        }
        if (c == L'[') {
            v.t = JV::Arr; ++i; ws();
            if (s[i] == L']') { ++i; return true; }
            for (;;) {
                JV x; if (!val(x, depth + 1)) return false; v.a.push_back(std::move(x)); ws();
                if (s[i] == L',') { ++i; continue; }
                if (s[i] == L']') { ++i; return true; }
                return false;
            }
        }
        if (c == L'"') { v.t = JV::Str; return str(v.s); }
        if (s.compare(i, 4, L"true") == 0) { v.t = JV::Bool; v.b = true; i += 4; return true; }
        if (s.compare(i, 5, L"false") == 0) { v.t = JV::Bool; i += 5; return true; }
        if (s.compare(i, 4, L"null") == 0) { i += 4; return true; }
        wchar_t* e; v.n = wcstod(s.c_str() + i, &e); v.t = JV::Num;
        size_t ni = e - s.c_str(); if (ni == i) return false; i = ni; return true;
    }
};
static std::wstring JsonEsc(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        switch (c) {
        case L'"': o += L"\\\""; break; case L'\\': o += L"\\\\"; break;
        case L'\n': o += L"\\n"; break; case L'\r': o += L"\\r"; break; case L'\t': o += L"\\t"; break;
        default:
            if (c < 0x20) { wchar_t b[8]; swprintf(b, 8, L"\\u%04x", c); o += b; } else o += c;
        }
    }
    return o + L"\"";
}

// ------------------------------------------------------------------ HTTP
struct AiResult { bool ok; std::wstring text; };

static AiResult HttpPost(const std::wstring& url, const std::wstring& headers, const std::string& body) {
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return { false, L"Invalid endpoint URL." };
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {   // API keys never travel in clear text to another machine
        std::wstring hst = host; for (auto& c : hst) c = towlower(c);
        bool local = hst == L"localhost" || hst == L"127.0.0.1" || hst == L"::1" || hst == L"[::1]";
        if (!local) return { false, L"Refusing to send the request over unencrypted http:// to a remote host. Use https://, or http://localhost for a local model." };
    }
    HINTERNET ses = WinHttpOpen(L"ModernNotepad/1.4", 4 /*WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY*/, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"ModernNotepad/1.4", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return { false, L"WinHTTP unavailable." };
    WinHttpSetTimeouts(ses, 10000, 15000, 30000, 180000);
    AiResult res{ false, L"" };
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (req && WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        std::string data;
        for (;;) {
            DWORD avail = 0; if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
            size_t o = data.size(); data.resize(o + avail);
            DWORD got = 0; WinHttpReadData(req, &data[o], avail, &got); data.resize(o + got);
        }
        res.ok = status >= 200 && status < 300;
        res.text = np::Utf8ToWide(data);
        if (!res.ok) {
            JV v; JP p(res.text);
            std::wstring msg;
            if (p.val(v)) { if (auto e = v.get(L"error")) { if (e->t == JV::Str) msg = e->s; else if (auto m = e->get(L"message")) msg = m->s; } }
            res.text = L"HTTP " + std::to_wstring(status) + L": " + (msg.empty() ? res.text.substr(0, 400) : msg);
        }
    } else {
        DWORD e = GetLastError();
        res.text = L"Request failed (WinHTTP error " + std::to_wstring(e) + L"). Check the endpoint and your network.";
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return res;
}

static void Worker(int provider, std::wstring endpoint, std::wstring model, std::wstring key, std::wstring system, std::wstring user) {
    std::wstring body, headers = L"Content-Type: application/json\r\n";
    if (provider == 0) {
        body = L"{\"model\":" + JsonEsc(model) + L",\"max_tokens\":2048,\"system\":" + JsonEsc(system) +
               L",\"messages\":[{\"role\":\"user\",\"content\":" + JsonEsc(user) + L"}]}";
        headers += L"x-api-key: " + key + L"\r\nanthropic-version: 2023-06-01\r\n";
    } else {
        body = L"{\"model\":" + JsonEsc(model) + L",\"messages\":[{\"role\":\"system\",\"content\":" + JsonEsc(system) +
               L"},{\"role\":\"user\",\"content\":" + JsonEsc(user) + L"}]}";
        if (!key.empty()) headers += L"Authorization: Bearer " + key + L"\r\n";
    }
    AiResult r = HttpPost(endpoint, headers, np::WideToUtf8(body));
    if (r.ok) {
        JV v; JP p(r.text); std::wstring out;
        if (p.val(v)) {
            if (provider == 0) {
                if (auto c = v.get(L"content")) for (auto& part : c->a) if (auto t = part.get(L"text")) out += t->s;
            } else if (auto ch = v.get(L"choices")) {
                if (auto c0 = ch->at(0)) if (auto m = c0->get(L"message")) if (auto t = m->get(L"content")) out = t->s;
            }
        }
        if (out.empty()) r = { false, L"Unexpected response: " + r.text.substr(0, 300) };
        else r.text = out;
    }
    PostMessageW(g_hwnd, WM_APP_AI, 0, (LPARAM) new AiResult(r));
}

void AiRun(int cmd, const std::wstring& custom) {
    Doc* d = Cur();
    if (!d || s_busy || !g_set.online) return;
    g_aiPane = true;
    if (g_set.aiEndpoint.empty() || (g_set.aiProvider == 0 && g_set.aiKey.empty())) {
        s_error = L"Set up the AI provider in Settings › AI assist (endpoint, model and API key). "
                  L"For a local model, choose \"OpenAI-compatible\" and point it at e.g. http://localhost:11434/v1/chat/completions.";
        s_output.clear(); Invalidate(); return;
    }
    s_hadSel = EditorHasSel(d) && !d->hex;
    s_srcDoc = d;
    s_selA = std::min(d->caret, d->anchor); s_selB = std::max(d->caret, d->anchor);
    std::wstring src = s_hadSel ? EditorSelection(d) : d->text.Text();
    if (src.size() > 60000) src = src.substr(0, 60000) + L"\n[...truncated...]";
    std::wstring sys = L"You are a writing and coding assistant embedded in a text editor. Answer concisely. "
                       L"When asked to rewrite or fix text, reply with only the resulting text and no commentary or code fences.";
    std::wstring task;
    switch (cmd) {
    case C_AI_EXPLAIN: task = L"Explain the following " + std::wstring(np::LangName(d->lang)) + L" content clearly:"; s_title = L"Explain"; break;
    case C_AI_SUMMARIZE: task = L"Summarize the following text in a few bullet points:"; s_title = L"Summary"; break;
    case C_AI_REWRITE: task = L"Rewrite the following text to be clearer and more concise, preserving meaning and format:"; s_title = L"Rewrite"; break;
    case C_AI_GRAMMAR: task = L"Fix spelling, grammar and punctuation in the following text. Keep wording otherwise unchanged:"; s_title = L"Fix grammar"; break;
    case C_AI_CONTINUE: task = L"Continue writing the following text naturally. Reply with only the continuation:"; s_title = L"Continue"; break;
    default: task = custom + L"\n\nText:"; s_title = L"Custom"; break;
    }
    s_busy = true; s_output.clear(); s_error.clear(); s_scroll = 0;
    std::thread(Worker, g_set.aiProvider, g_set.aiEndpoint, g_set.aiModel, g_set.aiKey, sys, task + L"\n\n" + src).detach();
    Invalidate();
}

void AiOnResult(LPARAM lp) {
    std::unique_ptr<AiResult> r((AiResult*)lp);
    s_busy = false;
    if (r->ok) { s_output = r->text; s_error.clear(); } else { s_error = r->text; s_output.clear(); }
    s_scroll = 0;
    Invalidate();
}

// ------------------------------------------------------------------ UI
enum { H_CLOSE = 1, H_EXPLAIN, H_SUM, H_REWRITE, H_GRAMMAR, H_CONT, H_CUSTOM, H_REPLACE, H_INSERT, H_COPY };

static void Chip(const RectF& r, const wchar_t* label, int id, bool primary = false) {
    bool hov = s_hover == id;
    if (primary) FillRR(r, 5, hov ? Mix(g_th.accent, g_th.text, 0.12f) : g_th.accent);
    else { FillRR(r, 5, hov ? Mix(g_th.surface, g_th.text, 0.08f) : g_th.surface); StrokeRR(r, 5, g_th.border); }
    Text(label, r, UiFmt(12.5f), primary ? g_th.accentText : g_th.text, 1);
    s_hits.push_back({ r, id });
}

void AiPaint(const RectF& area) {
    s_area = area; s_hits.clear();
    FillR(area, g_mica ? Alpha(g_th.surface, 0.35f) : g_th.chrome);
    Line(area.left, area.top, area.left, area.bottom, g_th.border);
    float x = area.left + 14, w = RW(area) - 28, y = area.top + 12;
    DrawIcon(I_SPARKLE, x + 8, y + 12, 16, g_th.accent);
    Text(L"AI assist", RF(x + 24, y, w, 24), UiFmt(14, true), g_th.text);
    RectF cl = RF(area.right - 40, y - 2, 28, 28);
    if (s_hover == H_CLOSE) FillRR(cl, 4, g_th.hover);
    DrawIcon(I_CLOSE, (cl.left + cl.right) / 2, (cl.top + cl.bottom) / 2, 12, g_th.dim);
    s_hits.push_back({ cl, H_CLOSE });
    y += 36;
    Doc* d = Cur();
    bool sel = d && EditorHasSel(d) && !d->hex;
    Text(sel ? L"Acts on the selected text" : L"Acts on the whole document (select text to narrow it)", RF(x, y, w, 18), UiFmt(11.5f), g_th.dim);
    y += 24;
    float cw = (w - 8) / 3, ch = 30;
    Chip(RF(x, y, cw, ch), L"Explain", H_EXPLAIN); Chip(RF(x + cw + 4, y, cw, ch), L"Summarize", H_SUM); Chip(RF(x + 2 * (cw + 4), y, cw, ch), L"Rewrite", H_REWRITE);
    y += ch + 4;
    Chip(RF(x, y, cw, ch), L"Fix grammar", H_GRAMMAR); Chip(RF(x + cw + 4, y, cw, ch), L"Continue", H_CONT); Chip(RF(x + 2 * (cw + 4), y, cw, ch), L"Ask…", H_CUSTOM);
    y += ch + 14;
    float btnH = 32;
    s_outBox = { x, y, x + w, area.bottom - btnH - 26 };
    FillRR(s_outBox, 8, g_th.editor); StrokeRR(s_outBox, 8, g_th.border);
    RectF inner = { s_outBox.left + 12, s_outBox.top + 10, s_outBox.right - 12, s_outBox.bottom - 10 };
    std::wstring body; Color bc = g_th.text;
    if (s_busy) {
        int dots = (int)(GetTickCount64() / 400 % 4);
        body = L"Thinking" + std::wstring(dots, L'.'); bc = g_th.dim;
    } else if (!s_error.empty()) { body = s_error; bc = g_th.danger; }
    else if (!s_output.empty()) body = s_output;
    else { body = L"Pick an action above. Results appear here and can be inserted into the document or replace the selection.\n\n"
                  L"Provider, model and key are set in Settings."; bc = g_th.dim; }
    if (!s_title.empty() && !s_output.empty()) {
        Text(s_title, RF(inner.left, inner.top - 2, RW(inner), 18), UiFmt(11.5f, true), g_th.dim);
        inner.top += 20;
    }
    Com<IDWriteTextLayout> L;
    g_dw->CreateTextLayout(body.c_str(), (UINT32)body.size(), UiFmt(13.5f), RW(inner), 1e6f, &L);
    L->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    DWRITE_TEXT_METRICS m; L->GetMetrics(&m);
    s_contentH = m.height;
    s_scroll = std::max(0.f, std::min(s_scroll, std::max(0.f, s_contentH - RH(inner))));
    g_rt->PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_ALIASED);
    g_rt->DrawTextLayout({ inner.left, inner.top - s_scroll }, L, Brush(bc), (D2D1_DRAW_TEXT_OPTIONS)4);
    g_rt->PopAxisAlignedClip();
    if (s_contentH > RH(inner)) {
        float th = std::max(20.f, RH(inner) * RH(inner) / s_contentH);
        float ty = inner.top + (RH(inner) - th) * s_scroll / (s_contentH - RH(inner));
        FillRR(RF(s_outBox.right - 6, ty, 3, th), 1.5f, g_th.scrollThumb);
    }
    if (s_busy) SetTimer(g_hwnd, 7, 400, nullptr);
    float by = area.bottom - btnH - 14, bw = (w - 8) / 3;
    bool have = !s_output.empty();
    if (have) {
        Chip(RF(x, by, bw, btnH), s_hadSel ? L"Replace" : L"Replace all", H_REPLACE, true);
        Chip(RF(x + bw + 4, by, bw, btnH), L"Insert", H_INSERT);
        Chip(RF(x + 2 * (bw + 4), by, bw, btnH), L"Copy", H_COPY);
    }
}

static void CustomDone(const std::wstring& s) { if (!s.empty()) AiRun(C_AI_CUSTOM, s); }

bool AiMouse(UINT msg, float x, float y) {
    if (!g_aiPane || !In(s_area, x, y)) { if (s_hover != -1) { s_hover = -1; Invalidate(); } return false; }
    int hit = -1;
    for (auto& h : s_hits) if (In(h.first, x, y)) hit = h.second;
    if (msg == WM_MOUSEMOVE) { if (hit != s_hover) { s_hover = hit; Invalidate(); } return true; }
    if (msg != WM_LBUTTONUP) return true;
    Doc* d = Cur();
    switch (hit) {
    case H_CLOSE: g_aiPane = false; break;
    case H_EXPLAIN: AiRun(C_AI_EXPLAIN); break;
    case H_SUM: AiRun(C_AI_SUMMARIZE); break;
    case H_REWRITE: AiRun(C_AI_REWRITE); break;
    case H_GRAMMAR: AiRun(C_AI_GRAMMAR); break;
    case H_CONT: AiRun(C_AI_CONTINUE); break;
    case H_CUSTOM: Prompt(L"Ask about the selection / document", L"", CustomDone); break;
    case H_COPY: ClipboardSet(s_output); break;
    case H_INSERT:
        if (d && !d->hex) { d->anchor = d->caret = std::max(d->caret, d->anchor); EditorInsert(d, s_output); }
        break;
    case H_REPLACE:
        if (d && !d->hex && d == s_srcDoc) {
            if (s_hadSel) { d->anchor = d->text.Clamp(s_selA); d->caret = d->text.Clamp(s_selB); }
            else { d->anchor = { 0, 0 }; d->caret = d->text.End(); }
            d->text.BreakCoalesce();
            EditorInsert(d, s_output);
        }
        break;
    }
    Invalidate();
    return true;
}
void AiWheel(int delta) { s_scroll -= delta * 0.5f; Invalidate(); }

} // namespace app
