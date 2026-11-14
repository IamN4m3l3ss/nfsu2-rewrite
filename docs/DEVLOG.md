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

## 2026-11-13 — M2 + M3: unattended launch loop, DirectInput injection

**Changed**
- `tools/run_game.sh`: waits for probe + presented frames (exit 5), kills a running game first, `--kill` verifies
  (exit 6). `tools/probe.py`: `ready`, `shot-wait` (screenshot until not black), `press/hold/release/release_all/input`.
- `tests/m2_loop.sh`: launch → ready → screenshot of title → kill, N times.
- `probe/src/input.cpp`: MinHook `DirectInput8Create` → patch `IDirectInput8::CreateDevice` (3) → device vtable
  `Acquire` 7, `GetDeviceState` 9, `GetDeviceData` 10, `SetDataFormat` 11, `SetCooperativeLevel` 13,
  `BuildActionMap` 29, `SetActionMap` 30. Devices not seen at creation are classified with `GetCapabilities`.
  Injected keys: merged into `GetDeviceState`, and as action-tagged events (`uAppData` from the captured action map)
  through `GetDeviceData`. Under keep_active, failed reads (unfocused) succeed with injected data only.
- `tools/dik.py` generated from SDK `dinput.h`. MCP tools `press hold release release_all input_state keep_active`.
- `docs/structs/input.md`: devices, keyboard read function, frontend action map.

**Verified**
- M2: `tests/m2_loop.sh 5` → 5/5 OK, probe ready ~1.2 s after launch, title drawn by frame 44.
- M3: `tests/m3_input.py` → 4/4 PASS: injected Enter moves title → "Save/Load: create a new profile?" (mean pixel
  difference 40.2), with the game unfocused.

**Broke / learned**
- First injected Enter did nothing: the keyboard is DirectInput 8 *action-mapped*, and the game reads only
  `dwData` + `uAppData` (action id) from events. Found by disassembling the read site (`0x5C1840`, caller address
  logged by the hook) and fixed by hooking `SetActionMap` to learn key → action ids.
- The title screen ignores Enter for a few seconds after it appears; harness retries until the screen changes.

**Open**
- `axis` (analog/gamepad injection) not done: the game's gamepad path isn't mapped yet (and the WidescreenFix
  adds XInput). Keyboard covers menus and driving for now.
- In-game driving action maps (53/19/9 actions) not yet seen in `SetActionMap`; check when driving (M4/M5).

## 2026-11-13 — M4 (input-driven): boot to free roam in ~17 s

**Changed**
- `tools/boot_freeroam.py` + `tools/run_game.sh --freeroam` (exit 7 on failure): the user's key sequence
  title Enter → Load "Nameless" → Load complete → Main Menu, Left → Career, Enter ×3 → Garage → "Return to Explore
  Mode?", Left → Yes, Enter → LOADING → free roam. Each Enter waits for the screen to react (diff threshold), the
  title Enter retries every 1.5 s. Free roam = picture moves away from the LOADING screen (stopgap until M5's
  car struct). `tests/m4_loop.sh N` keeps per-run step logs, probe log and Wine log in `work/logs/m4/`.
- Reference screenshots of every step: `work/oracle/m4/`.

**Verified**
- Saves: `C:\users\<user>\AppData\Local\NFS Underground 2\<profile>` in the prefix (user copied the "Nameless"
  profile in; the game loads it from the Load screen).
- `tests/m4_loop.sh 5` twice → 10/10 OK, free roam 16.7–17.5 s after launch (18–19 s incl. process start).

**Broke**
- First 5-run attempt: 3/5. One step-threshold miss (fixed: per-step thresholds, 6 s timeouts) and **one game
  crash**: `Unhandled page fault on write access to 0D2B0184 at 0043FE97` (main thread, ~14 s in, menus/loading).
  0x43FE97 is the game's block allocator filling a new block with 0xAAAAAAAA (free-list tag 0x44443333), i.e. a
  memory pool overflowed or was corrupted. Not reproduced in the next 10 boots. Unknown whether it also happens in
  normal play, or comes from boot pace / mods. Open.

**Open**
- Second half of M4: find the frontend state machine / save-load path and boot directly into free roam without
  menu input.

## 2026-11-14 — M4: faster, verified boot (~13 s); Camera Toolkit workspace as reference

**Changed**
- `tools/boot_freeroam.py` rewritten around screen classification: each step waits until two consecutive frames
  match that step's reference shot (`work/boot_refs/`, 320x180, made from `work/oracle/m4/`; gitignored because they
  are game imagery). The title Enter is re-pressed only while the title is still showing. The final Enter is only
  pressed when the Explore dialog shows Yes selected (Yes/No decided on the button strip only: 7.1 vs 20.0).
  Loading/in-game from the game state int `0x008654A4` (3 frontend, 4 loading started, 5 loading, 6 in game).
