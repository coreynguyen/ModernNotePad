#include "core/text_doc.h"
#include "core/search.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
using namespace np; using C = std::chrono::steady_clock;
static double ms(C::time_point a) { return std::chrono::duration<double, std::milli>(C::now() - a).count(); }
int main(int argc, char** argv) {
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    printf("file %.1f MB\n", b.size() / 1048576.0);
    auto t = C::now(); Decoded d = DecodeBytes(b); printf("decode        %8.1f ms\n", ms(t));
    b.clear(); b.shrink_to_fit();
    TextDoc doc; t = C::now(); doc.SetText(std::move(d.text)); printf("SetText       %8.1f ms  lines=%d\n", ms(t), doc.LineCount());
    t = C::now(); size_t L = 0; for (int i = 0; i < 100; ++i) L += doc.Length(); printf("Length x100   %8.1f ms\n", ms(t));
    t = C::now(); size_t o = doc.Offset(doc.End()); printf("Offset(end)   %8.1f ms (%zu)\n", ms(t), o);
    np::Pos p{ 10, 0 };
    t = C::now();
    for (int i = 0; i < 50; ++i) { p = doc.Replace(p, p, L"x", p, p, true); o += doc.Offset({ doc.LineCount() - 1, 0 }); }
    printf("50 keystrokes near top + Offset(end) each: %8.1f ms\n", ms(t));
    t = C::now(); np::Searcher s; SearchOpts so; so.pattern = L"needle"; s.Prepare(so); std::vector<Match> m; s.FindAll(doc, m); printf("FindAll       %8.1f ms (%zu)\n", ms(t), m.size());
    return (int)(o & 1);
}
