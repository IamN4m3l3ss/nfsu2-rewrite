# nfsu2-rewrite — CLAUDE.md

Private, personal project. Read this whole file before doing anything.

## Goal

Remake Need for Speed: Underground 2 (PC, 2004) as our own C++ source code, written from scratch, behaving the same
as the original. **Game first, everything else later.**

Method (re3/reVC style — "hook and replace"):
1. An ASI plugin loaded into the original game exposes it to Claude Code through an MCP server: read/write memory,
   hook and log functions, inject input, take screenshots. Claude uses it to map the game live.
2. Our rewritten code lives in a separate DLL that the ASI loads and can **hot-reload**. Each rewritten function is
   hooked in place of the original and can be switched between original and rewrite at runtime.
3. Every rewritten function is compared live against the original (same input → same output/state) before it
   counts as done. The game stays playable the whole time.
4. When (almost) everything is replaced, the original exe is no longer needed → standalone build → port
   (renderer, platforms). That is **later** and not part of this phase.

## How we work (rules for Claude Code)

The user wants Claude Code to do the engineering. You drive: plan, implement, build, run, test, debug.
Only involve the user for what they alone can do: decisions about scope, and things that need their eyes/hands
that the automation can't cover yet. Give them one exact command and what to look for.

1. **Verify before coding.** Before using any API, tool flag, package name, Wine/Proton option or version, check it
   against the primary source (headers, repo, docs, `pacman -Si`, `--help`). Never code against an assumed API.
   The user has lost hours to that before. If something can't be verified, say so.
2. **Automate verification.** Every milestone has a check you run yourself (MCP queries, logs, screenshots, exit codes).
3. **The original game is the oracle.** When in doubt about behaviour, measure the original through the MCP.
4. **Never commit** `SPEED2.EXE`, game data, saves, or other people's binaries. Reading other projects' code and
   docs for knowledge is fine; all code in this repo is written by us.
5. Small commits. Keep `docs/DEVLOG.md` (date, what changed, what was verified, what broke).
6. Stuck on the same problem after ~3 attempts: stop, write down what you tried, propose options to the user.

## Local paths (user: fill in on first run, store in `.env.local`, gitignored)

- `GAME_DIR=` — the game folder (contains `SPEED2.EXE`)
- `WINEPREFIX=` / Proton setup used to run it
- `CAMTK_SRC=` — optional: the user's private fork of Archie's NFSU2 Camera ToolKit (C++ ASI; knows camera structs)

## Known facts (verified 12 Nov 2026 — don't re-derive)

**Executable:** North American v1.2 no-CD, 4,800,512 bytes,
SHA-256 `f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7`. All addresses in this project are for
this build. Check the hash at startup and refuse anything else.

**Compiler/language:** Microsoft Visual C++ (MSVC CRT startup, C++ static constructors via `_initterm`, C++ RTTI
strings like `.?AVexception@@`). Expect `__thiscall`, `__stdcall`, `__cdecl`, vtables, MSVC struct layout.
Our replacement code must match MSVC's ABI exactly where it touches game objects.

**Graphics/input/audio:** Direct3D 9 (`Direct3DCreate9` is the only D3D import; the rest goes through vtables),
D3DX9 statically linked into the exe, DirectInput 8, DirectSound.

**The user's game folder** (project copy: `/mnt/m2drive/Games/Need for Speed - Underground 2`) is a repack with an
ASI loader (`dinput8.dll`, Ultimate ASI Loader), DXVK 3.1 (`d3d9.dll`) and `scripts/`: `NFSU2ExtraOptions.asi`,
`NFSU2HDContentSupport.asi`, `NFSUnderground2.WidescreenFix.asi`. **These are required (user decision, 13 Nov 2026)**:
WidescreenFix's `NoOpticalDriveFix` bypasses "please insert disc 2", and the data has an HD UI that needs
HDContentSupport (without it, menu text is garbled). Always run with the full `scripts/` set plus our probe.
They patch game code, so when mapping a function, check whether one of them hooks it (compare bytes against the exe
on disk). `tools/run_game.sh` runs from a symlink copy in `work/profile/`, so the user's folder is never written to.

