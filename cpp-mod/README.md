# CairnMap — UE4SS C++ mod

Build: GitHub Actions (`.github/workflows/build-cpp-mod.yml`) compiles
`CairnMap.dll` on windows-latest against Okaetsu/RE-UE4SS pinned to `c2ac246`
(the UE4SS v3.0.1 ABI shipped by the Workshop item). Install the artifact as
`<UE4SS>/Mods/CairnMap/dlls/main.dll` and add `CairnMap : 1` to mods.txt.

Local development: the pure core will compile natively on Linux for unit
tests (see docs/SPEC-2.0.md §7.1); only the DLL needs the CI.
