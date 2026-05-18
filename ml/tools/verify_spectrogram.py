#!/usr/bin/env python3
"""Compare device [TENSOR] logs to micro_speech reference (host kiss frontend)."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
VENV_PY = REPO / ".venv" / "bin" / "python"
BUILD_DIR = REPO / "build" / "host_speech"
VERIFY_BIN = BUILD_DIR / "spectrogram_verify"

SK_SLICES = 49
SK_BINS = 40
PAD_TARGET = 100

COL_LINE = re.compile(r"^\[TENSOR\]\s+col(\d+):\s+(.+)$", re.M)
SHOT_LINE = re.compile(
    r"\[SHOT\]\s+hops=(\d+)\s+pk=(\d+)\s+col(\d+)\.\.(\d+)\s+cls=(\w+)"
)


def venv_python() -> str:
    return str(VENV_PY if VENV_PY.is_file() else Path(sys.executable))


def parse_tensor_log(text: str) -> list[list[int]]:
    cols: dict[int, list[int]] = {}
    for m in COL_LINE.finditer(text):
        idx = int(m.group(1))
        vals = [int(x) for x in m.group(2).split()]
        if len(vals) != SK_BINS:
            raise ValueError(f"col{idx}: expected {SK_BINS} bins, got {len(vals)}")
        cols[idx] = vals
    if len(cols) != SK_SLICES:
        raise ValueError(f"expected {SK_SLICES} columns, got {len(cols)}")
    return [cols[i] for i in range(SK_SLICES)]


def col_energy(col: list[int], pad: int = PAD_TARGET) -> int:
    return sum(abs(v - pad) for v in col)


def analyze_log(path: Path) -> None:
    text = path.read_text(errors="replace")
    mat = parse_tensor_log(text)
    print(f"=== {path.name} ===")
    active = [c for c in range(SK_SLICES) if col_energy(mat[c]) > 400]
    print(f"active cols: {active[0]}..{active[-1]} ({len(active)} cols)" if active else "no active")
    sat = sum(1 for c in mat for v in c if v >= 127)
    neg = sum(1 for c in mat for v in c if v <= -60)
    print(f"saturated={sat} very_negative={neg}")
    sm = SHOT_LINE.search(text)
    if sm:
        print(f"firmware: hops={sm.group(1)} col{sm.group(3)}..{sm.group(4)} cls={sm.group(5)}")


def build_host() -> None:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cmake", "-S", str(REPO / "ml/host"), "-B", str(BUILD_DIR)],
        check=True,
        cwd=REPO,
    )
    subprocess.run(
        ["cmake", "--build", str(BUILD_DIR), "-j"],
        check=True,
        cwd=REPO,
    )


def run_ref() -> str:
    if not VERIFY_BIN.is_file():
        build_host()
    return subprocess.run([str(VERIFY_BIN)], check=True, capture_output=True, text=True).stdout


def align_to_ref(device: list[list[int]], ref: list[list[int]], speech_cols: int = 12) -> None:
    """Find best 12-col window in ref vs device speech block (cols 31..42)."""
    dev0 = SK_SPEECH_END_COL + 1 - speech_cols
    dev = [device[c] for c in range(dev0, SK_SPEECH_END_COL + 1)]
    best = (0, 1 << 30)
    for start in range(SK_SLICES - speech_cols + 1):
        win = [ref[c] for c in range(start, start + speech_cols)]
        l1 = sum(abs(a - b) for d, r in zip(dev, win) for a, b in zip(d, r))
        if l1 < best[1]:
            best = (start, l1)
    print(f"best ref window cols {best[0]}..{best[0] + speech_cols - 1} L1={best[1]} vs device {dev0}..{SK_SPEECH_END_COL}")


def compare_logs(yes_log: Path, no_log: Path) -> None:
    yes = parse_tensor_log(yes_log.read_text(errors="replace"))
    no = parse_tensor_log(no_log.read_text(errors="replace"))
    diff = 0
    for c in range(31, 43):
        for b in range(SK_BINS):
            if yes[c][b] != no[c][b]:
                diff += 1
    print(f"device yes vs no cols 31..42: {diff}/480 cells differ")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, action="append", help="UART log with [TENSOR]")
    parser.add_argument("--ref", action="store_true", help="Run host spectrogram_verify on golden PCM")
    parser.add_argument("--compare", nargs=2, metavar=("YES_LOG", "NO_LOG"))
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args()

    if args.rebuild and BUILD_DIR.is_dir():
        import shutil

        shutil.rmtree(BUILD_DIR)

    if args.ref:
        print(run_ref())
    for p in args.log or []:
        analyze_log(p)
    if args.compare:
        compare_logs(Path(args.compare[0]), Path(args.compare[1]))
    if not any([args.ref, args.log, args.compare]):
        parser.print_help()
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
