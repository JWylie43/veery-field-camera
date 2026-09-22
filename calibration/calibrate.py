#!/usr/bin/env python3
"""
calibrate.py - Full ChArUco calibration for the ROCK 5T stereo rig (run on your Mac)

This is a PROCESSING tool, not something the Rock runs. In ONE pass over the
snapshot pairs it computes, for the rig now fixed in its housing:
  1. CAM0 camera intrinsics  (K, distortion, FOV)
  2. CAM1 camera intrinsics  (K, distortion, FOV)
  3. STEREO extrinsics       (rotation + translation between the two cameras:
                              baseline in mm and toe-in angle)

Workflow:
  1. On the Rock, run calibration/calib_server.py (or snap_pair.sh) and capture
     ~30-40 pairs with the ChArUco board at varied angles/distances. Cover each
     camera's whole frame (edges/corners) AND get a good batch with the board
     centered where BOTH cameras see it (that overlap set is what the extrinsics
     are solved from).
  2. Pull the shots to your Mac (from the calibration/ folder):
         scp -r radxa@rock.local:~/calib0 images-cam0
         scp -r radxa@rock.local:~/calib1 images-cam1
  3. Run this (from the calibration/ folder):
         python3 calibrate.py

The rig records TWO INDEPENDENT full-frame files per pose, one per camera - the
cameras are paired by sort order, so shoot them back-to-back (a static board
needs no sync). Intrinsics are solved per camera from that camera's own views;
extrinsics only from poses where BOTH cameras see the board, with the intrinsics
held FIXED (the stable way).

Intrinsics are mount-independent; extrinsics describe the fixed geometry of the
CURRENT housing - re-run this if you ever disturb the mount.

*** Projection model: FISHEYE (equidistant), always. ***
The rig's CIL391 lenses are 110 deg H with ~-16% barrel. The pinhole+polynomial
models (OpenCV "standard"/"rational") fit them with a uniform ~2.6px residual -
a model mismatch, not noise. cv2.fisheye lands at ~0.23px. Every JSON this
writes carries "model": "fisheye", and the stitcher requires it.

Requires OpenCV with the aruco module (>= 4.7):
    pip install -U opencv-contrib-python numpy

Board defaults match the printed board MEASURED with calipers (7x10,
DICT_5X5_1000, square=78mm, marker=58mm). If you reprint at a different scale,
re-measure a square and pass --square-mm / --marker-mm so the baseline scale is
correct.

Outputs (in --out dir):
    cam0_intrinsics.json   cam1_intrinsics.json   stereo_extrinsics.json
    <cam>_undistort_sample.jpg   stereo_rectified_sample.jpg
"""


import argparse
import re
import glob
import json
import math
import os
import sys

import numpy as np

try:
    import cv2
    import cv2.aruco as aruco
except ImportError:
    sys.exit("OpenCV not found. Install with:  pip install -U opencv-contrib-python numpy")

if not hasattr(aruco, "CharucoDetector"):
    sys.exit("Your OpenCV is too old for this script (needs the >=4.7 aruco API).\n"
             "Upgrade:  pip install -U opencv-contrib-python")


def build_board(cfg):
    dict_const = getattr(aruco, cfg.dict, None)
    if dict_const is None:
        sys.exit(f"Unknown ArUco dictionary '{cfg.dict}'. Examples: DICT_5X5_1000, "
                 f"DICT_4X4_50, DICT_6X6_250.")
    dictionary = aruco.getPredefinedDictionary(dict_const)
    # Units in mm: intrinsics (pixels) are unaffected by the choice; the extrinsic
    # translation (baseline) inherits these units, so measure the board for real.
    board = aruco.CharucoBoard(
        (cfg.squares_x, cfg.squares_y), cfg.square_mm, cfg.marker_mm, dictionary)
    detector = aruco.CharucoDetector(board)
    return board, detector


def grid_coverage(points, w, h, gx=8, gy=5):
    """Fraction of a gx*gy grid that has at least one detected corner."""
    filled = set()
    for p in points:
        x, y = float(p[0]), float(p[1])
        cx = min(gx - 1, max(0, int(x / w * gx)))
        cy = min(gy - 1, max(0, int(y / h * gy)))
        filled.add((cx, cy))
    return len(filled) / float(gx * gy)


