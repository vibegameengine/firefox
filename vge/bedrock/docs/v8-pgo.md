# Optimized V8 build: PGO and ThinLTO

Chrome ships V8 compiled with profile-guided optimization and ThinLTO. A plain
`-O3` V8 runs the V8 C++ runtime (inline cache misses, lazy compilation, the
parser, JSON, strings, shape transitions) clearly slower, and page start-up is
dominated by that runtime. Our V8 is therefore built the same way, with a
profile taken on our own workload.

## Toolchain

V8 is compiled with the same clang that builds Gecko
(`~/.mozbuild/clang`, LLVM 22), not with Chromium's bundled clang:

- `llvm-profdata` from that toolchain must read the raw profiles the
  instrumented V8 writes (Chromium's clang 24 writes raw version 11, LLVM 22
  reads up to 10);
- with ThinLTO the bitcode in `wee8.lib` is optimized by Gecko's `lld-link`
  when `xul.dll` is linked, so it has to be the same LLVM.

Common `args.gn` additions on top of `out.gn/vge`:

```
clang_base_path = "C:/Users/pavel/.mozbuild/clang"
clang_use_chrome_plugins = false
clang_version = "22"
```

The V8 checkout needs an empty `chrome/build/win64.pgo.txt`: the PGO config
lists it as an input even when `pgo_data_path` is given.

## Steps

1. Instrumented V8: `chrome_pgo_phase = 1`, build `v8_monolith`, copy it to
   `dist/Lib/Win64MD/wee8.lib`.
2. Gecko with the profile runtime linked in:
   `VGE_V8_PROFILE_RUNTIME=C:/Users/pavel/.mozbuild/clang/lib/clang/22/lib/windows/clang_rt.profile-x86_64.lib ./mach build`.
3. Training: `python vge/bedrock/tools/pgotrain.py <dir> --seconds 30 <urls...>`.
   Each page is closed with `WM_CLOSE`, so the process leaves `main` normally
   and the profile runtime writes `vge-<n>-<pid>.profraw` at exit. The second
   process of each run is killed at shutdown and its file is unreadable; it is
   skipped.
4. `llvm-profdata merge --failure-mode=warn -o v8.profdata <dir>/*.profraw`;
   the current profile is kept in `C:/Projects/v8-build/pgo/v8.profdata`.
5. Optimized V8: `chrome_pgo_phase = 2`, `pgo_data_path = <profdata>`,
   `use_thin_lto = true`; copy to `wee8.lib` and build Gecko without
   `VGE_V8_PROFILE_RUNTIME`.

The profile has to be retaken when V8 changes enough that most functions no
longer match it (clang reports hash mismatches as warnings, which the build
suppresses).

## Effect (PGO alone, same machine, median of 5)

| test | -O3 | PGO | Chrome 149 |
|---|---|---|---|
| compile and first run of 400 new functions | 5 ms | 3 ms | 2.4 ms |
| JSON.stringify + parse of 20000 objects | 19-20 ms | 11 ms | 12 ms |
| string split/replace/upper, 50000 | 21-22 ms | 14-15 ms | 13.7-16.7 ms |
| object shape churn | 3 ms | 1 ms | 0.6 ms |
