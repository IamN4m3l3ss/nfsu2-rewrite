# nfsu2-rewrite

Rebuilding **Need for Speed: Underground 2** (PC, 2004, NA v1.2) as our own C++ code, one function at a time,
while the original game keeps running.

![progress](docs/progress.svg?v=693-2026-11-15)

**→ Full lists of what's done and what isn't: [PROGRESS.md](PROGRESS.md)**

The game files are not part of this repository; you need your own copy of NFSU2 (NA v1.2).

## How it works

The original game runs as normal. A small helper inside it lets us look at and control it, and our own C++ replaces
the game's code one function at a time — only after it's proven to behave identically.

```
SPEED2.EXE  (the original game, under Wine)
 └─ probe loader   probe/loader/  → out/nfsu2_probe.asi   never changes while the game runs
     └─ probe core probe/core/    → out/nfsu2_core.dll    reloads by itself when rebuilt
         └─ rewrite game/         → out/nfsu2_game.dll    reloads by itself when rebuilt
```

| Piece | What it does |
|---|---|
| **Probe loader** | Loaded by the ASI loader with the other mods. Keeps the game running when it's not focused, owns the log, and (re)loads the core. |
| **Probe core** | The toolbox: reads/writes memory, injects keys, reads car telemetry, runs scripted drives, takes screenshots, switches rewritten functions on and off, and draws the in-game panel. |
| **Rewrite** | Our own code: every function we've rewritten, each with a test that compares it with the original. |

Because the core and the rewrite reload themselves, you can keep driving while they change.

### How a function gets rewritten

1. **Find it** — look it up (EA's name and the decompiled code, see Credits) and read the game's machine code
   (`tools/re/`); record it in `docs/symbols.csv`.
2. **Rewrite it** — our own C++ in `game/systems/<system>/`.
3. **Prove it** — a diff test runs the original and ours side by side inside the game on thousands of random and
   real inputs. Results must match exactly (floats bit for bit), and so must the CPU registers the game's
   optimised code relies on.
4. **Switch it on** — in the running game. If anything feels off, it can be switched back instantly.
5. **Count it** — marked *verified* in `docs/symbols.csv`; the tracker updates from that.

## Everyday commands

Everything goes through one script in the project folder (`./nfsu2` alone lists them all):

```
./nfsu2 freeroam        start the game straight into free roam
./nfsu2 status          is it running, how many rewrites are on
./nfsu2 all-off         everything back to the original code (compare!)
./nfsu2 all-on          every verified rewrite back on
./nfsu2 test            run every diff test in the running game
./nfsu2 kill            stop the game
./nfsu2 push            update the progress tracker and publish it
```

In the game, **F8** opens the panel: switch each rewritten function on/off and test it, live car telemetry, probe
info and the log.

## Folders

| Folder | What's inside |
|---|---|
| `probe/` | the helper inside the game: `loader/`, `core/`, and `common/` (their interfaces) |
| `game/framework/` | how rewrites plug in: registration, register-safe entry stubs, the diff harness |
| `game/systems/` | the rewritten game code, one folder per system (`bstring`, `bmath`, `memory`, `file`, …) |
| `tools/` | `game/` start the game and boot to free roam · `probe/` talk to the probe · `re/` reverse-engineering helpers |
| `tests/` | the checks that proved each milestone, the ABI test, recorded drives |
| `docs/` | `DEVLOG.md` (day by day), `symbols.csv` (every named function), `structs.md` (game data layouts) |

## Words used here

- **Verified** — rewritten and proven identical; safe to have on.
- **Diff test** — the side-by-side comparison of original and rewrite.
- **Register contract** — the game's compiled code sometimes expects a function to leave certain CPU registers
  untouched or holding a pointer; our versions honour that.
- **Hot reload** — rebuilding swaps the new code into the running game.

## Credits

- **[nfsu2-gcpc](https://github.com/octavioCuatrochio/nfsu2-gcpc)** (CC0) — its knowledge base matching the
  GameCube build's symbols to `SPEED2.EXE`, and its Ghidra tooling, give us EA's function names and readable
  decompilations to work from. Some of our rewrites are adapted from its code (credited in each file), and its
  menu-engine/camera rewrites are included as-is in `third_party/nfsu2-gcpc/` (licence there).
  Every function only counts once our own diff test proves it identical in the game.
- Libraries in `third_party/`: [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause),
  [nlohmann/json](https://github.com/nlohmann/json) (MIT), [Dear ImGui](https://github.com/ocornut/imgui) (MIT).