def detect_all(files, detector, cfg, cam1_files=None):
    """One detection pass over every image. Returns (records, size).

    records: list of {path, <cam>: (corners, ids) | None, <cam>_n: int}
             where cam is 'cam0'/'cam1' (or 'single' with --single).
    size:    (w, h) of one camera's image - used as the calibration size.

    The rig captures one FULL FRAME per camera per pose: files[i] is CAM0's and
    cam1_files[i] is CAM1's, paired by sort order (shoot them back-to-back per
    board pose; a static board needs no sync). With --single there is one camera
    and no pairing.
    """
    cams = ["single"] if cfg.single else ["cam0", "cam1"]
    records, size = [], None
    pair_iter = (list(zip(sorted(files), sorted(cam1_files)))
                 if cam1_files is not None else [(p, None) for p in sorted(files)])
    for path, rpath in pair_iter:
        img = cv2.imread(path)
        if img is None:
            continue
        if rpath is not None:
            rimg = cv2.imread(rpath)
            if rimg is None:
                continue
            frames = {"cam0": img, "cam1": rimg}
            rec = {"path": path, "path_cam1": rpath}
        else:
            frames = {"single": img}
            rec = {"path": path}
        for side in cams:
            gray = cv2.cvtColor(frames[side], cv2.COLOR_BGR2GRAY)
            if size is None:
                size = (gray.shape[1], gray.shape[0])   # (w, h)
            ch_corners, ch_ids, _, _ = detector.detectBoard(gray)
            n = 0 if ch_ids is None else len(ch_ids)
            rec[side] = (ch_corners, ch_ids) if (ch_ids is not None and n >= 6) else None
            rec[side + "_n"] = n
        records.append(rec)
    return records, size


def calibrate_intrinsics(name, records, board, size, cfg, out_dir):
    """Per-camera intrinsics from that camera's detected views. Returns
    (result_dict, K, dist) or (None, None, None) if there aren't enough views."""
    all_obj, all_img, all_pts, recs_used = [], [], [], []
    used = 0
    print(f"\n=== {name.upper()} camera (intrinsics) ===")
    for rec in records:
        det = rec.get(name)
        if det is not None:
            ch_corners, ch_ids = det
            obj, imgp = board.matchImagePoints(ch_corners, ch_ids)
            if obj is not None and len(obj) >= 6:
                all_obj.append(obj)
                all_img.append(imgp)
                all_pts.extend(imgp.reshape(-1, 2))
                recs_used.append(rec["path"])
                used += 1
        print(f"  {os.path.basename(rec['path']):28} corners: {rec.get(name + '_n', 0)}")

    if used < 8:
        print(f"  ! Only {used} usable views for {name} (want >= ~15). "
              f"Take more shots covering the frame edges/corners and re-run.")
        if used < 4:
            print(f"  Too few for {name}; skipping this camera.")
            return None, None, None

    # FISHEYE (equidistant) only - see the module docstring. The pinhole and
    # rational-polynomial models leave a ~2.6px uniform residual on this lens.
    return _calibrate_fisheye(name, all_obj, all_img, all_pts, recs_used,
                              used, board, size, cfg, out_dir, records)