- Reference workspace from the user: `/mnt/m2drive/waah/games/NFSU2 Mod/` (Camera Toolkit fork + notes, CE table,
  two full-memory dumps, ZMenu, analysis scripts). Verified live from it: game state `0x8654A4`, frame time
  `0x865198` (1/60), time scale `0x7A5730` (1.0), player car `*(CarState**)0x89CCF8` → `+0x34` → `+0x20` = RigidBody
  (vtable `0x7A20C4`, tag `'Rigi'`, position at `+0x20`).

**Verified**
- `tests/m4_loop.sh 5` → 5/5, free roam 12.9–13.3 s after launch (14.1–14.7 s incl. process start); menus take
  ~2.3 s after the title accepts Enter (~6.6 s in). Final screenshot checked: free roam, not a race.

**Broke / learned**
- First "press faster" attempt (fixed 0.15–0.25 s settles + Enter retries on the last step) ended in a **circuit race**
  (reported by the user): screens counted as "changed" at the start of a fade, keys landed mid-transition, and blind
  Enter retries walked into Quick Race. Lesson: never retry Enter blindly in menus; confirm the screen first.
- The Toolkit notes say the keyboard is window messages, not DirectInput. For this game it is action-mapped
  DirectInput (EnumDevicesBySemantics, so no GUID_SysKeyboard reference) — see docs/structs/input.md.

## 2026-11-14 — input passthrough fix; boot steps tolerate input-ignore windows

**Broke**
- User: their real keyboard stopped working in the game. Cause: on `DIERR_NOTACQUIRED`/`INPUTLOST` the game calls
  `Acquire` and skips the frame; the probe turned those errors into success (to inject while unfocused), so the game
  never re-acquired and the physical keyboard stayed dead after refocusing.
- Two quick injected Enters merged into one (second `press` while the first was still held sent no new key-down).
- The Load screen, like the title, ignores Enter for a moment after it appears.

**Changed**
- `input.cpp`: on those errors the hook acquires the keyboard itself and retries the read, so real keys pass through
  when focused and injected keys are merged on top; unfocused, injected keys only. A `press` on a held key now sends
  release + press.
- `boot_freeroam.py`: one `step_enter(src, dst)` for every Enter: press, wait ≤1 s for `dst`; re-press only while the
  screen is still clearly `src` (never once a transition started).

**Verified**
- `tests/m4_loop.sh 5` → 5/5, free roam 12.1–13.4 s.

## 2026-11-14 — M5: telemetry, frame-locked scripts, record/replay

**Changed**
- Probe: `car_state` (`car.cpp`, validated pointer chain), frame scheduler (`schedule`, applied at Present),
  `record_start/stop` (every event the game reads from the keyboard — real and injected — as game actions, plus car
  checkpoints), `trace_start/stop`. `tools/drive.py` (state / script / record / replay), MCP tools `car_state`,
  `drive_script`, `record_inputs`, `replay_inputs`. `docs/structs/car.md`.
- In-game keyboard map (53 actions): 1 throttle W/Up, 2 brake S/Down, 3/4 steer A/D, 5 handbrake Space,
  6 nitrous Alt, 7 shift up LShift, 8 shift down LCtrl (manual gearbox in the save).

**Verified**
- Gear `vehicle+0x1E4` (1 N, 2 = 1st …) and RPM `engine+0x14` (rad/s) live; the notes' drivetrain RPM field did not
  hold. Real keyboard passthrough confirmed by the user.
- `tests/m5_drive.py` PASS: record a scripted straight-line drive (throttle 2 s, brake 2 s) in boot A, replay the
  recording in boot B (end error 0.014 m, worst 0.019 m over 12 checkpoints), rerun the key script in boot C (0.000 m).
  With frame-locked input the game is deterministic across reboots (same spawn every boot).

**Open**
- The test road is a 30 m straight; a longer route with steering/shifting/traffic is the real stress test.

## 2026-11-14 — no-traffic switch, determinism findings

**Changed**
- Probe `no_traffic` (MCP tool, `run_game.sh --no-traffic`): `0x42F910` (per-frame traffic update, `__thiscall`,
  1 arg, only caller `0x609ED7`) patched to `ret 4`, the same patch ZMenu's "No Traffic" makes (found by tracing
  its menu label to the handler at `0x10057DA0` in ZMenuU2.asi). Refuses if the original bytes aren't there.
- Trace samples can carry raw dwords of up to 8 watch addresses (`trace_start {"watch": [...]}`).

