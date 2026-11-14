# util_mxs: MAXScript interpreter

Imported from **MAX2Modern** (same author), an independent recreation of 3D Studio MAX 2.
Portable C++17, no dependencies. Released under the same [Unlicense](../../LICENSE) as the rest of ModernNotepad.

ModernNotepad uses it for *Evaluate expression* (`src/app/evaluate.cpp`), with file and process access denied
(`Engine::CapFile | Engine::CapProcess`) and a 3-second watchdog per run.

MAXScript is a language designed by Autodesk; this is an independent implementation and is not affiliated with Autodesk.
