# Game data layouts (NA v1.2)

Measured live with the probe. Addresses are for SPEED2.EXE NA v1.2.

## Player car (telemetry)

Every field below was verified live on 2026-11-14 with the probe (`probe/core/car.cpp`).

```
CarState* player = *(CarState**)0x0089CCF8          // render-side mirror (don't write it)
assembly = *(void**)(player + 0x34)                 // owns the physics parts ('Phys')
  +0x04  vehicle      vtable 0x00784FC8
           +0x1E4 int gear: 0 reverse, 1 neutral, 2 = 1st, 3 = 2nd, ...   (verified: Left Shift steps 1->2->3->4)
  +0x20  RigidBody    vtable 0x007A20C4, tag 'Rigi' at +0x0C
           +0x20 position (3 floats)
           +0x30 / +0x40 / +0x50 forward / up / side rows (4 floats each)
           +0x70 linear velocity, +0x80 angular velocity
  +0x48  Engine
           +0x14 float engine speed, rad/s (idle 83.8 = 800 rpm; limiter ~934 = ~8,900 rpm)
  +0x4C  Drivetrain   vtable 0x007A2508, tag 'Drvt'
           +0x38/+0x3C int mirrors of the gear
           +0x44 int: 1 at rest, 3 while driving (clutch/drive state?)
           +0x4C float: driveline speed rad/s (0 in neutral; follows engine when engaged)
```

The notes' `drivetrain+0x58` (negated RPM) did not track the engine here; `engine+0x14` does.

Globals: game state `0x008654A4` (3 frontend, 4/5 loading, 6 in game), a frame time `0x00865198` (1/60 at the
WidescreenFix 60 fps limit; written by UpdateFrameTime 0x57EAD0 — halving it did not slow the game, so it is not the
sim step), time scale `0x007A5730` (1.0).

Spawn after `run_game.sh --freeroam` (Nameless save): (-1258.42, -618.66, 19.54), facing (0.776, -0.629, -0.042),
gear 2. Same every boot.

## Input (DirectInput 8)

Measured 2026-11-13 with the probe (`probe/core/input.cpp`) on NA v1.2 + the user's mod set.

### Devices

| Device | Created via | Data format | Cooperative level | Read |
|---|---|---|---|---|
| Mouse | `IDirectInput8A::CreateDevice(GUID_SysMouse)` | `SetDataFormat`, 20-byte state, 11 objects | FOREGROUND, NONEXCLUSIVE | `GetDeviceState(20)` every frame |
| Keyboard | DirectInput 8 **action mapping** (not `CreateDevice`; likely `EnumDevicesBySemantics`) | `BuildActionMap` + `SetActionMap`, genre `0x01000000`, map name `"NFS Underground 2"`, buffer 16 | — | `Poll` then buffered `GetDeviceData(20-byte records, 16)` every frame |

`g_pKeyboardDevice` = `*(IDirectInputDevice8A**)0x0086E5C0`.

### Keyboard read: `0x005C1840` (`__thiscall`, `this` = game input-device object)

- Only runs when `this->devType (+0x234) == 0x13` (DI8DEVTYPE_KEYBOARD).
- `Poll` (slot 25), then `GetDeviceData(0x14, buf, &n=16, 0)` (slot 10).
- On `DIERR_NOTACQUIRED` (0x8007000C) or `DIERR_INPUTLOST` (0x8007001E): `Acquire` (slot 7) and return.
- If `g_TextEntryMode` (byte `0x00870768`) is set: a separate path uses the events' `dwOfs` (typing).
- Otherwise, for each event: **only `dwData` and `uAppData` are used**. `mask = _allshl(1, uAppData)`;
  pressed (`dwData != 0`) → `this->heldActions (+0x08, u64) |= mask`, released → `&= ~mask`.
  `uAppData` 3 or 4 additionally sets byte `0x00870785 = 1`, `0x00870784 = 0`.

So injected keyboard events must carry the game's action id in `uAppData`; `dwOfs` alone does nothing outside text
entry.

### Frontend action map (32 actions, from `SetActionMap`)

All `dwHow = DIAH_DEFAULT (0x20)`, semantic = `0x81000400 | DIK`.

