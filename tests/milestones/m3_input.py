#!/usr/bin/env python3
"""M3 acceptance: navigate one menu screen by injected input, confirmed by screenshots.
Launches the game, waits for the title screen, injects Enter, and checks the screen changed substantially
(title -> Save/Load profile dialog on a fresh prefix). Exit code 0 = pass. Leaves the game running unless --kill."""
import io
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "probe"))
import probe_client as pc  # noqa: E402
from PIL import Image, ImageChops, ImageStat  # noqa: E402


def shot() -> tuple[Image.Image, Path]:
    png, _, path = pc.screenshot_png(4)
    return Image.open(io.BytesIO(png)).convert("RGB"), path


def main() -> int:
    subprocess.run([str(ROOT / "tools" / "game" / "run_game.sh")], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, str(ROOT / "tools" / "probe" / "probe.py"), "shot-wait", "4", "30"], check=True,
                   stdout=subprocess.DEVNULL)
    t0 = time.monotonic()
    time.sleep(2.0)  # title fully faded in
    before, p0 = shot()
    # The title screen ignores input for a while after it appears (measured: ignored ~4 s after the first
    # frame, accepted ~10 s after). Retry until the screen changes.
    diff, attempts = 0.0, 0
    while attempts < 10:
        attempts += 1
        pc.key("enter", "press", 150)
        time.sleep(2.0)
        after, p1 = shot()
        diff = sum(ImageStat.Stat(ImageChops.difference(before, after)).mean) / 3
        if diff > 20:
            break
    print(f"  Enter accepted on attempt {attempts}, {time.monotonic() - t0:.1f} s after the title appeared")
    state = pc.call("input_state")
    kb = next((d for d in state["devices"] if d["kind"] == "keyboard"), {})
    ok = diff > 20 and kb.get("mapped_keys", 0) > 0
    print(f"{'PASS' if ok else 'FAIL'}  mean pixel difference title->after Enter = {diff:.1f} (need > 20), "
          f"keyboard mapped keys = {kb.get('mapped_keys')}")
    print(f"  before: {p0}\n  after:  {p1}")
    if "--kill" in sys.argv:
        subprocess.run([str(ROOT / "tools" / "game" / "run_game.sh"), "--kill"])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
