#!/usr/bin/env python3
"""Drive the frontend from the title screen into free roam (Explore Mode) with injected keys.

Key sequence (from the user, verified by screenshots in work/oracle/m4/):
  title -Enter-> Load "Nameless" -Enter-> Load complete -Enter-> Main Menu (Quick Race)
  -Left-> Career -Enter-> Resume Career -Enter-> The Garage -Enter-> "Return to Explore Mode?" (No)
  -Left-> Yes -Enter-> LOADING -> free roam
Needs a save in the prefix (C:\\users\\<user>\\AppData\\Local\\NFS Underground 2\\<profile>).

Free roam is detected by the picture moving away from the LOADING screen (stopgap until the car struct is mapped).
Exit code 0 = in free roam; prints the elapsed time.
"""
import io
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import probe_client as pc  # noqa: E402
from PIL import Image, ImageChops, ImageStat  # noqa: E402

# (key, label, enter-threshold) after the first (title) Enter
SEQUENCE = [("enter", "Load profile", 5), ("enter", "Load complete -> Main Menu", 5), ("left", "Career", 0),
            ("enter", "Career menu", 5), ("enter", "Resume Career -> Garage", 5), ("enter", "Explore Mode?", 5),
            ("left", "Yes", 0), ("enter", "Yes -> LOADING", 10)]
step = "start"


def log(msg: str) -> None:
    print(msg, flush=True)


def grab() -> Image.Image:
    png, _, _ = pc.screenshot_png(8, save=False)
    return Image.open(io.BytesIO(png)).convert("RGB")


def diff(a: Image.Image, b: Image.Image) -> float:
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3


def press_until_change(key: str, threshold: float, timeout: float, retry_every: float | None = None) -> float:
    before = grab()
    pc.key(key, "press", 120)
    t_end = time.monotonic() + timeout
    t_retry = time.monotonic() + retry_every if retry_every else None
    while time.monotonic() < t_end:
        time.sleep(0.2)
        d = diff(before, grab())
        if d > threshold:
            return d
        if t_retry and time.monotonic() >= t_retry:
            pc.key(key, "press", 120)
            t_retry = time.monotonic() + retry_every
    raise pc.ProbeError(f"screen did not react to {key!r} within {timeout}s (diff {d:.1f})")


def main() -> int:
    t0 = time.monotonic()
    try:
        pc.wait_frames(60)
        # title screen: wait until drawn, then Enter (ignored for a few seconds; retry)
        end = time.monotonic() + 30
        while sum(ImageStat.Stat(grab()).mean) / 3 < 12:
            if time.monotonic() > end:
                raise pc.ProbeError("title screen never appeared")
            time.sleep(0.25)
        global step
        step = "title"
        d = press_until_change("enter", 20, 20, retry_every=1.5)
        log(f"  {time.monotonic() - t0:5.1f}s title accepted Enter (diff {d:.1f})")
        time.sleep(0.6)
        for key, label, threshold in SEQUENCE:
            step = label
            if key == "left":
                pc.key(key, "press", 120)
                time.sleep(0.6)
                d = 0.0
            else:
                d = press_until_change(key, threshold, 6)
                time.sleep(0.6)
            log(f"  {time.monotonic() - t0:5.1f}s {label} (diff {d:.1f})")
        step = "loading"
        loading = grab()
        end = time.monotonic() + 40
        while diff(loading, grab()) < 25:
            if time.monotonic() > end:
                raise pc.ProbeError("still loading after 40 s")
            time.sleep(0.25)
        time.sleep(0.5)
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