| uAppData | Key (DIK) | | uAppData | Key (DIK) |
|---|---|---|---|---|
| 27 | Up (0xC8) | | 44 | Home (0xC7) |
| 28 | Down (0xD0) | | 45 | End (0xCF) |
| 29 | Left (0xCB) | | 46 | P (0x19) |
| 30 | Right (0xCD) | | 47 | Q (0x10) |
| 31 | Enter (0x1C) | | 48 | R (0x13) |
| 32 | Numpad Enter (0x9C) | | 49 | I (0x17) |
| 33 | H (0x23) | | 50 | K (0x25) |
| 34 | Esc (0x01) | | 51 | J (0x24) |
| 35 | C (0x2E) | | 52 | L (0x26) |
| 36 | , (0x33) | | 53 | U (0x16) |
| 37 | . (0x34) | | 54 | Y (0x15) |
| 38 | [ (0x1A) | | 55 | Z (0x2C) |
| 39 | ] (0x1B) | | 56 | ; (0x27) |
| | | | 57 | ' (0x28) |
| | | | 58 | T (0x14) |
| | | | 59 | 6 (0x07) |
| | | | 60 | G (0x22) |
| | | | 61 | O (0x18) |
| | | | 62 | M (0x32) |

`BuildActionMap` is also called with 53-, 19- and 9-action formats (probably in-game driving maps); only the 32-action
map is set at the frontend. The probe re-reads the map on every `SetActionMap`.

### Timing

The title screen ignores Enter for a few seconds after it appears (ignored ~4 s after the first frame, accepted on the
2nd try ~6 s after the title appeared, deterministic over 4 runs).

## AI cars: traffic and roaming racers

Mapped live on 2026-11-14 with the user parked next to a traffic car in free roam (Bayview, Nameless save).
Class names come from each class's own name method at **vtable slot 2** (no MSVC RTTI on these classes).

### Roster (globals)

`0x0089CE58 .. 0x0089CE8C`: 14 controller pointers, one per car slot (counts at `0x89CE40..0x89CE4C` read 4,4,4,4):

| Slot | Global | Class (vtable) |
|---|---|---|
| 0 | `0x89CE58` | RealController / TrackedController (`0x785230`) — the player |
| 1–3 | `0x89CE5C`–`0x89CE64` | RaceCarController (`0x785370`, derives from TrafficController) — roaming racers (orange map arrows) |
| 4–13 | `0x89CE68`–`0x89CE8C` | TrafficController (`0x785428`) — traffic |

### Per car

```
physics assembly ("mover"):
  player:  PhysicsMover  vtable 0x7A25E0, tag 'Phys'   (full simulation)
  AI:      MellowMover   vtable 0x7A2538, tag 'Melo'   (cheap simulation)
     +0x04  driver: RealDriver 0x784FC8 (player) / PsychoDriver 0x784EB0 (AI)
     +0x20  RigidBody 0x7A20C4 'Rigi' (position +0x20, velocity +0x70; see car.md)
     +0x24  (Melo only) second 'Melo' part, vtable 0x7A2560
     +0x38..+0x44  4x Wheel 0x7A20E8 'Whee'
```

All RigidBodies come from one pool, 0x210 apart (seen: 0x060DA920 player, 0x060DAD40 traffic car, three more parked
far away, one unused). Only 6 were allocated at that moment.

Constructors (vtable writes): PsychoDriver at `0x40FEBE` (and `0x425AE2`), MellowMover at `0x5AFF83` / `0x5B6D48`.
Debug-name methods: `TrafficController` `0x429CF0` (vtable slot 48; walks `this+0x798`),
`StreamingTrafficCarManager` `0x412720` (walks a list at `this+0x900`). Strings: "Traffic in mellow mode"
(`0x783F6C`, in a name table at `0x7E8EBC`), "Be traffic car" (AI action, vtable `0x7848A0`).

### Not traffic

`0x42F910` (ZMenu's "No Traffic" patch target) is not the traffic system: with it patched, traffic and racers still
drive (user observation, 2026-11-14).

### Next

- Find where TrafficControllers get their car (callers of the PsychoDriver / MellowMover constructors) → spawner,
  density/count, and a real "no AI" switch.
- Find the AI's random source to make spawns repeatable.
