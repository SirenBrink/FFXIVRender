"""Summarize the last addon session without mixing appended older builds."""
import argparse
import csv
import re
import statistics
from pathlib import Path


def fields(line):
    return dict(re.findall(r"(?:^|, )([^=,:]+)=([^,]*)", line))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    lines = (args.directory / "XIVRender.log").read_text(encoding="utf-8-sig", errors="replace").splitlines()
    starts = [i for i, line in enumerate(lines) if re.search(r"\d+\.\d+\.\d+ native DLSS/NR/HDR/FG test build loaded", line)]
    start = starts[-1] if starts else 0
    lines = lines[start:]
    origin = int(lines[0].split()[0])
    print("Last session:", lines[0])
    rows, scene, inputs = [], {}, {}
    for line in lines:
        if "Session summary (periodic)" in line:
            scene = fields(line.split(": ", 1)[1])
        if "FG summary:" in line:
            timestamp = int(line.split()[0])
            value = fields(line.split("FG summary: ", 1)[1])
            rows.append({"seconds": (timestamp-origin)/1000, "tick": timestamp,
                         "provider": value.get("provider"), "factor": value.get("factor"),
                         "dynamic": value.get("native dynamic"), "active": value.get("active"),
                         "changes": value.get("live changes"), "real": int(value.get("real", 0)),
                         "displayed": int(value.get("displayed", 0)),
                         "nr_applied": scene.get("NR applied"),
                         "message": line.split("FG summary: ", 1)[1].split(", active=", 1)[0]})
        if "FG input summary:" in line and rows:
            inputs = fields(line.split("FG input summary: ", 1)[1])
            for key in ("missing", "invalid", "overflow", "stale", "overwritten", "resets"):
                rows[-1][key] = int(inputs.get(key, 0))
    prior = None
    for row in rows:
        elapsed = (row["tick"]-prior["tick"])/1000 if prior else 0
        row["real_fps"] = round((row["real"]-prior["real"])/elapsed, 3) if elapsed else 0
        row["displayed_fps"] = round((row["displayed"]-prior["displayed"])/elapsed, 3) if elapsed else 0
        prior = row
    if rows:
        with (args.directory / "fg-session-rates.csv").open("w", newline="", encoding="utf-8") as out:
            writer = csv.DictWriter(out, fieldnames=list(rows[-1]))
            writer.writeheader()
            writer.writerows(rows)
    print("seconds provider factor active changes realFPS displayFPS NR_applied missing resets message")
    for row in rows:
        if row["active"] == "1" or row is rows[-1] or row["provider"] == "0":
            print(*(row.get(key, "") for key in ("seconds", "provider", "factor", "active", "changes", "real_fps", "displayed_fps", "nr_applied", "missing", "resets", "message")))
    sl = (args.directory / "sl.log").read_text(encoding="utf-8-sig", errors="replace")
    times = []
    for line in sl.splitlines():
        if "Frame rate over 100.00ms" in line:
            match = re.search(r"\[(\d+)s:(\d+)ms:(\d+)us\]", line)
            if match:
                seconds, milliseconds, microseconds = map(int, match.groups())
                times.append(seconds + milliseconds/1000 + microseconds/1e6)
    intervals = [b-a for a, b in zip(times, times[1:])]
    print("SL >100ms warnings:", len(times))
    if intervals:
        print("Warning spacing seconds min/median/max:", min(intervals), statistics.median(intervals), max(intervals))
        print("Within1.9-2.1 seconds:", sum(1.9 <= value <= 2.1 for value in intervals), "/", len(intervals))


if __name__ == "__main__":
    main()