**Floating point (measured 14 Nov 2026):** on the game thread the x87 control word is `0x007F` — 24-bit (single)
precision, round to nearest, exceptions masked (D3D9 default); MXCSR `0x1FA0`. So x87 `+ - * / fsqrt` round like
SSE single precision: rewrites in plain `float` match in normal ranges. Transcendentals (`fsin/fcos/fpatan`) ignore
precision control → use the same x87 instructions; float→int conversions must copy the original's method. Always
diff-test. **NaN contract:** bit-exact for every non-NaN result; where the original yields NaN ours must yield NaN,
but sign/payload may differ (x87 and SSE propagate NaN operands differently; emulating that on every op isn't worth
it). Compare floats with `game::same_float`. `tools/re/x87lift.py` lifts straight-line x87 code to C++ (game/src/lift.h).

**Register contract (learned 14 Nov 2026):** the exe was built with whole-program optimisation; callers rely on
registers a small function doesn't touch (ECX live across bSin) and on pointers left in EAX by "void" functions
(bScaleAdd's out). Rewrites go through `NFSU2_REWRITE_N` entry stubs (save ECX/EDX), return what the original leaves
in EAX, and every diff test file includes a register test (`game/src/regs.h`, `tools/re/regcontract.py`).

**Data:** EA chunk bundles (`u32 id, u32 size`; high bit `0x80000000` = container). `GLOBAL/GlobalB.lzc` and
`FRONTEND/FrontB.lzc` are JDLZ-compressed (magic `JDLZ`); uncompressed `GLOBAL/GLOBALB.BUN` also exists.
Tracks: `TRACKS/L4Rx.BUN` + `TRACKS/STREAML4Rx.BUN` (x = A,B,C,D,F,G,H,R; STREAML4RA is 118 MB = most of Bayview),
per-event routes `TRACKS/ROUTESL4Rx/`. Event table chunk `0x00034201` in GLOBALB (IDs from 4001).
Cars: `CARS/<NAME>/GEOMETRY.BIN, TEXTURES.BIN, VINYLS.BIN`. The exe loads tracks via format strings
`TRACKS\%s.BUN`, `TRACKS\STREAM%s.BUN`, `Tracks\Routes%s\Paths%d.bin`.

**Scale (from NFSU2Recomp's notes, as a reference number):** ~23,278 functions in the exe. Use it to track coverage.

## Architecture

```
SPEED2.EXE (original, under Wine/Proton)
  └─ dinput8.dll (ASI loader) ─► nfsu2_probe.asi   (ours: stable loader — D3D hooks, keep_active, log)
        └─ nfsu2_core.dll (ours: hot-reloadable core — server, input, car, schedules; reloads when rebuilt)
        ├─ TCP JSON-RPC server on 127.0.0.1:<port>   (game side)
        ├─ hook manager (MinHook, x86): table of {addr, original, replacement, enabled}
        ├─ input injector (DirectInput8 GetDeviceState/GetDeviceData hooks)
        ├─ frame hook (IDirect3DDevice9::Present): screenshots, safe point for hot reload + queued calls
        └─ loads nfsu2_game.dll (ours: the rewrite) from a temp copy, hot-reloadable
tools/mcp_bridge.py  (stdio MCP server for Claude Code ↔ TCP to the probe)
tools/run_game.sh    (launch, clean profile, wait for probe, boot to free roam)
```

Probe hot reload (done 14 Nov 2026): the loader loads `out/nfsu2_core.dll` (env `NFSU2_CORE`, set by
`run_game.sh`) from a temp copy and reloads it at Present when the file changes (or on `reload`). The old core stops
its server, fails queued tasks, restores every vtable slot it patched, and hands its state (DirectInput devices +
key→action maps) to the new core as JSON. Interface: `probe/common/api.h` (C types only; each DLL has its own static
CRT). Loader changes still need a game restart.

Rules for the probe:
- **Anything that calls game code or changes hooks runs on the game's main thread**, queued and executed at the
  frame safe point (Present hook). Never call game functions from the server thread.
- Hot reload: at the safe point, disable all rewrite hooks → FreeLibrary → copy the new DLL to a fresh temp name →
  LoadLibrary → re-register → re-enable previous switches. Windows locks loaded DLLs, hence the temp copy.
- The probe must survive a crash in the rewrite where possible (log exceptions with address + stack, then stop).

Toolchain for the 32-bit Windows DLLs, built on Linux — **decided 13 Nov 2026: clang-cl + xwin** (MSVC ABI).
`cmake --preset win32` (toolchain `cmake/clang-cl-x86.cmake`, xwin 0.10.0 splat in `~/.local/share/xwin`, static
release CRT `/MT`). `tests/abi/abi_test.exe` checks thiscall, vtables, RTTI and layout: 14/14 pass under Wine;
MinGW fails 5 (two-slot virtual dtors, Itanium RTTI, 12-byte member pointers, 10-byte long double).
Calling a known function through a mismatched function type is UB that clang turns into `int3`: keep game
addresses opaque (integers / volatile), never cast our own functions to game signatures.

## Repository layout

```
nfsu2-rewrite/
  CLAUDE.md  docs/DEVLOG.md
  docs/symbols.csv        addr,name,signature,status(unknown|named|rewritten|verified),notes
  docs/structs/           one file per reverse-engineered struct/class
  probe/                  the ASI (server, hooks, input, frame hook, loader)
  game/                   the rewrite DLL, one folder per system (frontend/, camera/, car/, world/, ...)
  tools/                  run_game.sh, mcp_bridge.py, helpers, Ghidra scripts
  tests/                  diff tests + scripted scenarios (record/replay files)
  third_party/            MinHook, nlohmann/json (pinned, verified versions)
  .env.local work/ out/   gitignored
```

## Milestones (each with an acceptance check)

**M0 — Game runs under Wine/Proton** with the user's game folder, then with a clean profile (only `dinput8.dll` +
our probe). ✔ Game reaches the main menu; user confirms once; screenshots saved to `work/oracle/`.
Check how the game behaves when unfocused/minimised; if it pauses, run it in a Wine virtual desktop or gamescope
(verify the options first).

**M1 — Probe + MCP bridge.** Tools: `ping`, `read_mem`, `write_mem`, `aob_scan`, `log_tail`, `screenshot`.
✔ Claude Code reads the exe header from memory, reads a known constant, gets a screenshot of the menu.

**M2 — `tools/run_game.sh`.** Clean profile (move other ASIs aside, restore on exit), launch, wait for the probe port,
exit codes for failure. `--kill` to stop the game. ✔ Launch + connect + screenshot + kill, 5 times in a row, unattended.

**M3 — Input injection.** Hook DirectInput so the probe feeds keys/axes the game reads (no OS key events, no focus
needed). Tools: `press`, `hold`, `axis`, `release_all`. ✔ Navigate one menu screen by injected input, confirmed by
screenshots.

**M4 — Menus → free roam (first RE target).** First get from boot to free roam with injected input + screenshots.
Then find the frontend state machine / load-save path and add `boot_to_freeroam` that skips intro movies and loads
a test save directly. Find where the game reads/writes saves (game folder `SAVE/` and/or the prefix's documents —
check). ✔ `run_game.sh --freeroam` puts the car in Bayview in under ~30 s, unattended, 5/5 runs.

**M5 — Telemetry + driving.** Map the player car struct: position, rotation, speed, gear, rpm, inputs.
Tools: `car_state`, `drive_script` (timed inputs), `record_inputs` / `replay_inputs` with state checkpoints
(expect drift: the game runs on frame time). ✔ A scripted drive along a known road ends within tolerance of the
recorded end position.

**M6 — Rewrite DLL + hook table + hot reload.** `nfsu2_game.dll` registers replacements by address; MCP tools
`rewrite_list`, `rewrite_enable/disable`, `rewrite_reload`. ✔ Change a trivial replacement, rebuild, hot-reload
without restarting the game, see the new behaviour.

**M7 — Diff harness.** For a function: snapshot the relevant memory, run the original, capture outputs/state,
restore, run the rewrite, compare. Pure functions directly; stateful ones via snapshots; whole systems by running
scripted scenarios with original vs rewrite and comparing telemetry. ✔ Prove it on a few leaf functions
(math/string/hash helpers), including one deliberately broken rewrite that the harness catches.

**M8+ — System by system.** Suggested order (adjust as mapping shows dependencies): leaf utilities → file/chunk
loading (formats are well documented) → frontend/menus → camera → car physics/handling → world streaming →
AI/traffic/cops → audio → rendering last (renderer is the future port boundary). Each function: name it in
`docs/symbols.csv`, write it in `game/`, diff-test it, mark `verified`. Report coverage (verified / total) in DEVLOG.

## Later (not now)

- Standalone exe once the original isn't needed; then port: render abstraction with DX12 (Windows, Linux via
  Proton) and/or Vulkan (also for mobile). User leans DX12 for PC; decide at port time.
