#!/usr/bin/env python3
"""Compares two PNGs, or every PNG two folders have in common.

    imgdiff.py <a.png|dirA> <b.png|dirB> [--map <out.png>]

Prints, per image: mean |a - b| summed over RGB / 765, the largest pixel difference (0..765), and the share of pixels
whose difference is above 12/765, the threshold the N0 test uses. --map writes the difference of a single pair,
brightened four times, so where two runs differ can be looked at. Needs Pillow and numpy (~/.venvs/clothlab has both).
"""
import os
import sys

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.int32)


def compare(path_a, path_b, map_path=None):
    a, b = load(path_a), load(path_b)
    if a.shape != b.shape:
        return "size differs"
    d = np.abs(a - b).sum(axis=2)
    if map_path:
        Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(map_path)
    return f"mean {d.mean() / 765:.5f}  max {int(d.max()):3d}/765  changed {(d > 12).mean() * 100:6.3f}%"


def main(argv):
    args = [x for x in argv[1:] if x != "--map"]
    map_path = None
    if "--map" in argv:
        map_path = argv[argv.index("--map") + 1]
        args.remove(map_path)
    if len(args) != 2:
        print(__doc__)
        return 1

    a, b = args
    if os.path.isdir(a):
        for name in sorted(os.listdir(a)):
            if name.endswith(".png") and os.path.exists(os.path.join(b, name)):
                print(f"{name:44} {compare(os.path.join(a, name), os.path.join(b, name))}")
    else:
        print(f"{os.path.basename(a):44} {compare(a, b, map_path)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
