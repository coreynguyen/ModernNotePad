// game.cpp - "Glyph Invaders": the credits easter egg on the Settings page.
// Stray characters invade the editor; you are the caret. Left click fires the laser, right click a homing
// missile. Everything is generated in code: vector/glyph graphics, a 4-voice chiptune and the sound effects
// (synthesised into a waveOut stream - no audio files, no libraries). Music only plays while a game runs.
#include "app.h"
#include <mmsystem.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <map>
#include <algorithm>
#pragma comment(lib, "winmm.lib")

namespace app {

// ================================================================== audio
namespace audio {
static const int SR = 22050;
struct Voice { int type = 0; float t = 0, dur = 0.1f, f0 = 440, f1 = 440, vol = 0.3f, ph = 0, lp = 0; uint32_t rng = 0x12345u; };
static std::mutex s_mx;
static std::vector<Voice> s_voices;
static std::atomic<bool> s_run{ false }, s_music{ false };
static std::atomic<bool> s_musicMute{ false }, s_sfxMute{ false };
static std::thread s_thread;
static HANDLE s_evt = nullptr;
static HWAVEOUT s_wo = nullptr;
static bool s_failed = false;

static float NoteHz(int midi) { return 440.f * std::pow(2.f, (midi - 69) / 12.f); }
static float Square(float ph, float duty) { return ph - std::floor(ph) < duty ? 1.f : -1.f; }
static float Tri(float ph) { float x = ph - std::floor(ph); return x < 0.5f ? 4 * x - 1 : 3 - 4 * x; }
static float Noise(uint32_t& r) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (r & 0xFFFF) / 32768.f - 1.f; }

// ---- music: 140 BPM, 16th-note steps, 8 bars (A section + B section), A minor: Am F C G
static const int kMelA[64] = {
    76, -1, 81, -1, 79, 76, -1, 74,  76, -1, -1, -1, 72, -1, 74, -1,
    72, -1, 77, -1, 76, 72, -1, 69,  72, -1, -1, -1, 69, -1, 72, -1,
    79, -1, 76, -1, 79, 81, -1, 79,  76, -1, 74, -1, 72, -1, 74, -1,
    74, -1, 71, -1, 74, 79, -1, 77,  76, -1, 74, -1, 71, -1, -1, 0 };
static const int kMelB[64] = {
    81, -1, -1, 79, 81, -1, 84, -1,  83, -1, 81, -1, 79, -1, 76, -1,
    77, -1, -1, 76, 77, -1, 81, -1,  79, -1, 77, -1, 76, -1, 72, -1,
    76, -1, 79, -1, 84, -1, 83, 81,  79, -1, 76, -1, 72, -1, 76, -1,
    74, -1, 79, -1, 83, -1, 86, -1,  83, -1, -1, -1, 0, -1, 0, 0 };
static const int kRoot[4] = { 45, 41, 48, 43 };   // A2 F2 C3 G2
static const int kChord[4][3] = { { 0, 3, 7 }, { 0, 4, 7 }, { 0, 4, 7 }, { 0, 4, 7 } };
struct Music {
    long long n = 0;
    float phLead = 0, phBass = 0, phArp = 0, lp = 0; uint32_t rng = 777;
    int leadNote = 0; long long leadStart = 0;
    float Sample() {
        const int stepLen = SR * 60 / 140 / 4;
        long long step = n / stepLen; int inStep = (int)(n % stepLen);
        int s64 = (int)(step % 128), bar = (s64 / 16) % 4, sib = s64 % 16;
        bool sectionB = s64 >= 64;
        const int* mel = sectionB ? kMelB : kMelA;
        int idx = s64 % 64;
        if (inStep == 0) { int m = mel[idx]; if (m > 0) { leadNote = m; leadStart = n; } else if (m == 0) leadNote = 0; }
        float out = 0;
        // lead: 25% square with gentle vibrato and a short attack/decay
        if (leadNote > 0) {
            float age = (n - leadStart) / (float)SR;
            float env = std::min(1.f, age * 60.f) * (0.55f + 0.45f * std::exp(-age * 6.f));
            float vib = 1.f + 0.004f * std::sin(age * 34.f) * std::min(1.f, age * 3.f);
            phLead += NoteHz(leadNote) * vib / SR;
            out += Square(phLead, 0.25f) * 0.11f * env;
        }
        // bass: triangle, syncopated root/octave
        static const int bassPat[16] = { 1, 0, 0, 2, 0, 0, 1, 0, 1, 0, 0, 2, 0, 1, 0, 0 };
        int bp = bassPat[sib];
        static int bassNote = 45; static long long bassStart = 0;
        if (inStep == 0 && bp) { bassNote = kRoot[bar] + (bp == 2 ? 12 : 0); bassStart = n; }
        { float age = (n - bassStart) / (float)SR; float env = std::exp(-age * 3.5f);
          phBass += NoteHz(bassNote) / SR; out += Tri(phBass) * 0.20f * env; }
        // arpeggio: quiet 12.5% square through the chord, two octaves up
        { int tone = kRoot[bar] + 24 + kChord[bar][sib % 3] + (sib % 6 >= 3 ? 12 : 0);
          float env = std::exp(-(inStep / (float)SR) * 14.f);
          phArp += NoteHz(tone) / SR; out += Square(phArp, 0.125f) * 0.045f * env; }
        // drums
        float tStep = inStep / (float)SR;
        if (sib == 0 || sib == 8 || (sib == 10 && bar == 3)) {   // kick: pitch-dropping sine
            float f = 140.f * std::exp(-tStep * 28.f) + 45.f; static float kph = 0; kph += f / SR;
            out += std::sin(kph * 6.2831853f) * 0.32f * std::exp(-tStep * 9.f);
        }
        if (sib == 4 || sib == 12) out += Noise(rng) * 0.16f * std::exp(-tStep * 18.f);          // snare
        if (sib % 2 == 0) { float nz = Noise(rng); lp += (nz - lp) * 0.9f; out += (nz - lp) * 0.06f * std::exp(-tStep * 60.f); }   // hat
        ++n;
        return out;
    }
};
static Music s_song;

static float VoiceSample(Voice& v) {
    float k = v.t / v.dur;
    if (k >= 1) return 0;
    float f = v.f0 + (v.f1 - v.f0) * k, out = 0;
    switch (v.type) {
    case 0: v.ph += f / SR; out = Square(v.ph, 0.5f) * (1 - k) * (1 - k); break;                       // laser
    case 1: { v.ph += f / SR; float nz = Noise(v.rng); v.lp += (nz - v.lp) * 0.2f;                    // missile
              out = (Square(v.ph, 0.3f) * 0.5f + v.lp * 1.2f) * (1 - k); break; }
    case 2: { float nz = Noise(v.rng); float c = 0.35f * (1 - k) + 0.03f; v.lp += (nz - v.lp) * c;    // explosion
              out = v.lp * 1.8f * std::pow(1 - k, 1.6f); break; }
    case 3: v.ph += f / SR; out = Square(v.ph, 0.5f) * (1 - k); break;                                  // hit
    case 4: v.ph += f / SR; out = (Square(v.ph, 0.5f) * 0.6f + Noise(v.rng) * 0.25f) * (1 - k); break;  // player hurt
    case 5: { int step = std::min(3, (int)(k * 4)); static const int up[4] = { 0, 4, 7, 12 };            // jingle
              v.ph += f * std::pow(2.f, up[step] / 12.f) / SR; out = Square(v.ph, 0.25f) * (1 - k * 0.6f); break; }
    }
    v.t += 1.f / SR;
    return out * v.vol;
}

static void Fill(int16_t* buf, int n) {
    std::lock_guard<std::mutex> lk(s_mx);
    bool music = s_music && !s_musicMute;
    for (int i = 0; i < n; ++i) {
        float x = music ? s_song.Sample() * 0.8f : 0.f;
        if (!s_sfxMute) for (auto& v : s_voices) x += VoiceSample(v);
        x = std::tanh(x * 1.1f);   // soft clip
        buf[i] = (int16_t)(x * 26000);
    }
    s_voices.erase(std::remove_if(s_voices.begin(), s_voices.end(), [](const Voice& v) { return v.t >= v.dur; }), s_voices.end());
}
static void ThreadMain() {
    const int N = 1024, B = 4;
    std::vector<int16_t> data(N * B);
    WAVEHDR hdr[B]{};
    for (int i = 0; i < B; ++i) {
        hdr[i].lpData = (LPSTR)&data[i * N]; hdr[i].dwBufferLength = N * 2;
        Fill(&data[i * N], N);
        waveOutPrepareHeader(s_wo, &hdr[i], sizeof(WAVEHDR));
        waveOutWrite(s_wo, &hdr[i], sizeof(WAVEHDR));
    }
    while (s_run) {
        WaitForSingleObject(s_evt, 100);
        for (int i = 0; i < B; ++i) if (hdr[i].dwFlags & WHDR_DONE) {
            Fill(&data[i * N], N);
            hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(s_wo, &hdr[i], sizeof(WAVEHDR));
        }
    }
    waveOutReset(s_wo);
    for (int i = 0; i < B; ++i) waveOutUnprepareHeader(s_wo, &hdr[i], sizeof(WAVEHDR));
}
static void Start() {
    if (s_run || s_failed) return;
    WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = SR; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2; wf.nAvgBytesPerSec = SR * 2;
    if (!s_evt) s_evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (waveOutOpen(&s_wo, WAVE_MAPPER, &wf, (DWORD_PTR)s_evt, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) { s_failed = true; s_wo = nullptr; return; }
    s_run = true;
    s_thread = std::thread(ThreadMain);
}
static void Stop() {
    if (!s_run) return;
    s_run = false; SetEvent(s_evt);
    if (s_thread.joinable()) s_thread.join();
    waveOutClose(s_wo); s_wo = nullptr;
}
static void Play(int type, float f0, float f1, float dur, float vol) {
    if (!s_run) return;
    std::lock_guard<std::mutex> lk(s_mx);
    if (s_voices.size() > 24) s_voices.erase(s_voices.begin());
    Voice v; v.type = type; v.f0 = f0; v.f1 = f1; v.dur = dur; v.vol = vol; v.rng = (uint32_t)GetTickCount() * 2654435761u | 1;
    s_voices.push_back(v);
}
} // namespace audio

// ================================================================== game
namespace {
struct Enemy { std::wstring g; float x, y, vx = 0, vy = 0, t = 0, homeX = 0, homeY = 0; int hp = 1, maxHp = 1, kind = 0; float flash = 0, fireCd = 2; bool dive = false; };
struct Shot { float x, y, vx, vy; int kind; float life = 4; int target = -1; };   // 0 laser, 1 missile, 2 enemy
struct Part { float x, y, vx, vy, life, max; wchar_t ch; Color c; float size; };
struct Star { float x, y, z; };
struct Pop { float x, y, life; std::wstring t; };

enum State { Idle, Playing, Paused, Over };
State s_state = Idle;
RectF s_canvas, s_btnPlay, s_btnMusic, s_btnSfx;
float W = 320, H = 400;
float s_px = 160, s_py = 360, s_mx = 160, s_my = 360;
int s_lives = 3, s_score = 0, s_best = 0, s_wave = 0, s_missiles = 3;
float s_inv = 0, s_fireCd = 0, s_missileCd = 0, s_missileRegen = 0, s_waveDelay = 0, s_shake = 0;
bool s_lDown = false;
std::vector<Enemy> s_en; std::vector<Shot> s_shots; std::vector<Part> s_parts; std::vector<Star> s_stars; std::vector<Pop> s_pops;
LARGE_INTEGER s_last{}, s_freq{};
uint32_t s_rng = 0xC0FFEEu;
float Rnd() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return (s_rng & 0xFFFFFF) / 16777216.f; }

Color Neon(int i) { static const uint32_t c[] = { 0x5CE1E6, 0xFF5CA8, 0xFFD166, 0x8AFF80, 0xB28DFF, 0xFF8A5B }; return Hex(c[i % 6]); }
IDWriteTextFormat* GlyphFmt(float px, bool bold = true) {
    static std::map<int, Com<IDWriteTextFormat>> cache;
    int key = (int)(px * 4) * 2 + bold;
    auto& f = cache[key];
    if (!f) {
        g_dw->CreateTextFormat(g_monoFont.c_str(), nullptr, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", &f.p);
        if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    }
    return f;
}

void Burst(float x, float y, Color c, int n, float speed, const wchar_t* chars) {
    size_t cl = wcslen(chars);
    for (int i = 0; i < n; ++i) {
        float a = Rnd() * 6.2831853f, v = speed * (0.3f + Rnd());
        float life = 0.4f + Rnd() * 0.6f;
        s_parts.push_back({ x, y, std::cos(a) * v, std::sin(a) * v, life, life, chars[(size_t)(Rnd() * cl) % cl], c, 9 + Rnd() * 7 });
    }
}

void SpawnWave() {
    ++s_wave;
    s_en.clear();
    static const wchar_t* glyphs[] = { L"{", L"}", L";", L"#", L"@", L"&", L"%", L"$", L"*", L"<", L">", L"\\", L"~", L"^" };
    static const wchar_t* words[] = { L"NULL", L"0xDEAD", L"BUG", L"NaN", L"TODO", L"ÿUTF", L"\\r\\n", L"undef" };
    bool boss = s_wave % 5 == 0;
    if (boss) {
        Enemy e; e.g = s_wave == 5 ? L"SEGFAULT" : (s_wave == 10 ? L"STACK OVERFLOW" : L"BLUE SCREEN");
        e.x = W / 2; e.y = -30; e.homeX = W / 2; e.homeY = 70; e.hp = e.maxHp = 30 + s_wave * 4; e.kind = 2; e.fireCd = 1.5f;
        s_en.push_back(e);
        for (int i = 0; i < 4; ++i) { Enemy m; m.g = glyphs[i]; m.homeX = W * (0.2f + 0.2f * i); m.homeY = 130; m.x = m.homeX; m.y = -60 - i * 20; m.kind = 0; s_en.push_back(m); }
        return;
    }
    int cols = std::max(5, std::min(8, (int)(W / 40))), rows = std::min(5, 2 + s_wave / 2);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            Enemy e;
            bool word = r == 0 && s_wave >= 2 && c % 3 == 1;
            e.g = word ? words[(int)(Rnd() * 8) % 8] : glyphs[(int)(Rnd() * 14) % 14];
            e.kind = word ? 1 : 0; e.hp = e.maxHp = word ? 3 + s_wave / 3 : 1;
            e.homeX = W * (c + 0.5f) / cols; e.homeY = 50 + r * 34;
            e.x = e.homeX + (Rnd() - 0.5f) * 80; e.y = -40 - r * 30 - Rnd() * 40;
            e.t = Rnd() * 6; e.fireCd = 2 + Rnd() * 4;
            s_en.push_back(e);
        }
}

void NewGame() {
    s_lives = 3; s_score = 0; s_wave = 0; s_missiles = 3; s_inv = 1.5f;
    s_shots.clear(); s_parts.clear(); s_pops.clear();
    s_px = W / 2; s_py = H - 40;
    SpawnWave();
    s_state = Playing;
    audio::Start();
    audio::s_music = true;
    audio::Play(5, 523, 523, 0.45f, 0.18f);
}

void Hurt() {
    if (s_inv > 0) return;
    --s_lives; s_inv = 2; s_shake = 0.4f;
    Burst(s_px, s_py, Hex(0xFFFFFF), 24, 160, L"|_I");
    audio::Play(4, 440, 90, 0.55f, 0.35f);
    if (s_lives <= 0) {
        s_state = Over; s_best = std::max(s_best, s_score); audio::s_music = false;
        Burst(s_px, s_py, Neon(1), 60, 220, L"#*%@");
        audio::Play(2, 0, 0, 1.2f, 0.6f);
    }
}

void Update(float dt) {
    for (auto& s : s_stars) { s.y += (20 + 60 * s.z) * dt; if (s.y > H) { s.y = 0; s.x = Rnd() * W; } }
    for (auto& p : s_parts) { p.x += p.vx * dt; p.y += p.vy * dt; p.vx *= 0.97f; p.vy *= 0.97f; p.life -= dt; }
    s_parts.erase(std::remove_if(s_parts.begin(), s_parts.end(), [](const Part& p) { return p.life <= 0; }), s_parts.end());
    for (auto& p : s_pops) { p.y -= 30 * dt; p.life -= dt; }
    s_pops.erase(std::remove_if(s_pops.begin(), s_pops.end(), [](const Pop& p) { return p.life <= 0; }), s_pops.end());
    s_shake = std::max(0.f, s_shake - dt);
    if (s_state != Playing) return;
    // player follows the mouse (eased)
    s_px += (std::max(14.f, std::min(W - 14, s_mx)) - s_px) * std::min(1.f, dt * 16);
    s_py += (std::max(H * 0.55f, std::min(H - 20, s_my)) - s_py) * std::min(1.f, dt * 16);
    s_inv = std::max(0.f, s_inv - dt);
    s_fireCd -= dt; s_missileCd -= dt;
    if (s_missiles < 3) { s_missileRegen += dt; if (s_missileRegen > 3.5f) { s_missileRegen = 0; ++s_missiles; } }
    if (s_lDown && s_fireCd <= 0) {
        s_fireCd = 0.13f;
        s_shots.push_back({ s_px - 5, s_py - 14, 0, -620, 0 });
        s_shots.push_back({ s_px + 5, s_py - 14, 0, -620, 0 });
        audio::Play(0, 1500, 380, 0.09f, 0.10f);
    }
    // enemies
    float sway = std::sin(GetTickCount64() / 900.0f) * 18;
    for (auto& e : s_en) {
        e.t += dt; e.flash = std::max(0.f, e.flash - dt);
        if (e.kind == 2) {   // boss: patrol + spread fire
            float tx = W / 2 + std::sin(e.t * 0.7f) * (W / 2 - 70);
            e.x += (tx - e.x) * std::min(1.f, dt * 2); e.y += (e.homeY - e.y) * std::min(1.f, dt * 1.5f);
            e.fireCd -= dt;
            if (e.fireCd <= 0) {
                e.fireCd = std::max(0.6f, 1.6f - s_wave * 0.05f);
                for (int k = -2; k <= 2; ++k) s_shots.push_back({ e.x, e.y + 14, k * 55.f, 170, 2 });
                audio::Play(3, 300, 200, 0.12f, 0.1f);
            }
            continue;
        }
        if (e.dive) {   // dive at the player, then wrap back to formation
            float dx = s_px - e.x; e.vx += (dx > 0 ? 1 : -1) * 120 * dt; e.vx = std::max(-140.f, std::min(140.f, e.vx));
            e.x += e.vx * dt; e.y += (150 + s_wave * 8) * dt;
            if (e.y > H + 20) { e.y = -20; e.dive = false; e.vx = 0; }
        } else {
            float tx = e.homeX + sway + std::sin(e.t * 2 + e.homeY) * 6, ty = e.homeY + std::sin(e.t * 1.3f) * 4;
            e.x += (tx - e.x) * std::min(1.f, dt * 3); e.y += (ty - e.y) * std::min(1.f, dt * 2.5f);
            if (Rnd() < dt * (0.02f + s_wave * 0.006f)) e.dive = true;
        }
        e.fireCd -= dt;
        if (e.fireCd <= 0 && e.y > 0) {
            e.fireCd = 2.5f + Rnd() * std::max(1.f, 6 - s_wave * 0.4f);
            if (Rnd() < 0.5f + s_wave * 0.04f) s_shots.push_back({ e.x, e.y + 10, (s_px - e.x) * 0.15f, 160 + s_wave * 6.f, 2 });
        }
        if (std::fabs(e.x - s_px) < 16 && std::fabs(e.y - s_py) < 16) { e.hp = 0; Hurt(); Burst(e.x, e.y, Neon(e.kind + 1), 12, 120, e.g.c_str()); }
    }
    // shots
    for (auto& s : s_shots) {
        if (s.kind == 1) {   // missile: home on nearest enemy
            int best = -1; float bd = 1e9f;
            for (int i = 0; i < (int)s_en.size(); ++i) if (s_en[i].hp > 0) { float d = std::hypot(s_en[i].x - s.x, s_en[i].y - s.y); if (d < bd) { bd = d; best = i; } }
            if (best >= 0) {
                float ax = s_en[best].x - s.x, ay = s_en[best].y - s.y, l = std::max(1.f, std::hypot(ax, ay));
                s.vx += ax / l * 900 * dt; s.vy += ay / l * 900 * dt;
            }
            float sp = std::hypot(s.vx, s.vy); if (sp > 420) { s.vx *= 420 / sp; s.vy *= 420 / sp; }
            if (Rnd() < 0.8f) s_parts.push_back({ s.x, s.y, -s.vx * 0.1f + (Rnd() - 0.5f) * 30, -s.vy * 0.1f + (Rnd() - 0.5f) * 30, 0.35f, 0.35f, L'.', Neon(2), 10 });
        }
        s.x += s.vx * dt; s.y += s.vy * dt; s.life -= dt;
        if (s.kind == 2) {
            if (std::fabs(s.x - s_px) < 9 && std::fabs(s.y - s_py) < 11) { s.life = 0; Hurt(); }
            continue;
        }
        for (auto& e : s_en) {
            if (e.hp <= 0) continue;
            float hw = e.kind == 2 ? 9.f * (float)e.g.size() + 10 : e.kind == 1 ? 6.f * (float)e.g.size() + 6 : 11;
            if (std::fabs(s.x - e.x) < hw && std::fabs(s.y - e.y) < (e.kind == 2 ? 18 : 12)) {
                int dmg = s.kind == 1 ? 4 : 1;
                e.hp -= dmg; e.flash = 0.08f; s.life = 0;
                if (s.kind == 1) { Burst(s.x, s.y, Neon(2), 18, 170, L"*+"); audio::Play(2, 0, 0, 0.35f, 0.35f); }
                if (e.hp <= 0) {
                    int pts = e.kind == 2 ? 1000 : e.kind == 1 ? 150 : 50;
                    pts = pts * (100 + s_wave * 10) / 100;
                    s_score += pts; s_pops.push_back({ e.x, e.y, 0.8f, L"+" + std::to_wstring(pts) });
                    Burst(e.x, e.y, Neon(e.kind * 2 + (int)e.g[0]), e.kind == 2 ? 80 : 14, e.kind == 2 ? 260 : 130, e.kind == 2 ? L"SEGFAULT#@!" : e.g.c_str());
                    audio::Play(2, 0, 0, e.kind == 2 ? 1.0f : 0.32f, e.kind == 2 ? 0.6f : 0.32f);
                    if (e.kind == 2) s_shake = 0.6f;
                } else if (s.kind == 0) audio::Play(3, 900, 700, 0.04f, 0.06f);
                break;
            }
        }
    }
    s_shots.erase(std::remove_if(s_shots.begin(), s_shots.end(), [](const Shot& s) { return s.life <= 0 || s.y < -30 || s.y > H + 30 || s.x < -40 || s.x > W + 40; }), s_shots.end());
    s_en.erase(std::remove_if(s_en.begin(), s_en.end(), [](const Enemy& e) { return e.hp <= 0; }), s_en.end());
    if (s_en.empty()) {
        if (s_waveDelay <= 0) { s_waveDelay = 1.6f; audio::Play(5, 659, 659, 0.5f, 0.18f); if (s_lives < 5 && s_wave % 3 == 0) { ++s_lives; s_pops.push_back({ W / 2, H / 2, 1.2f, L"1UP" }); } }
        s_waveDelay -= dt;
        if (s_waveDelay <= 0) SpawnWave();
    }
}

void DrawShip(float x, float y, float a) {
    // the caret, armed: an I-beam hull with swept wings
    Color c = Alpha(Hex(0xE8F6FF), a), acc = Alpha(g_th.accent, a);
    ID2D1PathGeometry* g = nullptr; g_d2d->CreatePathGeometry(&g);
    if (g) {
        ID2D1GeometrySink* k = nullptr; g->Open(&k);
        k->BeginFigure({ x, y - 14 }, D2D1_FIGURE_BEGIN_FILLED);
        k->AddLine({ x + 3, y - 4 }); k->AddLine({ x + 13, y + 6 }); k->AddLine({ x + 13, y + 10 }); k->AddLine({ x + 3, y + 7 });
        k->AddLine({ x + 2, y + 11 }); k->AddLine({ x - 2, y + 11 }); k->AddLine({ x - 3, y + 7 }); k->AddLine({ x - 13, y + 10 });
        k->AddLine({ x - 13, y + 6 }); k->AddLine({ x - 3, y - 4 });
        k->EndFigure(D2D1_FIGURE_END_CLOSED); k->Close(); k->Release();
        g_rt->FillGeometry(g, Brush(Alpha(acc, 0.85f)));
        g_rt->DrawGeometry(g, Brush(c), 1.2f);
        g->Release();
    }
    FillR(RF(x - 0.9f, y - 10, 1.8f, 18), c);
    float fl = 4 + Rnd() * 5;
    FillRR(RF(x - 2, y + 11, 4, fl), 2, Alpha(Neon(2), a));
}
} // namespace

