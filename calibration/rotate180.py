#!/usr/bin/env python3
"""
rotate180.py - convert calibration JSONs from sensor orientation to the rig's
saved orientation (images rotated 180 degrees). ONE-TIME, run on your Mac.

The cameras are mounted upside down. The sensors still read out upside down,
but everything that SAVES or SHOWS an image (recording encoder, previews,
calibration snapshots) rotates it 180 degrees, so files on disk are upright.
Calibration solved on the old upside-down images is converted exactly:

  intrinsics   a 180-degree image rotation maps pixel (u, v) to
               (W-1-u, H-1-v), so  cx -> W-1-cx,  cy -> H-1-cy.
               fx, fy unchanged; fisheye distortion is radial, so D unchanged.
  extrinsics   each camera frame is rotated 180 deg about its optical axis,
               F = diag(-1,-1,1):  R -> F R F,  T -> F T.
               (pitch and yaw flip sign, roll is unchanged.)

The stitcher then sees CAM1 to the LEFT of CAM0 (negative yaw) and swaps the
two automatically - nothing to configure.

Every converted file gets "orientation": "rot180" and this script refuses to
convert a file that already has it, so running it twice is harmless.
Calibration solved from images captured AFTER the change (the /calib page) is
already upright - do not run this on it.

    python3 rotate180.py                 # converts cam0/cam1 intrinsics + stereo_extrinsics in .
    python3 rotate180.py --dir some/dir
"""

import argparse
import json
import os
import sys

import numpy as np

ORIENT = "rot180"
F = np.diag([-1.0, -1.0, 1.0])


def convert_intrinsics(d):
    w, h = d["image_width"], d["image_height"]
    K = d["camera_matrix"]
    K[0][2] = (w - 1) - K[0][2]
    K[1][2] = (h - 1) - K[1][2]
    return d


def convert_extrinsics(d):
    R = np.array(d["rotation_matrix"], dtype=float)
    d["rotation_matrix"] = (F @ R @ F).tolist()
    if "translation_mm" in d:
        d["translation_mm"] = [round(float(v), 4) for v in F @ np.array(d["translation_mm"], dtype=float)]
    eul = d.get("rotation_euler_deg")
    if eul:
        for k in ("pitch_x", "yaw_toe_y"):
            if k in eul:
                eul[k] = -eul[k]
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.dirname(os.path.abspath(__file__)))
    args = ap.parse_args()

    jobs = [("cam0_intrinsics.json", convert_intrinsics),
            ("cam1_intrinsics.json", convert_intrinsics),
            ("stereo_extrinsics.json", convert_extrinsics)]
    for name, fn in jobs:
        path = os.path.join(args.dir, name)
        if not os.path.exists(path):
            print(f"  skip  {name} (not found)")
            continue
        with open(path) as f:
            d = json.load(f)
        if d.get("orientation") == ORIENT:
            print(f"  skip  {name} (already {ORIENT})")
            continue
        d = fn(d)
        d["orientation"] = ORIENT
        with open(path, "w") as f:
            json.dump(d, f, indent=2)
        if "camera_matrix" in d:
            K = d["camera_matrix"]
            print(f"  done  {name}: cx {K[0][2]:.1f}, cy {K[1][2]:.1f}")
        else:
            R = np.array(d["rotation_matrix"])
            yaw = np.degrees(np.arctan2(R[2][0], R[2][2]))
            print(f"  done  {name}: yaw {yaw:+.1f} deg (stitcher puts cam1 on the left)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
