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

**Broke (toolchain)**
- First run trapped on `int3`: clang saw a known function called through a mismatched type (UB) and replaced
  the rest of `main` with traps. Fixed by laundering the pointer; rule noted in CLAUDE.md.

## 2026-11-13 — M1: probe + MCP bridge; game keeps running unfocused

**Changed**
- `probe/` → `out/nfsu2_probe.asi` (clang-cl, static CRT; imports WS2_32, bcrypt, KERNEL32). Exe SHA-256 check
  (hooks disabled on mismatch). MinHook on `Direct3DCreate9` → vtable patch `IDirect3D9::CreateDevice` (16) →
  `IDirect3DDevice9::Present` (17). Main-thread task queue drained in Present. TCP JSON lines on 127.0.0.1:47800:
  `ping info read_mem write_mem aob_scan log_tail screenshot keep_active`. Log: `work/profile/scripts/nfsu2_probe.log`.
- `tools/mcp_bridge.py` (mcp 2.3.0, registered in `.mcp.json` as `nfsu2`), `tools/probe.py` CLI,
  `tools/probe_client.py`, `tools/re/xrefs.py` (pefile + capstone static helpers), `tests/mcp_bridge_test.py`.
- `docs/symbols.csv` started.

**Verified**
- `MZ` at 0x400000; `TRACKS\%s.BUN` found by aob_scan at 0x7A0A30 = its .rdata file offset 0x3A0A30 mapped by the
  section table; back-buffer screenshot of the title screen. `tests/mcp_bridge_test.py`: 0 failures through a real
  MCP stdio client.
- Device: fullscreen 2560x1440 A8R8G8B8, DISCARD, immediate. Present runs on the thread that created the device.
- Focus: without the fix the game drops from 61 fps to 0 the moment it loses focus (and minimizes). Cause found
  statically: `MainWndProc` 0x5CCD60 writes `g_AppActive` 0x870988; the pump at 0x5BF212 uses blocking
  `GetMessageA` while it is 0. Probe `keep_active` (default on, env `NFSU2_KEEP_ACTIVE=0` to disable) rewrites
  focus-loss messages and pins the flag: game kept 60 fps unfocused + minimized, screenshot OK (user clicked away).

**Open**
- Fullscreen still minimizes visually on focus loss (harmless for automation; screenshots come from the back buffer).
- Input while unfocused: the game now polls DirectInput, but foreground-cooperative devices won't deliver data → M3.
- `WM_ACTIVATE` path calls into the object at `[0x82B884]` (0x4810F0 / 0x481210 / 0x483B40) — unknown, maybe a pause.