**Verified**
- Frame timing is fixed: frame time `0x865198` = 1/60, tick count `0x865140` = 30, mode `0x7FB700` = 2 on every
  frame while driving. The sim advances one fixed step per presented frame.
- Determinism: a scripted run and its recorded replay read identical event streams and end 0.000 m apart; two
  replays of the user's 60 s / ~1 km route (no traffic) are identical to the millimetre at all checkpoints.
- `WM_KEYDOWN` in MainWndProc only handles End/Home/Left/Right (cursor of a list/console at `0x8709B8`, only while
  `0x8709BC` is set) and F12 (`0x870994`); not driving input.

**Open**
- A *live* human drive drifts from its replay from the first input on (3 cm at +17 frames, ~1 m after 10 s, then
  a wall). The recording misses something the game takes from live input. Next: short live drive with a per-frame
  trace vs. its replay, find the first divergent frame and field.

## 2026-11-14 — probe hot reload (user request: play while the probe changes)

**Changed**
- Probe split: `nfsu2_probe.asi` = stable loader (`probe/loader/loader.cpp`: exe hash, log file + ring, D3D9
  Direct3DCreate9/CreateDevice/Present hooks, keep_active WndProc hook, core management); `nfsu2_core.dll` = core
  (`probe/src/`: server, input, car, schedule/record/trace, screenshots, no_traffic). Interface `probe/common/api.h`.
- Loader loads the core from a temp copy of `out/nfsu2_core.dll` (path via env `NFSU2_CORE`), polls its mtime twice a
  second at Present and reloads once it stopped changing; `reload` RPC / `tools/probe.py reload` / MCP `reload_core`.
  New core is loaded before the old one is shut down, so a broken build leaves the running core in place.
- Core shutdown: stop server thread (close listen socket, shut down client, join), fail queued tasks, save input state
  (devices, kinds, key→action maps), restore every patched vtable slot that still points at us, MH_Uninitialize.
  Core init after reload re-patches the saved devices. Exceptions in the core's per-frame code are caught (SEH) and
  disable the core until the next reload.
- Screenshots: `work/screenshots` capped at the newest 20 (user request).

**Verified** (one game session, no restart)
- `touch out/nfsu2_core.dll` → reload #1: keyboard restored with 53 mapped keys, injected W drove the car 5 m.
- Version bump + rebuild → auto reload #2 picked up `0.2.1`; `tools/probe.py reload` → #3.
- After 3 reloads: car_state, screenshot, schedule + trace, no_traffic, keep_active, log_tail all OK.

## 2026-11-14 — live-drive replay: exact until traffic; "no_traffic" doesn't work

**Verified**
- User drove ~17 s live (147 steering taps, 12 shifts, up to 217 km/h), recorded with a sample every frame. Replay
  after a reboot: identical event streams and identical car state on **every frame up to 1022** (~1 km). At 1022
  the replay hits a traffic car (speed 217 → 130 km/h in one frame); the live drive didn't meet it.
- So the game is deterministic given the same inputs; the remaining difference is AI (traffic, roaming racers)
  placement differing between boots.
- Real action-mapped events carry `dwOfs` = 4 × index into the action format (e.g. 4, 20); injected ones carry the
  DIK. The game's read loop doesn't use `dwOfs` outside text entry. Now recorded (`ofs`, `injected`).

**Broke**
- The ZMenu "No Traffic" patch (`0x42F910` → `ret 4`) does **not** remove traffic: the user saw traffic (somewhat
  less?) and roaming racers driving normally with it on. Marked experimental/not working; not used by default.
- CRT `rand` (`0x760D7D`) has only 6 callers (`0x7034B4`–`0x71917A`); the game's own RNG is elsewhere (open).

**Open**
- Make AI repeatable (find EA's RNG + seed, and/or start recordings at a fixed frame after loading) or find the real
  traffic spawner for a true no-traffic switch.

## 2026-11-14 — AI cars mapped from a live traffic car

With the user parked next to a traffic car: scanned memory for RigidBodies (vtable `0x7A20C4` + tag `'Rigi'`,
6 found, the traffic car 2.9 m away), walked back to its MellowMover (`'Melo'`) and PsychoDriver, found the AI roster
of 14 controllers at `0x89CE58` (player, 3 RaceCarControllers, 10 TrafficControllers), and named every class through
its vtable slot-2 name method. Details: `docs/structs/traffic.md`.

## 2026-11-14 — M6: rewrite DLL, hook table, hot reload

**Changed**
- `game/` → `out/nfsu2_game.dll` (the rewrite). Replacements register with `NFSU2_REWRITE(addr, fn, signature, orig)`
  (`game/src/rewrite.h`); exported table `nfsu2_game_api()` (`probe/common/game_api.h`).
- Core `rewrite.cpp`: loads the game DLL from a temp copy (path `NFSU2_GAME`, else next to `NFSU2_CORE`), MinHook per
  entry (created disabled), compares the target's first 8 bytes with the exe on disk (flags functions another mod
  already patched), switches at the render thread, hot-reloads when the file changes (new DLL loaded before the old
  one is dropped; enabled set kept), hands the enabled set to the next core on a core reload.
