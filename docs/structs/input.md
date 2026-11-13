# Input (DirectInput 8)

Measured 2026-11-13 with the probe (`probe/src/input.cpp`) on NA v1.2 + the user's mod set.

## Devices

| Device | Created via | Data format | Cooperative level | Read |
|---|---|---|---|---|
| Mouse | `IDirectInput8A::CreateDevice(GUID_SysMouse)` | `SetDataFormat`, 20-byte state, 11 objects | FOREGROUND, NONEXCLUSIVE | `GetDeviceState(20)` every frame |
| Keyboard | DirectInput 8 **action mapping** (not `CreateDevice`; likely `EnumDevicesBySemantics`) | `BuildActionMap` + `SetActionMap`, genre `0x01000000`, map name `"NFS Underground 2"`, buffer 16 | — | `Poll` then buffered `GetDeviceData(20-byte records, 16)` every frame |

`g_pKeyboardDevice` = `*(IDirectInputDevice8A**)0x0086E5C0`.

## Keyboard read: `0x005C1840` (`__thiscall`, `this` = game input-device object)

- Only runs when `this->devType (+0x234) == 0x13` (DI8DEVTYPE_KEYBOARD).
- `Poll` (slot 25), then `GetDeviceData(0x14, buf, &n=16, 0)` (slot 10).
- On `DIERR_NOTACQUIRED` (0x8007000C) or `DIERR_INPUTLOST` (0x8007001E): `Acquire` (slot 7) and return.
- If `g_TextEntryMode` (byte `0x00870768`) is set: a separate path uses the events' `dwOfs` (typing).
- Otherwise, for each event: **only `dwData` and `uAppData` are used**. `mask = _allshl(1, uAppData)`;
  pressed (`dwData != 0`) → `this->heldActions (+0x08, u64) |= mask`, released → `&= ~mask`.
  `uAppData` 3 or 4 additionally sets byte `0x00870785 = 1`, `0x00870784 = 0`.

So injected keyboard events must carry the game's action id in `uAppData`; `dwOfs` alone does nothing outside text
entry.

## Frontend action map (32 actions, from `SetActionMap`)

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

## Timing

The title screen ignores Enter for a few seconds after it appears (ignored ~4 s after the first frame, accepted on the
2nd try ~6 s after the title appeared, deterministic over 4 runs).