def _calibrate_fisheye(name, all_obj, all_img, all_pts, recs_used, used,
                       board, size, cfg, out_dir, records):
    """cv2.fisheye (equidistant) solve - the right projection for the CIL391
    (110deg H, GoPro-style): the pinhole+polynomial models fit it with a
    uniform ~2.6px residual (model mismatch), fisheye is its native shape."""
    obj = [o.reshape(1, -1, 3).astype(np.float64) for o in all_obj]
    img = [i.reshape(1, -1, 2).astype(np.float64) for i in all_img]
    K = np.zeros((3, 3))
    D = np.zeros((4, 1))
    # These flags live under cv2.fisheye in OpenCV 4.x and were promoted to the
    # top level in 5.x - accept either so the script runs on both.
    _f = cv2.fisheye if hasattr(cv2.fisheye, "CALIB_RECOMPUTE_EXTRINSIC") else cv2
    fflags = (_f.CALIB_RECOMPUTE_EXTRINSIC | _f.CALIB_FIX_SKEW)
    crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 200, 1e-7)
    # fisheye.calibrate can throw on degenerate views; drop offenders and retry
    names = list(recs_used)
    for _ in range(10):
        try:
            rms, K, D, _, _ = cv2.fisheye.calibrate(
                obj, img, size, K, D, flags=fflags, criteria=crit)
            break
        except cv2.error as e:
            m = re.search(r"input array (\d+)", str(e))
            if m and len(obj) > 8:
                i = int(m.group(1))
                print(f"  dropping degenerate view {os.path.basename(names[i])}")
                for lst in (obj, img, names):
                    del lst[i]
                K = np.zeros((3, 3)); D = np.zeros((4, 1))
            else:
                raise
    used = len(obj)
    w, h = size
    fx, fy, cx, cy = K[0, 0], K[1, 1], K[0, 2], K[1, 2]
    # equidistant projection: r = f * theta  ->  edge angle = (w/2)/fx radians
    hfov = math.degrees(2 * ((w / 2 - abs(cx - w / 2)) + abs(cx - w / 2)) / (2 * fx) * 2) / 2
    hfov = math.degrees(w / fx)
    vfov = math.degrees(h / fy)
    cov = grid_coverage(all_pts, w, h)
    print(f"  model           : FISHEYE (equidistant)")
    print(f"  images used     : {used}")
    print(f"  image size      : {w} x {h}")
    print(f"  RMS reproj error: {rms:.3f} px   ({'good' if rms < 1.0 else 'high - see notes'})")
    print(f"  focal (fx, fy)  : {fx:.1f}, {fy:.1f} px")
    print(f"  principal (cx,cy): {cx:.1f}, {cy:.1f}")
    print(f"  distortion (k1-4): {np.round(D.ravel(), 5).tolist()}")
    print(f"  >> FOV          : {hfov:.1f} deg horizontal, {vfov:.1f} deg vertical")
    print(f"  frame coverage  : {cov*100:.0f}% of an 8x5 grid "
          f"({'good' if cov > 0.8 else 'thin - add edge/corner shots'})")
    result = {
        "camera": name, "model": "fisheye",
        "image_width": w, "image_height": h,
        "rms_reproj_error_px": round(float(rms), 4),
        "camera_matrix": K.tolist(),
        "distortion_coefficients": D.ravel().tolist(),
        "fov_horizontal_deg": round(hfov, 2),
        "fov_vertical_deg": round(vfov, 2),
        "images_used": used,
        "board": {"squares_x": cfg.squares_x, "squares_y": cfg.squares_y,
                  "square_mm": cfg.square_mm, "marker_mm": cfg.marker_mm,
                  "dictionary": cfg.dict},
    }
    out_json = os.path.join(out_dir, f"{name}_intrinsics.json")
    with open(out_json, "w") as f:
        json.dump(result, f, indent=2)
    print(f"  saved -> {out_json}")
    # undistort sample via fisheye maps
    for rec in records:
        if rec.get(name) is None:
            continue
        imgf = cv2.imread(rec["path"])
        if imgf is None:
            continue
        frame = imgf if name != "cam1" else cv2.imread(rec["path_cam1"])
        newK = cv2.fisheye.estimateNewCameraMatrixForUndistortRectify(
            K, D, size, np.eye(3), balance=0.4)
        m1, m2 = cv2.fisheye.initUndistortRectifyMap(
            K, D, np.eye(3), newK, size, cv2.CV_16SC2)
        und = cv2.remap(frame, m1, m2, cv2.INTER_LINEAR)
        out_img = os.path.join(out_dir, f"{name}_undistort_sample.jpg")
        cv2.imwrite(out_img, und)
        print(f"  saved -> {out_img}  (eyeball: straight lines should be straight)")
        break
    return result, K, D


# Above this stereo reprojection error the solve is not believable for this rig
# (a genuine housed-rig calibration lands under ~1px).
MAX_STEREO_RMS_PX = 3.0