- Tools: `tools/probe.py rewrites | enable | disable | reload-game`, MCP `rewrite_list/enable/disable/reload`.
- First entry: `UpdateFrameTime` 0x57EAD0 (`void __cdecl(float real_dt)`), passthrough + scale.

**Verified** (running game, no restart; the game had been started before the game DLL existed — the core reloaded
itself and picked it up)
- Hooked, bytes match disk (no other mod). Off: 0x865198 = 0.016667. On (scale 1.0): 0.016667. Edited to 0.5 +
  rebuilt → game DLL hot-reloaded (#1), still enabled, 0x865198 = 0.008333. Reverted + rebuilt → #2, 0.016667; off.

**Broke / learned**
- No visible slow motion while 0x865198 was halved (user was driving): it is not the simulation step. M6's
  "see the new behaviour" is met by measurement only; a visible demo needs a function with a visible effect.
- My throttle comparison ran while the user was driving (car at 218 km/h) — invalid, and it drove the user's car.
  Don't inject driving into a session the user is playing without asking.

## 2026-11-14 — M7: diff harness; first verified rewrites (EA bString)

**Changed**
- `game/src/diff.h`: seeded xorshift inputs (empty strings, letters, punct, bytes >= 0x80), `run_diff` (original via
  MinHook trampoline vs rewrite), `NFSU2_DIFF_TEST`. Game API v2 exports `diff_list` / `diff(name, n, seed)`; core RPC
  `diff_test`, MCP `diff_test`, `tests/m7_diff.py`.
- `game/src/bstring/bstring.cpp`: bStringHash 0x43DB50, bStringHash2 0x43DB80, bStrLen 0x43DBB0, bStrCpy 0x43DBD0
  (compares output buffers), bStrCmp 0x43DC30, bStrICmp 0x43DCC0 — written from the disassembly, keeping the game's
  quirks (signed-char compares, `&0x5F` uppercasing). Plus `bStringHash_BROKEN` (signed bytes) as a harness self-check.

**Verified** (live game, user playing, hot-reloaded — no restart)
- `tests/m7_diff.py`: 6 rewrites × 3 seeds × 10,000 cases, 0 mismatches; the broken hash is caught (~8,000/10,000,
  with reproducible inputs). 70,000 cases run in 0.05 s on the render thread.
- All six enabled in the running game (1,300+ call sites now run our code): 601 frames in 10 s, no errors.
- Interface version guard: the old core refused the v2 game DLL during the reload; the new core loaded it.

**Coverage:** 6 functions verified (+1 rewritten passthrough) of ~23,278 (0.03 %).

## 2026-11-14 — M8 start: rest of the bString block; enabled set persists

**Changed**
- Enabled rewrites persist across game restarts: `out/rewrites_enabled.json` (written on every switch, read on a
  fresh start; core reloads still hand the set over directly).
- `game/src/bstring/bstring2.cpp`: bStrNCpy, bStrNCmp, bStrNICmp, bStrCat (3-arg), bStrChr, bStrUpr, bStrToInt,
  bWStrLen, bStrToWStr, bStrNToWStr. Diff inputs place strings at random offsets in arenas with random leading bytes,
  because the bounded compares end by reading `a[-1]` / `b[-1]`.

**Verified** (user driving the whole time; nothing injected)
- `tests/m7_diff.py`: 16 rewrites × 3 seeds × 10,000 cases, 0 mismatches; broken hash still caught.
- All 16 enabled in the running game: 601 frames / 10 s, no errors.

**Learned**
- bStrToFloat (0x43DEF0) is x87 code. Our compiler uses SSE by default and D3D9 normally drops the x87 FPU to 24-bit
  precision, so float rewrites need a deliberate exact-x87 strategy before they can be bit-identical. Next topic.

**Coverage:** 16 verified of ~23,278.

## 2026-11-14 — FPU measured; first bit-exact float rewrites (bMath)

**Measured:** game-thread x87 control word `0x007F` (24-bit precision, round-nearest, masked), MXCSR `0x1FA0`
(new core RPC `fpu_state`). Consequence (now in CLAUDE.md): x87 `+ - * / fsqrt` round like SSE single precision.

**Changed:** `game/src/bmath/bmath.cpp`: bRandom (the game RNG, seed `0x7EA624`), bIdentity, bDistBetween,
bNormalize2, bNormalize, bScaleAdd2, bScaleAdd, bCross, bInitBounds2 — operation order copied from the x87 code.
Plus `bDistBetween_BROKEN` (same formula, summation reordered) as a float self-check. bRandom's diff test saves and
restores the game's seed, so testing doesn't change the session's randomness.

**Verified** (user driving): 9 × 3 seeds × 20,000 cases, bit-exact (raw float bits), inputs 0 / ±1e-3..±1e5 incl.
-0, zero vectors, aliased outputs. The reordered variant differs in 793/20,000 cases (1 ulp) — caught. All 25
rewrites enabled in game: 601 frames / 10 s, no errors.

**Note:** inputs beyond float range in intermediates (x87 keeps a wider exponent) would differ; not seen in game
magnitudes, not tested.

**Coverage:** 25 verified of ~23,278.

## 2026-11-14 — collision bug from the vector rewrites: the register contract

**Broke**
- User: "handling changed… car hitting the ground… more floaty on a jump", then "driving feels fine but collision
  was messed up", ~1 min after the 9 bMath rewrites were enabled. Disabled them (17:12). Re-enabling the vector
  ones in a passthrough mode (to measure the FPU) **crashed the game**: page fault at `0x5963BC` reading
  `0x63F6F327` right after a call to bScaleAdd.
- Cause: the caller does `fld [eax+4]` after `call bScaleAdd` — the original leaves its `out` pointer in EAX and
  the game (MSVC whole-program optimisation) relies on it. My rewrites were `void`. Also: a caller keeps ECX live
  across bSin (the original never writes ECX), and several vector originals never write EDX.
- The FPU-precision theory was wrong: 3.24 M live calls during driving, all at 24-bit precision, round-nearest.

**Changed**
- `tools/re/regcontract.py` (which of EAX/ECX/EDX callers read after a call; zeroing idioms excluded) and
  `tools/re/regwrites.py` (which registers an original writes; unwritten ones are effectively preserved).
- `game/src/regs.h`: every rewrite is installed through a naked entry stub that saves/restores ECX and EDX
  (`NFSU2_REWRITE_N(addr, fn, nargs, ...)`); vector functions return their out pointer explicitly. `call_regs()`
  calls with sentinel EAX/ECX/EDX; `check_regs()` requires: EAX equal (non-float), registers the original preserves
  preserved, st0 bit-equal for float returns. Per-file register tests `regs_bstring/regs_bstring2/regs_bmath/
  regs_btrig`, and `regs_BROKEN` (bScaleAdd as `void`) which is caught 1000/1000.
- Fixed the persisted enabled list before restarting so the broken versions didn't come back.

**Verified**
- All value tests + all register tests pass for 30 rewrites (2 seeds); self-checks caught.
- Vector rewrites re-enabled with real maths at 17:21:49 while the user drives: 601 frames / 10 s, no exceptions.
  Waiting on the user's feel for collisions (races / roaming racers encouraged — more code paths).

**Rule (in regs.h):** a rewrite must leave EAX/ECX/EDX as the original does; diff tests check registers, not just
return values.

## 2026-11-14 — all verified rewrites live; bMath batch 2

- User confirmed collisions normal with the fixed vector rewrites. Enabled the rest (bRandom, bIdentity,
  bInitBounds2, bSin, bCos, bSinRad, bRandomFloat at 17:24; bStrUpr, bWStrLen with stubs).
- `game/src/bmath/bmath2.cpp`: bNormalizeTo (zero → (len,0,0)), bNormalizeTo2 (zero → (1,0)), bEqualEps (NaN counts
  as equal), bInBox2. 3 seeds × 20,000 value cases + register tests, 0 mismatches; enabled in game, 60 fps.

**Coverage:** 33 verified of ~23,278, all running in the user's game.

## 2026-11-14 — bMath batch 3: matrix * point, seeded random, modulo

- User: "everything feels normal so far" with all 33 live.
- `game/src/bmath/bmatrix.cpp`: bMulMatrix (0x441000, 122 callers via the thunk 0x43C9C0), bRandomSeeded, bMod
  (keeps the negative-multiple quirk), bFMod (copies the `_ftol2` truncation + `f > t` correction). 3 seeds × 20,000
  value cases + register tests, 0 mismatches; enabled in game.
- `regcontract.py` false positive noted: `push ecx` used to reserve a stack slot (then `fstp [esp]`) reads ECX
  harmlessly.

**Coverage:** 37 verified of ~23,278, all live.

## 2026-11-14 — bSinCos, bTransposeMatrix; EAX comparison made explicit

- bSinCos 0x43C390 (all 65,536 angles bit-exact) and bTransposeMatrix 0x43C9D0 (in-place, SNaN quieting of the two
  elements copied through x87). Live since 17:34:14, 39 rewrites.
- `check_regs(..., compare_eax)`: EAX is compared by default; `false` only where the function's EAX is garbage and
  `regcontract.py` shows no caller reads it (bSinCos leaves the x87 status word there).

**Coverage:** 39 verified of ~23,278, all live.

## 2026-11-14 — inverse trig: exact down to leftover register bits

- bASin 0x43C490 and bATan2 0x43C570 return angles built with 16-bit moves, so EAX's upper half is leftover: from
  `_ftol2`'s result (bASin) or from the caller's own EAX (bATan2's equal case, where only `fnstsw ax` wrote EAX).
  Callers use the full EAX in places (`sub ecx, eax`, `mov ebx, eax`), so the leftovers are reproduced.
- New stub `NFSU2_REWRITE_N_EAX` passes the caller's entry EAX as a hidden last argument.
- Tests compare full EAX/ECX/EDX through `call_regs` (sentinel EAX), 3 seeds × 20,000, 0 mismatches. Live since
  the timestamp in the commit; 41 rewrites.

**Coverage:** 41 verified of ~23,278, all live.

## 2026-11-14 — x87 lifter; NaN contract; bInvertMatrix

- User turned all rewrites off (17:39:36) to compare, saw no difference, all 41 back on at 17:40:56.
- `tools/re/x87lift.py`: lifts straight-line x87 code to C++ — symbolic x87 stack + integer registers, stack frame
  relative to entry esp, args/pointers, constants read from .rdata — emitting the same loads, single-precision ops
  and stores in the original order (`game/src/lift.h` helpers). Fixed on the way: one-operand pop forms
  (`fsubp st(i)` = st(i) op st0, pop).
- First lifted function: bInvertMatrix 0x43C7C0 (~130 x87 instructions).
- Learned: LLVM may fold `-(a*b)` into `a*(-b)`, flipping a NaN's sign → `lift::fchs`/`fabs_` use an asm barrier.
  Remaining differences were only NaN payload/sign in singular matrices: x87 and SSE pick different NaN operands.
  **Contract adopted (CLAUDE.md):** bit-exact for non-NaN; NaN where the original gives NaN (sign/payload free).
  `game::same_float`.
- bInvertMatrix: 3 seeds × 20,000 (incl. near-singular, singular, in place) + register test pass; live.

**Coverage:** 42 verified of ~23,278, all live.

## 2026-11-14 — M8: file/chunk loading starts with JDLZ

- Note: I confused the user's session (closed with Alt+F4 before a usage-limit break) with a new launch, and read
  its exit as a crash. Real issue found on the way: **Alt+F4 exit ends in a fail-fast (c0000409) inside CRT cleanup
  of one of our DLLs** (offset 0xDBA8; symbolized candidates: vector emplace in game DLL / json dealloc in core).
  Harmless for play; to fix (clean shutdown of probe/core/game DLLs at process exit). Also: a game launched from
  the background terminal starts minimized on Wayland (fullscreen without focus) — the user can't see it.
- Data reality check: `GlobalB.lzc`/`FrontB.lzc` in this install contain no JDLZ; 86 files do (car/global textures).
- `game/src/bchunk/jdlz.cpp`: JdlzDecompress 0x573700 (function-pointer target, no direct callers). Diff test
  decompresses all real blocks (861) from three texture packs with both versions into guarded buffers: identical.
  Register test incl. bad header. Live.

**Coverage:** 43 verified of ~23,278, all live.

## 2026-11-14 — chunk dispatch, progress tracker

- `game/src/bchunk/chunks.cpp`: FindChunkLoader 0x43C100 (exhaustive over all registered IDs + random), LoadChunk
  0x570D70, LoadChunks 0x577AB0 (they call real loaders, so verified by exact transcription, three full boots with
  them on, and the M5 drive test). New `game_fn<F>(addr)`: rewrites call other game functions through their game
  address, so every rewrite's switch stays independent.
- **M5 test finding:** with ALL rewrites off the replay (B) still ends ~0.3 m from the recording (A) while the rerun
  script (C) matches exactly: `record_start` and `schedule` are separate RPCs, so A's inputs can land one frame
  off. Same pattern with rewrites on → not caused by rewrites. To fix: start recording and schedule atomically.
- User's GitHub repo; `tools/progress.py` generates `docs/progress.svg` (card in README) and `PROGRESS.md`. SSH push
  needs the user's key authorised (github.com host key added after checking the published fingerprint).

**Coverage:** 46 verified of ~23,278, all live.

## 2026-11-14 — Reorganization, ./nfsu2, in-game panel

- **No cops in NFSU2**: removed from the tracker and the M8 plan.
- **Layout**: `probe/core`, `game/framework` + `game/systems/<system>`, `tools/{game,probe,re}`, `tests/milestones`
  (git mv, all paths/imports updated, clean rebuild, all scripts compile, 177/177 diff runs pass). `./nfsu2` is the
  single entry point (run/freeroam/kill/status/build/rewrites/on/off/all-on/all-off/test/progress/push).
  `docs/GUIDE.md` explains the project for the user. `.idea/` untracked.
- **In-game panel (F8)**: Dear ImGui v1.93.0 (vendored core + dx9/win32 backends) in the core, so it hot-reloads.
  Loader 0.3.0 / API v2 adds `set_msg_hook` (called first in the WndProc hook; overlay swallows mouse/keys it
  captures) and `set_reset_hook` (IDirect3DDevice9::Reset, slot 16, so ImGui drops/recreates its D3D objects).
  GameApi v3 adds `GameRewrite.file` → rewrites grouped by system. Tabs: Rewrites (toggles, all on/off, per-function
  and all diff tests), Car, Probe, Log. Verified: log "overlay: ready (ImGui 1.93.0, scale 1.53)"; user confirmed
  panel, toggles and car tab work in free roam.
- **Fixed**: after an API-version mismatch the loader/core retried loading the same file every 0.5 s (log spam);
  now it waits for the next rebuild.
- Known: the game still reads DirectInput while the panel is open (typing in the filter box also reaches the game).

## 2026-11-14 — Chunk unloading + realignment; call counters

- **New verified rewrites** (`game/systems/bchunk/`): `UnloadChunks` 0x577B10, `CopyChunksAligned` 0x570DD0,
  `ClearChunks` 0x570F00. UnloadChunks reuses EDI: after each group of up to 64 the earliest chunk unloaded becomes
  the new end, so a bundle is unloaded in exact reverse order.
- **Diff tests**: random chunk trees at random addresses mod 128 (copy/clear: 10k cases, 0 mismatches, byte-exact
  dst + return); LoadChunks/UnloadChunks now have a real diff test too: a fake bChunkLoader is spliced into its
  registry bucket for one case and records the call order (bundles of up to 300 chunks), plus the debug-name ring.
  Mutation check: padding boundary 8→7 → 1384 mismatches, forward unload order → 9086 mismatches.
- **Entry stubs** support 5 and 6 args; **call counters**: every stub does `lock inc` on a per-rewrite counter
  (GameApi v4 `GameRewrite.calls`), shown in `./nfsu2 rewrites` and the F8 panel.
- **Live**: all 49 on, 3 boots to free roam (12–13 s, no errors). Per boot: UnloadChunks 6, CopyChunksAligned 404,
  LoadChunks 73, LoadChunk ~1790, JdlzDecompress ~121 calls. ClearChunks not reached during boot (realign path only).

## 2026-11-14 — Memory allocator (bMalloc/bFree + bMemoryPool)

- **New system `game/systems/memory/`**: bMalloc 0x440390, bMalloc2 0x440820, bFree 0x440540, bMemoryPool::Init /
  AddFreeMemory / Allocate / Destroy, bInitMemoryPool, bCloseMemoryPool (9 functions). Layout in pool.cpp and
  symbols.csv. New `NFSU2_REWRITE_THIS` entry stub (thiscall: `this` in ECX, callee pops) + `check_regs_this`
  (also checks how many bytes the callee popped).
- **Diff tests**: two private pools on two arenas with the same alignment run identical random operation sequences
  (original vs ours); after every op the whole pool state (free/alloc/OS-block lists, stats) is compared with
  pointers normalised to region+offset, and the memory at the end (pointer-aware). bMalloc/bFree run through pool
  slot 7 (unused by the game). Test pitfalls found and handled: OS blocks come from the CRT heap at unrelated
  addresses (only 8-aligned, and in arbitrary address order vs the arena, which matters because the free list is
  sorted by address) → growing cases use alignment 4/8 and stop comparing when the region order differs.
  Mutation checks: min-waste 16→8 → 1173 mismatches; bFree not clearing the magic → 4958.
- **Incident**: a test of mine read a freed header after its OS block had gone back to the heap → access violation
  → the probe core disabled itself (user: F8 stopped working, game froze during tests). Fixes: diff tests now run
  under SEH (a crash = failed test with address, core survives; `crash_BROKEN` self-check), the suite calls one
  test per request (game pauses briefly instead of 30 s), slot 7 reclaimed after a crashed test. Crash addresses
  are symbolized with `llvm-symbolizer --obj=out/nfsu2_game.dll --pdb=out/nfsu2_game.pdb --relative-address`.
- **Hot reload safety**: the entry stubs count in-flight calls (`nfsu2_inflight`, GameApi v5); the core disables
  the hooks and waits for 0 before removing hooks and unloading (other threads call the allocator; one can be
  blocked on a pool lock inside our code). If it doesn't drain in 2 s the old DLL is kept loaded.
- **Live**: enabled step by step in the user's session (pool internals, then bMalloc/bFree), then 3 boots to free
  roam with all 58 on: 13 s each, no errors; per boot ~3100 bMalloc, ~2700 bMalloc2, ~2000 bFree, and our
  PoolInit/bInitMemoryPool created the game's pools. Suite: 207/207.

## 2026-11-14 — Resource files + decompression front end

- **New system `game/systems/resource/`** (12 verified): IsCompressed, Decompress (dispatch), RawwDecompress,
  ResourceFile::SetAllocParams / AllocateBuffer / SetOwnsBuffer / FreeBuffer / Load / Unload, DecPending,
  LoadChunksAligned, FindResourceFile. ResourceFile layout in resource_file.cpp / symbols.csv.
- **Finding**: LoadChunksAligned's re-align branch is dead code: it is only taken if CopyChunksAligned in mode 1
  returns a moved offset, and mode 1 never moves it. Explains ClearChunks never being called. Transcribed anyway.
- **Test infra** (shared headers): `bchunk/fake_loader.h` (fake leaf + container loader recording load/unload
  calls), `memory/test_heap.h` (private bMemoryPool in slot 7, offsets for pointer comparison, shared slot-7
  ownership). Tests run Load/Unload/LoadChunksAligned on random objects with fake bundles (some RAWW-packed) in two
  identical heaps and compare object, loader calls, heap stats and buffer bytes. 10k each, 0 mismatches; mutation
  checks (Load flag 4→2: 3736, Unload ignoring ownership: 3125). FindResourceFile against all 25 live files.
- **Live**: all 70 on; the user entered a shop, browsed, left: "all normal". During it: Unload 23,
  LoadChunksAligned 74, DecPending 23, SetAllocParams 13, IsCompressed 2, ~3000 bMalloc/bFree; no errors.
- Next: the constructor needs the file layer (0x57CA10 open/size, async reads) → file system is the next system.

## 2026-11-14 — File layer (bFile, open-file cache) + a crash

- **New system `game/systems/file/`** (10 verified): bFileNameHash, FindFileIndexEntry, FindOpenFile, AddOpenFile,
  OpenFileEntry::Close, FlushOpenFileCache, bFile::bFile, bFile::Close, bOpen, bFileExists. They sit on EA's async
  file library at 0x6F7xxx (called by address; not rewritten yet). Read-mode opens only stat the file, so the
  open-file cache stays empty in normal play (handles are only cached on the CD-drive fallback path).
- **Tests never write files and never close real handles** (the profile is symlinked to the user's game folder):
  real names in read mode (stat), missing names, fake cache entries and a fake index node spliced into the game's
  lists. Mutation checks: FindOpenFile not moving the hit (3255), ctor not taking a reference (61).
- **Original bug found**: bFileNameHash copies into a 0xA0-byte stack buffer without a bound; a test name of 190+
  chars smashed the original's stack (EIP 0x41414141). SEH caught it - but the crashed test's fixtures stayed
  spliced: the archive-index head pointed into freed memory until repaired by hand. Fix: `game::ScopedUndo` undo
  stack; the harness runs pending undos after a crashed test (crash_BROKEN checks "1 fixture(s) undone").
- **Game crash 22:36** (user was playing): write fault in our PoolAllocate's debug fill on the main thread, i.e. a
  game pool's free list was already damaged. Root cause NOT proven; prime suspect: the dangling index node above
  (live for ~10 min before the repair). Added `heap_integrity` (walks every real pool: magic, links, ordering,
  header counts, free total; `heap_integrity_BROKEN` proves it detects a planted bad magic). After a restart:
  3 cycles of DLL reload + full suite with heap checks before/after: all clean. Keep running heap_integrity; if a
  pool is ever damaged again this pins down when.
- **Live**: 80 on; the user did a shop and a race (the save changed: start position differs from M5's recording).
  bOpen/ctor 1342, Close 489, bFileExists 38, FlushOpenFileCache 11 (the save was written through our code);
  heap clean, no errors. Suite 261/261.
