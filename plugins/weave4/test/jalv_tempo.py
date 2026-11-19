#!/usr/bin/env python3
"""Verify actual Jalv host tempo delivery without changing the server's tempo or routing."""
import os
import pathlib
import re
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
URI = "http://dmtry3.github.io/zynthian-eka/weave4"

def main():
    directory = ROOT / "build/lv2"
    directory.mkdir(exist_ok=True)
    link = directory / "eka-weave4.lv2"
    if not link.exists() and not link.is_symlink():
        link.symlink_to("../eka-weave4.lv2", target_is_directory=True)
    assert link.resolve() == ROOT / "build/eka-weave4.lv2"
    env = dict(os.environ, LV2_PATH=str(directory))
    log_path = ROOT / "build/jalv-tempo.txt"
    with log_path.open("w") as log:
        process = subprocess.Popen(["jalv", "-n", "EkaWeave-4Tempo", URI],
                                   stdin=subprocess.PIPE, stdout=log,
                                   stderr=subprocess.STDOUT, env=env, text=True)
        try:
            time.sleep(1)
            if process.poll() is not None:
                raise RuntimeError("Jalv did not start")
            process.stdin.write("mode=1\nchord=5\nenable4=1\nmaster=-6\n"
                                "selected_voice=4\noctave1=-1\nfeedback1=50\nsync1=1\ndivision1=12\n")
            process.stdin.flush()
            for _ in range(4):
                time.sleep(.3)
                process.stdin.write("controls\nmonitors\n")
                process.stdin.flush()
            time.sleep(.3)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    text = log_path.read_text()
    values = {key: float(value) for key, value in
              re.findall(r"([A-Za-z_][A-Za-z_0-9]*)\s*=\s*([-+0-9.eE]+)", text)}
    print({key: values.get(key) for key in ("bpm", "tempo_host", "delay_actual1", "beat_unit", "enabled_mask")})
    assert values.get("tempo_host") == 1, text
    expected = min(3000, 60000 / values["bpm"] * values["beat_unit"] / 4)
    assert abs(values["delay_actual1"] - expected) < .1, values
    assert values["enabled_mask"] == 9 and values["interval1"] == -8, values
    for key, expected in {"master": -6, "selected_voice": 4, "enable4": 1,
                          "feedback1": 50, "sync1": 1, "division1": 12}.items():
        assert values.get(key) == expected, (key, values.get(key))
    assert "doesn't exist" not in text and "control index out of range" not in text, text

if __name__ == "__main__":
    main()
