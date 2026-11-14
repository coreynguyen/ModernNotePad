# Contributing

Thanks for helping! A few ground rules keep the project small, portable and legally clean.

## Ground rules

1. **No dependencies.** Only what ships with Windows 10/11 (Win32, Direct2D, DirectWrite, DWM, WIC, UIA, WinHTTP, ...).
   No NuGet packages, no vendored libraries from other authors.
2. **Original code and artwork only.** Don't paste code from other editors, Microsoft samples under restrictive
   licenses, or decompiled binaries. Icons are vector paths in `src/app/gfx.cpp`; add new ones the same way.
3. **Everything you contribute is released under the [Unlicense](LICENSE)** (public domain). By opening a pull
   request you confirm you have the right to do that.
4. **Keep `src/core` portable** (C++17, no `<windows.h>` outside `#ifdef _WIN32`) so its tests keep running on Linux.
5. **Data safety first.** Anything that touches saving, loading or session recovery needs a test or a written
   explanation of how it was verified.

## Building and testing

```
msbuild ModernNotepad.sln /p:Configuration=Release /p:Platform=x64
g++ -std=c++17 -O2 -Isrc tests/core_tests.cpp src/core/*.cpp -o core_tests && ./core_tests
```

CI runs both on every pull request.

## Style

4-space indent, braces on the same line, `PascalCase` functions, `camelCase` locals, `s_` statics, `g_` globals.
Comments explain *why*, not *what*.