- Photo mode (camera system is an early RE target anyway), renderer upgrades incl. ray tracing.
- Launcher with mod toggles: Unlimiter, HD reflections (Aero_'s NFS HD Reflections), rain droplets (ThirteenAG's
  FusionRainDroplets), Extra Options, Widescreen Fix — re-implemented natively in the rewrite.
- TexMod packs: `.tpf` = every little-endian u32 XORed with `0x3FA43FA4` → ZipCrypto ZIP with TexMod's fixed
  42-byte password → `texmod.def` lines `0xHASH|file`; hash = `~crc32()` (zlib) over mip level 0 bytes as the
  game uploaded them. Loose `*_0xHASH.dds` packs too.
- Add-on cars, custom tracks.

## Verify before coding (open items)

- Wine vs Proton(-CachyOS) setup for running the game outside Steam; package names on CachyOS.
- ~~Toolchain~~ done: clang-cl + xwin (see Architecture).
- ~~Versions~~ done: MinHook v1.3.4 (x86 ok), nlohmann/json v3.12.0, vendored in `third_party/`.
- ~~MCP SDK~~ done: `mcp` 2.3.0 in `.venv/`; API is `from mcp.server.mcpserver import MCPServer, Image`,
  `@server.tool()`, `server.run()` (stdio). Not the 1.x `FastMCP` import path.
