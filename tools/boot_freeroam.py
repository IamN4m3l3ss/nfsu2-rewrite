#!/usr/bin/env python3
"""Drive the frontend from the title screen into free roam (Explore Mode) with injected keys.

Key sequence (from the user, verified by screenshots in work/oracle/m4/):
  title -Enter-> Load "Nameless" -Enter-> Load complete -Enter-> Main Menu (Quick Race)
  -Left-> Career -Enter-> Resume Career -Enter-> The Garage -Enter-> "Return to Explore Mode?" (No)
  -Left-> Yes -Enter-> LOADING -> free roam
Needs a save in the prefix (C:\\users\\<user>\\AppData\\Local\\NFS Underground 2\\<profile>).

Game state int at 0x008654A4 (from the user's Camera Toolkit fork notes, verified live 2026-11-13):
3 = frontend, 4 = loading started (after "Yes"), 5 = loading, 6 = in game / free roam.
Timing knobs below; env BOOT_LEFT_WAIT / BOOT_RETRY override them for experiments.
Exit code 0 = in free roam; prints the elapsed time.
"""
import io
import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import probe_client as pc  # noqa: E402
from PIL import Image, ImageChops, ImageStat  # noqa: E402

step = "start"
GAME_STATE = 0x008654A4
LEFT_WAIT = float(os.environ.get("BOOT_LEFT_WAIT", "0.3"))  # after Left (only moves a highlight)
RETRY = float(os.environ.get("BOOT_RETRY", "1.0"))  # re-press Enter if the screen ignored it


def game_state() -> int:
    return struct.unpack("<i", bytes.fromhex(pc.call("read_mem", {"addr": GAME_STATE, "size": 4})["hex"]))[0]


def log(msg: str) -> None:
    print(msg, flush=True)


def grab() -> Image.Image:
    png, _, _ = pc.screenshot_png(8, save=False)
    return Image.open(io.BytesIO(png)).convert("RGB")


def diff(a: Image.Image, b: Image.Image) -> float:
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3


REFS = Path(__file__).resolve().parents[1] / "work" / "boot_refs"  # 320x180 shots of each screen (gitignored)
# screen class -> reference images (the two Left steps only move a highlight, so they share a class)
CLASSES = {"title": ["title"], "load": ["load"], "load_complete": ["load_complete"],
           "main": ["main_quickrace", "main_career"], "career": ["career"], "garage": ["garage"],
           "explore": ["explore_no", "explore_yes"]}
YESNO_STRIP = (48, 100, 278, 134)  # Yes/No buttons of the Explore dialog, in 320x180 coordinates
_refs: dict[str, Image.Image] = {}


def ref(name: str) -> Image.Image:
    if name not in _refs:
        _refs[name] = Image.open(REFS / f"{name}.png").convert("RGB")
    return _refs[name]


def frame() -> Image.Image:
    return grab().resize((320, 180))


def classify(img: Image.Image) -> tuple[str, float]:
    best = min(((c, min(diff(img, ref(r)) for r in rs)) for c, rs in CLASSES.items()), key=lambda x: x[1])
    return best


def wait_for(cls: str, timeout: float, max_dist: float = 15.0) -> float:
    """Wait until two consecutive frames classify as `cls` (a fading/half-drawn frame matches nothing)."""
    end = time.monotonic() + timeout
    hits, last = 0, ("?", 99.0)
    while time.monotonic() < end:
        last = classify(frame())
        hits = hits + 1 if (last[0] == cls and last[1] < max_dist) else 0
        if hits >= 2:
            return last[1]
        time.sleep(0.05)
    raise pc.ProbeError(f"expected screen '{cls}', got '{last[0]}' (dist {last[1]:.1f}) after {timeout}s")


def step_enter(src: str, dst: str, timeout: float = 8.0) -> float:
    """Press Enter on screen `src` until screen `dst` is confirmed. Re-press only while still clearly on `src`
    (screens ignore input for a moment after they appear); never once a transition has started."""
    end = time.monotonic() + timeout
    while True:
        pc.key("enter", "press", 80)
        try:
            return wait_for(dst, RETRY)
        except pc.ProbeError:
            pass
        if time.monotonic() > end:
            raise pc.ProbeError(f"'{src}' never accepted Enter")
        c, dist = classify(frame())
        if not (c == src and dist < 10):
            return wait_for(dst, 4)  # in transition: wait for it, don't press again


def main() -> int:
    global step
    t0 = time.monotonic()
    el = lambda: f"{time.monotonic() - t0:5.1f}s"
    try:
        pc.wait_frames(60)
        step = "title"
        wait_for("title", 30)
        # The title ignores Enter for a few seconds. Re-press only while it is still showing the title, so a
        # late-reacting first press can't be followed by a second Enter on the next screen.
        prev = "title"
        for key, expect in [("enter", "load"), ("enter", "load_complete"), ("enter", "main"), ("left", None),
                            ("enter", "career"), ("enter", "garage"), ("enter", "explore"), ("left", None)]:
            step = f"{key} -> {expect or 'highlight'}"
            if key == "left":
                pc.key("left", "press", 80)
                time.sleep(LEFT_WAIT)
                continue
            dist = step_enter(prev, expect, timeout=20 if prev == "title" else 8)
            log(f"  {el()} {expect} (dist {dist:.1f})")
            prev = expect
        # Never press the last Enter blindly: anywhere else it starts a race instead of free roam.
        step = "Yes -> LOADING"
        strip = frame().crop(YESNO_STRIP)
        d_yes = diff(strip, ref("explore_yes").crop(YESNO_STRIP))
        d_no = diff(strip, ref("explore_no").crop(YESNO_STRIP))
        if not (classify(frame())[0] == "explore" and d_yes < d_no):
            raise pc.ProbeError(f"not on 'Return to Explore Mode? -> Yes' (strip yes {d_yes:.1f}, no {d_no:.1f})")
        log(f"  {el()} Yes selected (strip yes {d_yes:.1f}, no {d_no:.1f})")
        pc.key("enter", "press", 80)
        end = time.monotonic() + 6
        while game_state() == 3:
            if time.monotonic() > end:
                raise pc.ProbeError("game state still 3 (frontend) 6 s after Yes")
            time.sleep(0.05)
        log(f"  {el()} loading (state {game_state()})")
        step = "loading"
        end = time.monotonic() + 40
        while game_state() != 6:
            if time.monotonic() > end:
                raise pc.ProbeError(f"game state {game_state()} after 40 s of loading")
            time.sleep(0.1)
        _, meta, path = pc.screenshot_png(4)
    except pc.ProbeError as e:
        print(f"FAIL at step '{step}' after {time.monotonic() - t0:.1f}s: {e}", file=sys.stderr)
        try:
            _, _, path = pc.screenshot_png(4)
            print(f"  screen at failure: {path}", file=sys.stderr)
        except pc.ProbeError:
            print("  (no screenshot: probe gone -> game crashed? see work/logs/game_latest.log)", file=sys.stderr)
        return 1
    print(f"free roam after {time.monotonic() - t0:.1f}s (frame {meta['frame']}), screenshot {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