def calibrate_extrinsics(records, board, size, KL, dL, KR, dR, cfg, out_dir):
    """Stereo extrinsics from frames where BOTH cameras see the board. Intrinsics
    are held FIXED. Returns the result dict or None if there aren't enough pairs."""
    print("\n=== STEREO extrinsics (cam0 -> cam1) ===")
    chess = board.getChessboardCorners().astype(np.float32)   # (Ncorners, 3) in mm
    obj_pts, pts_l, pts_r = [], [], []
    used = 0
    for rec in records:
        if rec.get("cam0") is None or rec.get("cam1") is None:
            continue
        cl, il = rec["cam0"]
        cr, ir = rec["cam1"]
        il, ir = il.flatten(), ir.flatten()
        common = np.intersect1d(il, ir)                       # ids both cameras saw
        if len(common) < 6:
            continue
        map_l = {int(i): c for i, c in zip(il, cl.reshape(-1, 2))}
        map_r = {int(i): c for i, c in zip(ir, cr.reshape(-1, 2))}
        obj_pts.append(np.array([chess[int(i)] for i in common],
                                dtype=np.float32).reshape(-1, 1, 3))
        pts_l.append(np.array([map_l[int(i)] for i in common],
                              dtype=np.float32).reshape(-1, 1, 2))
        pts_r.append(np.array([map_r[int(i)] for i in common],
                              dtype=np.float32).reshape(-1, 1, 2))
        used += 1
        print(f"  {os.path.basename(rec['path']):28} shared corners: {len(common)}")

    if used < 6:
        print(f"  ! Only {used} stereo views (want >= ~10). Take more shots with the "
              f"board centered where BOTH cameras see it, at varied depth/tilt.")
        if used < 3:
            print("  Too few for a reliable stereo solve; skipping extrinsics.")
            return None

    crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-6)
    sflags = cv2.CALIB_FIX_INTRINSIC
    rms, KL, dL, KR, dR, R, T, _, _ = cv2.stereoCalibrate(
        obj_pts, pts_l, pts_r, KL, dL, KR, dR, size,
        flags=sflags, criteria=crit)

    T = T.ravel()
    baseline = float(np.linalg.norm(T))                       # mm between camera centers
    # Proper Euler decomposition (deg) for pitch/yaw/roll; RQDecomp3x3 returns the
    # per-axis angles. Total is the true geodesic rotation (Rodrigues vector norm).
    euler = np.asarray(cv2.RQDecomp3x3(R)[0], dtype=float)    # [pitch(x), yaw(y), roll(z)]
    total = float(np.degrees(np.linalg.norm(cv2.Rodrigues(R)[0])))
    toe = abs(float(euler[1]))                                # convergence about vertical

    print(f"  stereo views used : {used}")
    print(f"  RMS reproj error  : {rms:.3f} px   ({'good' if rms < 1.0 else 'high - see notes'})")
    print(f"  baseline          : {baseline:.2f} mm   (T = "
          f"[{T[0]:.1f}, {T[1]:.1f}, {T[2]:.1f}] mm)")
    print(f"  rotation (deg)    : pitch {euler[0]:+.2f}, yaw/toe {euler[1]:+.2f}, "
          f"roll {euler[2]:+.2f}   (total {total:.2f})")
    print(f"  >> toe-in angle   : {toe:.2f} deg   (relative convergence of the cameras)")

    result = {
        "rms_reproj_error_px": round(float(rms), 4),
        "stereo_views_used": used,
        "image_width": size[0], "image_height": size[1],
        "baseline_mm": round(baseline, 3),
        "translation_mm": [round(float(v), 4) for v in T],
        "rotation_matrix": R.tolist(),
        "rotation_euler_deg": {"pitch_x": round(float(euler[0]), 3),
                               "yaw_toe_y": round(float(euler[1]), 3),
                               "roll_z": round(float(euler[2]), 3)},
        "toe_in_angle_deg": round(toe, 3),
        "total_rotation_deg": round(total, 3),
        "board": {"squares_x": cfg.squares_x, "squares_y": cfg.squares_y,
                  "square_mm": cfg.square_mm, "marker_mm": cfg.marker_mm,
                  "dictionary": cfg.dict},
    }
    # Sanity gate. A real solve on this rig lands well under 1px; anything
    # above a few px means the "pairs" are not simultaneous views of the same
    # board pose. That happens when cam0/cam1 folders hold INDEPENDENT
    # per-camera intrinsics shoots (calib_server.py writes ~/calib0 and
    # ~/calib1 separately, both numbered img_NNN) - sort-order pairing then
    # matches unrelated frames and the solve is meaningless. Use snap_pair.sh,
    # which captures one genuine cam0+cam1 pair per board pose.
    # Refusing to write is deliberate: silently replacing a good
    # stereo_extrinsics.json with a bad one produces a plausible-looking
    # panorama built from the wrong geometry.
    if rms > MAX_STEREO_RMS_PX and not cfg.force_extrinsics:
        print(f"\n  !! REFUSING to write stereo_extrinsics.json: RMS {rms:.1f}px "
              f"exceeds {MAX_STEREO_RMS_PX}px.")
        print( "     The cam0/cam1 images are almost certainly not SIMULTANEOUS "
               "pairs of the\n     same board pose. Capture real pairs with "
               "snap_pair.sh and re-run.")
        print( "     (Pass --force-extrinsics to write it anyway.)")
        return None

    out_json = os.path.join(out_dir, "stereo_extrinsics.json")
    with open(out_json, "w") as f:
        json.dump(result, f, indent=2)
    print(f"  saved -> {out_json}")

    _save_rectified_sample(records, size, KL, dL, KR, dR, R, T, out_dir)
    return result