- ~~Unfocused pause~~ done: it stops (blocking `GetMessageA` while `g_AppActive` 0x870988 is 0); the probe's
  `keep_active` (default on) keeps it at 60 fps unfocused/minimized. See DEVLOG. Input while unfocused → M3.
- A Ghidra MCP server usable from Claude Code (which project/version), to sync names between Ghidra and symbols.csv.

## References (read for knowledge; write our own code)

- NFSU2Recomp docs (the user has the zip): `PROJECT.md` (boot sequence, CRT, threads, D3DX effect findings, run-time
  generated code), `D3DX_HLE.md`, `GRAPHICS_RESEARCH.md` — github.com/TalhaMudo/NFSU2Recomp
- re3/reVC (GTA III/VC rewrites) — the hook-and-replace workflow this project copies
- Names/structs for NA v1.2: nlgxzef/NFSU2Unlimiter, ExOptsTeam/NFSU2ExOpts, ThirteenAG/WidescreenFixesPack,
  xan1242/NFSU-XtendedInput, the user's Camera ToolKit fork
- Formats: whoismept/OpenUG2 `docs/FORMATS.md`, bdrtr/PryHUB (gizmo-nfs), NFSTools/GlobalLib, NFSTools/Binary
- Tools: x64dbg MCP servers (e.g. bromoket/x64dbg_mcp), Cheat Engine MCP bridges — usable alongside the probe