bool GameRunning() { return s_state == Playing; }
void GamePause() {
    if (s_state == Playing) s_state = Paused;
    audio::s_music = false;
    KillTimer(g_hwnd, 7);
}
void GameShutdown() { GamePause(); audio::Stop(); }

void GameTick() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    float dt = s_last.QuadPart ? (float)((now.QuadPart - s_last.QuadPart) / (double)s_freq.QuadPart) : 0.016f;
    s_last = now;
    dt = std::min(dt, 0.05f);
    Update(dt);
    RECT r{ (LONG)(s_canvas.left * g_scale) - 2, (LONG)(s_canvas.top * g_scale) - 2, (LONG)(s_canvas.right * g_scale) + 2, (LONG)(s_canvas.bottom * g_scale) + 2 };
    InvalidateRect(g_hwnd, &r, FALSE);
    if (s_state != Playing && s_parts.empty()) KillTimer(g_hwnd, 7);
}
static void EnsureTimer() { s_last.QuadPart = 0; SetTimer(g_hwnd, 7, 15, nullptr); }

// Panel: credits + game. `r` is the whole card.
void GamePaint(const RectF& r) {
    float ts = std::min(g_textScale, 1.3f);
    Color card = g_mica ? (g_th.dark ? Hex(0xFFFFFF, 0.055f) : Hex(0xFFFFFF, 0.72f)) : g_th.surface;
    FillRR(r, 8, card); StrokeRR(r, 8, g_th.border);
    float x = r.left + 18, y = r.top + 14, w = RW(r) - 36;
    DrawAppIcon(x, y + 2, 28);
    Text(L"ModernNotepad 1.4.1", RF(x + 38, y, w - 38, 20 * ts), UiFmt(15, true), g_th.text);
    Text(L"Win32 · Direct2D · DirectWrite · no dependencies", RF(x + 38, y + 20 * ts, w - 38, 16 * ts), UiFmt(11), g_th.dim);
    y += 44 * ts;
    static const wchar_t* credits[] = {
        L"Created by Corey",
        L"Code, icons, synth and game: written by Claude (Anthropic)",
        L"Inspired by the classic Windows Notepad",
    };
    for (auto c : credits) { Text(c, RF(x, y, w, 17 * ts), UiFmt(11.5f), g_th.dim); y += 17 * ts; }
    y += 8;
    // canvas
    float bottomBar = 34 * ts;
    s_canvas = RF(x, y, w, std::max(160.f, r.bottom - 14 - bottomBar - y));
    W = RW(s_canvas); H = RH(s_canvas);
    if (s_stars.empty() || s_stars.size() != 70) { s_stars.clear(); for (int i = 0; i < 70; ++i) s_stars.push_back({ Rnd() * 400, Rnd() * 700, Rnd() }); }
    g_rt->PushAxisAlignedClip(s_canvas, D2D1_ANTIALIAS_MODE_ALIASED);
    FillRR(s_canvas, 6, Hex(0x0B0E1F));
    float sx = s_shake > 0 ? (Rnd() - 0.5f) * 8 * s_shake : 0, sy = s_shake > 0 ? (Rnd() - 0.5f) * 8 * s_shake : 0;
    D2D1_MATRIX_3X2_F old; g_rt->GetTransform(&old);
    g_rt->SetTransform(D2D1::Matrix3x2F::Translation(s_canvas.left + sx, s_canvas.top + sy) * old);
    for (auto& s : s_stars) if (s.x < W && s.y < H) FillR(RF(s.x, s.y, 1 + s.z, 1 + s.z), Hex(0xBFD4FF, 0.25f + s.z * 0.6f));
    if (s_state == Idle) {
        Text(L"GLYPH INVADERS", RF(0, H * 0.22f, W, 40), GlyphFmt(26), Neon(0), 1);
        Text(L"{ } ; # @  vs  |", RF(0, H * 0.22f + 40, W, 24), GlyphFmt(15, false), Neon(1), 1);
        Text(L"Move: mouse   Laser: left click (hold)   Missile: right click", RF(10, H * 0.55f, W - 20, 40), UiFmt(11), Hex(0xC9D3F5), 1);
        Text(L"▶  Click to play", RF(0, H * 0.7f, W, 30), UiFmt(15, true), Hex(0xFFFFFF), 1);
        if (s_best) Text(L"Best " + std::to_wstring(s_best), RF(0, H * 0.82f, W, 20), UiFmt(11.5f), Hex(0x8A93B8), 1);
    } else {
        for (auto& e : s_en) {
            Color c = e.flash > 0 ? Hex(0xFFFFFF) : Neon(e.kind == 2 ? 1 : (int)e.g[0]);
            float fs = e.kind == 2 ? 22.f : e.kind == 1 ? 15.f : 20.f;
            Text(e.g, RF(e.x - 120, e.y - 16, 240, 32), GlyphFmt(fs), c, 1);
            if (e.kind == 2) {
                float bw = 160, fr = (float)e.hp / e.maxHp;
                FillRR(RF(W / 2 - bw / 2, 8, bw, 5), 2, Hex(0xFFFFFF, 0.15f)); FillRR(RF(W / 2 - bw / 2, 8, bw * fr, 5), 2, Neon(1));
            }
        }
        for (auto& s : s_shots) {
            if (s.kind == 0) FillRR(RF(s.x - 1.2f, s.y - 7, 2.4f, 12), 1.2f, Neon(0));
            else if (s.kind == 1) { g_rt->FillEllipse(D2D1::Ellipse({ s.x, s.y }, 4, 4), Brush(Neon(2))); g_rt->FillEllipse(D2D1::Ellipse({ s.x, s.y }, 2, 2), Brush(Hex(0xFFFFFF))); }
            else Text(L"•", RF(s.x - 10, s.y - 10, 20, 20), GlyphFmt(14), Neon(5), 1);
        }
        for (auto& p : s_parts) Text(std::wstring(1, p.ch), RF(p.x - 10, p.y - 10, 20, 20), GlyphFmt(p.size, false), Alpha(p.c, std::max(0.f, p.life / p.max)), 1);
        for (auto& p : s_pops) Text(p.t, RF(p.x - 50, p.y - 10, 100, 20), GlyphFmt(12), Alpha(Hex(0xFFFFFF), std::min(1.f, p.life * 2)), 1);
        if (s_state != Over && (s_inv <= 0 || ((int)(s_inv * 12) % 2 == 0))) DrawShip(s_px, s_py, 1.f);
        // HUD
        Text(L"SCORE " + std::to_wstring(s_score), RF(8, H - 22, 140, 18), GlyphFmt(11), Hex(0xC9D3F5));
        Text(L"WAVE " + std::to_wstring(s_wave), RF(W / 2 - 50, H - 22, 100, 18), GlyphFmt(11), Hex(0x8A93B8), 1);
        std::wstring lives; for (int i = 0; i < s_lives; ++i) lives += L"| ";
        Text(lives, RF(W - 86, H - 22, 80, 18), GlyphFmt(12), Neon(0), 2);
        for (int i = 0; i < 3; ++i) g_rt->FillEllipse(D2D1::Ellipse({ W - 14.f - i * 10, 12 }, 3, 3), Brush(i < s_missiles ? Neon(2) : Hex(0xFFFFFF, 0.15f)));
        if (s_state == Paused) { FillR(RF(0, 0, W, H), Hex(0x000000, 0.45f)); Text(L"PAUSED — click to resume", RF(0, H / 2 - 15, W, 30), UiFmt(14, true), Hex(0xFFFFFF), 1); }
        if (s_state == Over) {
            FillR(RF(0, 0, W, H), Hex(0x000000, 0.35f));
            Text(L"GAME OVER", RF(0, H * 0.35f, W, 40), GlyphFmt(26), Neon(1), 1);
            Text(L"Score " + std::to_wstring(s_score) + L"   ·   Best " + std::to_wstring(s_best), RF(0, H * 0.35f + 42, W, 22), UiFmt(12.5f), Hex(0xFFFFFF), 1);
            Text(L"Click to play again", RF(0, H * 0.6f, W, 24), UiFmt(13, true), Hex(0xC9D3F5), 1);
        }
    }
    g_rt->SetTransform(old);
    g_rt->PopAxisAlignedClip();
    StrokeRR(s_canvas, 6, Alpha(g_th.border, 0.8f));
    // controls
    float by = s_canvas.bottom + 8;
    s_btnPlay = RF(x, by, 92 * ts, 26 * ts);
    s_btnMusic = RF(s_btnPlay.right + 6, by, 92 * ts, 26 * ts);
    s_btnSfx = RF(s_btnMusic.right + 6, by, 92 * ts, 26 * ts);
    auto btn = [&](const RectF& b, const wchar_t* t, bool on) {
        FillRR(b, 5, on ? Alpha(g_th.accent, 0.22f) : g_th.editor); StrokeRR(b, 5, g_th.border);
        Text(t, b, UiFmt(11.5f), g_th.text, 1);
    };
    btn(s_btnPlay, s_state == Playing ? L"❙❙  Pause" : (s_state == Paused ? L"▶  Resume" : L"▶  Play"), false);
    btn(s_btnMusic, audio::s_musicMute ? L"Music: off" : L"Music: on", !audio::s_musicMute);
    btn(s_btnSfx, audio::s_sfxMute ? L"Sound: off" : L"Sound: on", !audio::s_sfxMute);
    if (audio::s_failed) Text(L"no audio device", RF(s_btnSfx.right + 8, by, 120, 26 * ts), UiFmt(10.5f), g_th.faint);
}

