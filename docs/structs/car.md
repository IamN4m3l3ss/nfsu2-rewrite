# Player car (telemetry)

Chain and fields first found in the user's Camera Toolkit fork notes (`/mnt/m2drive/waah/games/NFSU2 Mod/CLAUDE.md`);
every field below was re-verified live on 2026-11-14 with the probe (`probe/src/car.cpp`).

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
