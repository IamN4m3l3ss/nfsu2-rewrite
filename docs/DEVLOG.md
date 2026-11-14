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