def _save_rectified_sample(records, size, KL, dL, KR, dR, R, T, out_dir):
    """Rectify the first both-visible frame and draw horizontal rulers. If the
    stereo solve is good, matching features sit on the SAME horizontal line."""
    try:
        pair = next((r for r in records
                     if r.get("cam0") is not None and r.get("cam1") is not None), None)
        if pair is None:
            return
        left, right = cv2.imread(pair["path"]), cv2.imread(pair["path_cam1"])
        R1, R2, P1, P2, _, _, _ = cv2.stereoRectify(
            KL, dL, KR, dR, size, R, T.reshape(3, 1), alpha=0)
        ml = cv2.initUndistortRectifyMap(KL, dL, R1, P1, size, cv2.CV_16SC2)
        mr = cv2.initUndistortRectifyMap(KR, dR, R2, P2, size, cv2.CV_16SC2)
        rl = cv2.remap(left, ml[0], ml[1], cv2.INTER_LINEAR)
        rr = cv2.remap(right, mr[0], mr[1], cv2.INTER_LINEAR)
        combo = np.hstack([rl, rr])
        for y in range(0, combo.shape[0], combo.shape[0] // 20):
            cv2.line(combo, (0, y), (combo.shape[1], y), (0, 255, 0), 1)
        out_img = os.path.join(out_dir, "stereo_rectified_sample.jpg")
        cv2.imwrite(out_img, combo)
        print(f"  saved -> {out_img}  (eyeball: same feature should land on the same green line)")
    except Exception as e:                               # noqa: BLE001 - sanity image only
        print(f"  (rectified sample skipped: {e!r})")


def load_intrinsics(dirpath, name):
    """Load a saved <name>_intrinsics.json -> (K, dist)."""
    path = os.path.join(dirpath, f"{name}_intrinsics.json")
    with open(path) as f:
        j = json.load(f)
    K = np.array(j["camera_matrix"], dtype=float)
    dist = np.array(j["distortion_coefficients"], dtype=float).reshape(1, -1)
    return K, dist


def main():
    ap = argparse.ArgumentParser(
        description="ChArUco calibration for the ROCK 5T rig: cam0 + cam1 "
                    "intrinsics (fisheye) + stereo extrinsics.")
    ap.add_argument("--cam0-glob", default="images-cam0/*",
                    help="glob for CAM0 frames (default: images-cam0/*)")
    ap.add_argument("--cam1-glob", default="images-cam1/*",
                    help="glob for CAM1 frames (default: images-cam1/*)")
    ap.add_argument("--out", default=".",
                    help="where to write results (default: alongside the scripts, "
                         "which is where the stitcher looks)")
    ap.add_argument("--use-intrinsics", default=None, metavar="DIR",
                    help="load cam0_intrinsics.json & cam1_intrinsics.json from DIR "
                         "and compute ONLY extrinsics (skip re-computing intrinsics)")
    ap.add_argument("--squares-x", type=int, default=7)
    ap.add_argument("--squares-y", type=int, default=10)
    ap.add_argument("--square-mm", type=float, default=78.0,
                    help="MEASURED printed square size in mm (default 78)")
    ap.add_argument("--marker-mm", type=float, default=58.0,
                    help="MEASURED printed marker size in mm (default 58)")
    ap.add_argument("--dict", default="DICT_5X5_1000", help="ArUco dictionary name")
    ap.add_argument("--force-extrinsics", action="store_true",
                    help=f"write stereo_extrinsics.json even if its RMS exceeds "
                         f"{MAX_STEREO_RMS_PX}px (normally refused - see the "
                         f"note in calibrate_extrinsics)")
    ap.add_argument("--single", action="store_true",
                    help="ONE camera only: solve intrinsics from --cam0-glob and "
                         "write single_intrinsics.json (no pairing, no extrinsics)")
    args = ap.parse_args()

    files = sorted(set(glob.glob(args.cam0_glob)))
    files = [f for f in files if os.path.splitext(f)[1].lower()
             in (".jpg", ".jpeg", ".png")]
    cam1_files = None
    if not args.single:
        cam1_files = sorted(set(glob.glob(args.cam1_glob)))
        cam1_files = [f for f in cam1_files if os.path.splitext(f)[1].lower()
                      in (".jpg", ".jpeg", ".png")]
        if len(files) != len(cam1_files) or not files:
            sys.exit(f"Need equal, nonzero counts - got {len(files)} cam0 "
                     f"('{args.cam0_glob}') vs {len(cam1_files)} cam1 "
                     f"('{args.cam1_glob}'). Pairs are matched by sort order.")
    if not files:
        sys.exit(f"No images matched '{args.cam0_glob}'. Pull them from the Rock first:\n"
                 f"  scp -r radxa@rock.local:~/calib0 images-cam0\n"
                 f"  scp -r radxa@rock.local:~/calib1 images-cam1")
    os.makedirs(args.out, exist_ok=True)

    print(f"Found {len(files)} " + ("images (single camera)" if args.single
                                    else "cam0/cam1 pairs"))
    print(f"Board: {args.squares_x}x{args.squares_y}, square={args.square_mm}mm, "
          f"marker={args.marker_mm}mm, dict={args.dict}")
    print("(If detection is poor, confirm these match your PRINTED board.)")
    print("Model: fisheye (equidistant)")

    board, detector = build_board(args)
    records, size = detect_all(files, detector, args, cam1_files=cam1_files)
    if size is None:
        sys.exit("No readable images found.")

    if args.use_intrinsics:
        KL, dL = load_intrinsics(args.use_intrinsics, "cam0")
        KR, dR = load_intrinsics(args.use_intrinsics, "cam1")
        print(f"\nLoaded intrinsics from {args.use_intrinsics}/ (skipping intrinsic calc).")
        ext = calibrate_extrinsics(records, board, size, KL, dL, KR, dR, args, args.out)
        if ext:
            print(f"\n=== SUMMARY ===\n  baseline : {ext['baseline_mm']:.2f} mm"
                  f"\n  toe-in   : {ext['toe_in_angle_deg']:.2f} deg")
        return

    if args.single:
        calibrate_intrinsics("single", records, board, size, args, args.out)
        return

    cam0, KL, dL = calibrate_intrinsics("cam0", records, board, size, args, args.out)
    cam1, KR, dR = calibrate_intrinsics("cam1", records, board, size, args, args.out)

    if cam0 is None or cam1 is None:
        print("\n! Skipping extrinsics - both cameras need valid intrinsics first.")
        return

    ext = calibrate_extrinsics(records, board, size, KL, dL, KR, dR, args, args.out)

    print("\n=== SUMMARY ===")
    print(f"  cam0 FOV : {cam0['fov_horizontal_deg']:.1f} deg H")
    print(f"  cam1 FOV : {cam1['fov_horizontal_deg']:.1f} deg H")
    if ext:
        print(f"  baseline : {ext['baseline_mm']:.2f} mm")
        print(f"  toe-in   : {ext['toe_in_angle_deg']:.2f} deg")
        print("  Stereo overlap ~= min(FOV) - toe-in. Widen the baseline for more depth "
              "resolution; increase toe-in for more overlap (at the cost of far-field).")


if __name__ == "__main__":
    main()
