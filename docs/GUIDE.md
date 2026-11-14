# Guide: how this project fits together

The short version: the original game runs as normal, a small helper inside it lets us look at and control it, and
our own C++ replaces the game's code one function at a time — only after it's proven to behave identically.

## The three pieces that run inside the game

```
SPEED2.EXE  (the original game, under Wine)
 └─ probe loader   probe/loader/  → out/nfsu2_probe.asi   never changes while the game runs
     └─ probe core probe/core/    → out/nfsu2_core.dll    reloads by itself when rebuilt
         └─ rewrite game/         → out/nfsu2_game.dll    reloads by itself when rebuilt
```

| Piece | What it does |
|---|---|
| **Probe loader** | Loaded by the ASI loader with your other mods. Keeps the game running when it's not focused, owns the log, and (re)loads the core. |
| **Probe core** | The toolbox: reads/writes memory, injects keys, reads car telemetry, runs scripted drives, takes screenshots, and switches rewritten functions on and off. Claude talks to it through `tools/probe/`. |
| **Rewrite** | Our own code: every function we've rewritten, each with a test that compares it with the original. |

Because the core and the rewrite reload themselves, you can keep driving while they change.

## How a function gets rewritten

1. **Find it** — read the game's machine code (`tools/re/`), name it in `docs/symbols.csv`.
2. **Rewrite it** — C++ in `game/systems/<system>/`. Floating-point code can be converted automatically
   (`tools/re/x87lift.py`).
3. **Prove it** — a diff test runs the original and ours side by side inside the game on thousands of random and real
   inputs. Results must match exactly, and so must the CPU registers the game's optimised code relies on.
4. **Switch it on** — in your running game. If anything feels off, it can be switched back instantly.
5. **Count it** — marked *verified* in `docs/symbols.csv`; the tracker (`PROGRESS.md`) updates from that.

## Folders

| Folder | What's inside |
|---|---|
| `probe/loader/` | the stable loader (D3D hooks, focus fix, log, loading the core) |
| `probe/core/` | the toolbox (server, input, car telemetry, scripted driving, screenshots, rewrite switches) |
| `probe/common/` | the small interfaces between loader, core and rewrite |
| `game/framework/` | how rewrites plug in: registration, register-safe entry stubs, the diff harness |
| `game/systems/` | the rewritten game code, one folder per system (`bstring`, `bmath`, `bchunk`, …) |
| `tools/game/` | starting the game and booting into free roam |
| `tools/probe/` | talking to the probe (command line, Claude's MCP bridge, driving scripts) |
| `tools/re/` | reverse-engineering helpers (cross-references, register checks, the x87 lifter) |
| `tests/milestones/` | the checks that proved each milestone |
| `docs/` | this guide, the day-by-day `DEVLOG.md`, `symbols.csv` (every named function), `structs/` (game data layouts) |

## Everyday commands

Everything goes through one script in the project folder:

```
./nfsu2 freeroam        start the game straight into free roam
./nfsu2 status          is it running, how many rewrites are on
./nfsu2 all-off         everything back to the original code (compare!)
./nfsu2 all-on          every verified rewrite back on
./nfsu2 rewrites        list them
./nfsu2 test            run every diff test in the running game
./nfsu2 kill            stop the game
```

Run `./nfsu2` alone for the full list.

## Words used in the logs

- **Verified** — rewritten and proven identical; safe to have on.
- **Diff test** — the side-by-side comparison of original and rewrite.
- **Register contract** — the game's compiled code sometimes expects a function to leave certain CPU registers
  untouched or holding a pointer; our versions honour that.
- **Hot reload** — rebuilding swaps the new code into the running game.
