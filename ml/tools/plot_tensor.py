#!/usr/bin/env python3
"""Plot [TENSOR] dumps from UART (optional matplotlib)."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SK_SLICES = 49
SK_BINS = 40
COL = re.compile(r"^\[TENSOR\]\s+col(\d+):\s+(.+)$", re.M)


def load_tensor(path: Path) -> list[list[int]]:
    text = path.read_text(errors="replace")
    cols: dict[int, list[int]] = {}
    for m in COL.finditer(text):
        cols[int(m.group(1))] = [int(x) for x in m.group(2).split()]
    if len(cols) != SK_SLICES:
        raise ValueError(f"expected {SK_SLICES} columns, got {len(cols)}")
    return [cols[i] for i in range(SK_SLICES)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="UART log with [TENSOR] lines")
    parser.add_argument("-o", "--out", type=Path, default=None, help="PNG output path")
    args = parser.parse_args()

    try:
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("matplotlib/numpy not available; skip plots", file=sys.stderr)
        return 1

    mat = load_tensor(args.log)
    arr = np.array(mat, dtype=np.int16)

    out = args.out or args.log.with_suffix(".png")
    fig, ax = plt.subplots(figsize=(12, 5))
    im = ax.imshow(arr.T, aspect="auto", origin="lower", cmap="viridis", vmin=-128, vmax=127)
    ax.set_xlabel("time column (20 ms hop)")
    ax.set_ylabel("mel bin")
    ax.set_title(args.log.name)
    fig.colorbar(im, ax=ax, label="int8 feature")
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
