# AI cars: traffic and roaming racers

Mapped live on 2026-11-14 with the user parked next to a traffic car in free roam (Bayview, Nameless save).
Class names come from each class's own name method at **vtable slot 2** (no MSVC RTTI on these classes).

## Roster (globals)

`0x0089CE58 .. 0x0089CE8C`: 14 controller pointers, one per car slot (counts at `0x89CE40..0x89CE4C` read 4,4,4,4):

| Slot | Global | Class (vtable) |
|---|---|---|
| 0 | `0x89CE58` | RealController / TrackedController (`0x785230`) — the player |
| 1–3 | `0x89CE5C`–`0x89CE64` | RaceCarController (`0x785370`, derives from TrafficController) — roaming racers (orange map arrows) |
| 4–13 | `0x89CE68`–`0x89CE8C` | TrafficController (`0x785428`) — traffic |

## Per car

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

## Not traffic

`0x42F910` (ZMenu's "No Traffic" patch target) is not the traffic system: with it patched, traffic and racers still
drive (user observation, 2026-11-14).

## Next

- Find where TrafficControllers get their car (callers of the PsychoDriver / MellowMover constructors) → spawner,
  density/count, and a real "no AI" switch.
- Find the AI's random source to make spawns repeatable.
