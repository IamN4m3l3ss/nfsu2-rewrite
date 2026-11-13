# DEVLOG

## 2026-11-13 — M0: game runs under Wine

**Changed**
- Repo skeleton, `.gitignore`, `.env.local` (gitignored).
- Dedicated Wine prefix `~/Games/nfsu2-rewrite/prefix` (system wine 11.21, `wineboot -i`).
- `tools/run_game.sh`: hash check, runs from `work/profile/` (symlink copy of GAME_DIR + all of its `scripts/`),
  `WINEDLLOVERRIDES="dinput8=n,b;d3d9=n,b"`, logs to `work/logs/`, `--kill`, optional `--desktop WxH`.

**Verified**
- `SPEED2.EXE` SHA-256 matches the expected NA v1.2 hash.
- Game boots to the "Press Enter Key" title screen in ~30 s (screenshots in `work/oracle/`); user confirmed the
  first run looked right.
- Exe imports: advapi32, ddraw, dinput8, dsound, gdi32, kernel32, netapi32, shell32, shfolder, tapi32, user32,
  winmm, ws2_32, d3d9.
- Settings go to `HKCU\Software\EA Games\Need for Speed Underground 2`; save folder is
  `C:\users\<user>\AppData\Local\NFS Underground 2` (created in our prefix, so the user's saves are separate).

**Broke / learned**
- A stripped "clean" profile (no third-party ASIs) does not work: without WidescreenFix the game stops at
  "please insert disc 2"; without HDContentSupport the HD UI data renders garbled text. User decision: always run
  with the full mod set (now in `/mnt/m2drive/Games/Need for Speed - Underground 2/scripts/`). CLAUDE.md updated.
- Unfocused window: the game keeps a core at ~100% but stops drawing (identical frames 1.3 s apart).
  `--desktop` (Wine virtual desktop) launches the game but its effect on the pause is untested.

**Open**
- Main menu (past "Press Enter") not reached yet — needs input; will come with M3 injection or a user keypress.
- Focus pause: test virtual desktop, or handle focus loss in the probe.

## 2026-11-13 — toolchain decided: clang-cl + xwin

**Changed**
- xwin 0.10.0 (`cargo install`), x86 CRT + SDK 10.0.26100 splatted to `~/.local/share/xwin` (license accepted by
  the user). `cmake/clang-cl-x86.cmake`, `CMakePresets.json` (`win32`), top-level `CMakeLists.txt`.
- Vendored MinHook v1.3.4 and nlohmann/json v3.12.0 in `third_party/`. `.venv/` with `mcp` 2.3.0.
- Prefix: `HKCU\Software\Wine\WineDbg\ShowCrashDialog=0`, so crashes don't pop up a dialog on the user's screen.

**Verified**
- `tests/abi/abi_test.exe` (clang-cl, imports only KERNEL32): 14/14 MSVC-ABI checks pass under Wine.
  Same source with MinGW: 5 fail (two-slot virtual dtors, Itanium RTTI, 12-byte member pointers, long double).

**Broke**
- First run trapped on `int3`: clang saw a known function called through a mismatched type (UB) and replaced
  the rest of `main` with traps. Fixed by laundering the pointer; rule noted in CLAUDE.md.