bool GameMouse(UINT msg, float x, float y, WPARAM wp) {
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        if (In(s_btnPlay, x, y)) {
            if (s_state == Playing) GamePause();
            else if (s_state == Paused) { s_state = Playing; audio::Start(); audio::s_music = true; EnsureTimer(); }
            else { NewGame(); EnsureTimer(); }
            Invalidate(); return true;
        }
        if (In(s_btnMusic, x, y)) { audio::s_musicMute = !audio::s_musicMute; Invalidate(); return true; }
        if (In(s_btnSfx, x, y)) { audio::s_sfxMute = !audio::s_sfxMute; Invalidate(); return true; }
    }
    if (!In(s_canvas, x, y)) { if (msg == WM_LBUTTONUP) s_lDown = false; return false; }
    s_mx = x - s_canvas.left; s_my = y - s_canvas.top;
    if (msg == WM_MOUSEMOVE) { if (s_state == Playing) SetCursor(nullptr); return s_state == Playing; }
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        if (s_state == Idle || s_state == Over) { NewGame(); EnsureTimer(); }
        else if (s_state == Paused) { s_state = Playing; audio::Start(); audio::s_music = true; EnsureTimer(); }
        s_lDown = true; SetCapture(g_hwnd);
        return true;
    }
    if (msg == WM_LBUTTONUP) { s_lDown = false; ReleaseCapture(); return true; }
    if (msg == WM_RBUTTONDOWN && s_state == Playing) {
        if (s_missiles > 0 && s_missileCd <= 0) {
            --s_missiles; s_missileCd = 0.35f;
            s_shots.push_back({ s_px, s_py - 12, (Rnd() - 0.5f) * 120, -260, 1 });
            audio::Play(1, 180, 620, 0.35f, 0.22f);
        } else audio::Play(3, 220, 180, 0.05f, 0.05f);
        return true;
    }
    if (msg == WM_RBUTTONUP) return s_state == Playing;
    return false;
}

} // namespace app
