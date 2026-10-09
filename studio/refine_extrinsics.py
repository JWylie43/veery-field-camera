#!/usr/bin/env python3
"""
refine_extrinsics.py - check / correct the stereo rotation from real footage (run on your Mac)

The ChArUco solve (calibrate.py) pins the camera geometry, but a fraction of a
degree of tilt or roll between the two cameras is enough to misalign the stitch
vertically, and the mount can settle by that much between sessions. This tool
measures that misalignment on any take and, with --apply, removes it.

It only uses PARALLAX-FREE measurements, so nothing that depends on where the rig
stands goes into the calibration (that stays with --shift-top/--shift-bottom):
  * tilt (pitch): the vertical offset AT THE SEAM - the baseline is perpendicular
    to that viewing direction, so no distance shifts it vertically there;
  * roll: how the vertical offset changes ACROSS the overlap, taken at the FAR
    rows (near rows add a parallax term that grows toward the camera);
  * yaw: the horizontal offset on the far field (top of the ground) - parallax
    shrinks to ~0 there. --keep-yaw leaves yaw alone.

Usage (on the Mac, from the repo root, with the repo venv - or from Studio's
takes list, which runs --align for you):
    .venv/bin/python studio/refine_extrinsics.py --align  ~/Desktop/takes/take_TS_cam0.mkv
    .venv/bin/python studio/refine_extrinsics.py --check  ~/Desktop/takes/take_TS_cam0.mkv
    .venv/bin/python studio/refine_extrinsics.py --apply  ~/Desktop/takes/take_TS_cam0.mkv

--align   (the normal one) solve this take's correction and write it, with the
          measured shear, to take_TS.align.json next to the take. The stitcher and
          Studio use that file automatically for this take; other takes keep using
          the base calibration (stereo_extrinsics.json), which is not touched.
--check   measure and report only (nothing is written); uses the take's
          .align.json if there is one (--base: measure against the base instead)
--apply   solve and write the correction into the BASE calibration instead (with
          a backup) - for when the rig has settled for good
Either file of the pair works (the partner _cam0/_cam1 is found next to it).
The two files are paired the way the stitcher pairs them (--pair-offset auto: exact
from the capture timestamps on shared-clock takes), so both measure the same frames.
Needs ffmpeg/ffprobe on PATH and a daytime take with textured ground in the overlap.

How it measures: a few frame pairs spread through the take are decoded (with the
files' own colour tags), warped onto the stitcher's cylinder with the current
calibration (the same maths as the stitcher), and small textured patches of the
overlap are matched between the two cameras (phase correlation on high-passed
images). The solve is a few Newton steps on three small rotations of the right
camera about axes at the seam. The geometry lives in rig.py (shared with Studio's
stitch preview).
"""
import argparse, json, math, os, shutil, sys, time
import numpy as np
import cv2

from rig import (CALIB_DIR, Rig, align_path, decode, load_calib, pair_offset, pair_paths, probe,
                 skips, take_name)


class MeasureError(Exception):
    pass


def highpass(img):
    g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY).astype(np.float32)
    return g - cv2.GaussianBlur(g, (0, 0), 6)


# ---------------------------------------------------------------- measurement
def measure(rig, pairs, R_lr):
    """Offsets of the right camera's picture vs the left's, on the canvas.
    pairs: list of (left high-passed canvas band, right BGR source frame)."""
    rx, ry, okR = rig._map(rig.KR, rig.DR, R_lr, np.arange(rig.X0, rig.X1), np.arange(rig.OH))
    both = rig.okL & okR
    sc = rig.seam - rig.X0
    pts = []
    win = cv2.createHanningWindow((320, 160), cv2.CV_32F)
    for Lh, Rsrc in pairs:
        Rh = highpass(cv2.remap(Rsrc, rx, ry, cv2.INTER_LINEAR))
        for y0 in range(300, rig.OH - 300 - 160, 64):
            for xc in range(sc - 700, sc + 701, 140):
                x0 = xc - 160
                if x0 < 0 or x0 + 320 > both.shape[1] or not both[y0:y0 + 160, x0:x0 + 320].all():
                    continue
                (dx, dy), resp = cv2.phaseCorrelate(Rh[y0:y0 + 160, x0:x0 + 320], Lh[y0:y0 + 160, x0:x0 + 320], win)
                if resp > 0.08:
                    pts.append((xc - sc, y0 + 80, dx, dy))
    p = np.array(pts, float)
    near = np.abs(p[:, 0]) <= 140 if len(p) else np.zeros(0, bool)     # patches on/next to the seam column
    if len(p) < 30 or near.sum() < 8:
        raise MeasureError('too little texture in the overlap to measure (%d patches, %d at the seam) - use a '
                           'daytime take with ground/grass between the cameras' % (len(p), int(near.sum())))
    rtop = p[:, 1].min()
    # vertical: dy = a + u*(s0 + c*(row - far row)), u = column offset from the seam in 1000s of px.
    # a = offset at the seam; s0 = across-overlap tilt at the FAR rows (parallax-free).
    u = p[:, 0] / 1000.0
    A = np.c_[np.ones(len(p)), u, u * (p[:, 1] - rtop) / 1000.0]
    keep = np.ones(len(p), bool)
    for _ in range(3):
        coef = np.linalg.lstsq(A[keep], p[keep, 3], rcond=None)[0]
        keep = np.abs(p[:, 3] - A @ coef) < 3
    resid = float(np.abs(p[keep, 3] - A[keep] @ coef).std())
    # horizontal offset down the seam: per row, the median over the seam patches and frames
    s = p[near]
    rows = np.unique(s[:, 1])
    dxr = np.array([np.median(s[s[:, 1] == r, 2]) for r in rows])
    dyr = np.array([np.median(s[s[:, 1] == r, 3]) for r in rows])
    nfar = max(2, len(rows) // 5)
    far_dx = float(np.median(dxr[:nfar]))
    # shear as the stitcher defines it (full-canvas rows, applied to the right camera's own rows)
    shift_y = float(np.median(dyr))
    ys = rows - shift_y
    k_keep = np.ones(len(rows), bool)
    for _ in range(3):
        k, a0 = np.polyfit(ys[k_keep], dxr[k_keep], 1)
        k_keep = np.abs(dxr - (a0 + k * ys)) < 3
    return dict(dy_seam=float(coef[0]), tilt_far=float(coef[1]), parallax=float(coef[2]),
                far_dx=far_dx, resid=resid, n=int(keep.sum()), shift_top=float(a0),
                shift_bottom=float(a0 + k * (rig.OH - 1)), shift_y=shift_y,
                far_rows=(int(rows[0]), int(rows[nfar - 1])))


def report(m, label):
    print('  %s' % label)
    print('    vertical offset at the seam     %+6.2f px' % m['dy_seam'])
    print('    vertical tilt across overlap    %+6.2f px per 1000 px  (far rows)' % m['tilt_far'])
    print('    far-field horizontal offset     %+6.2f px  (rows %d-%d)' % (m['far_dx'], *m['far_rows']))
    print('    measured on %d patches, vertical fit residual %.2f px' % (m['n'], m['resid']))
    print('    shear for this rig position:    --shift-top %.1f --shift-bottom %.1f  (vertical %+.1f px)'
          % (m['shift_top'], m['shift_bottom'], m['shift_y']))


def needs_fix(m):
    return abs(m['dy_seam']) > 1.0 or abs(m['tilt_far']) > 2.0


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--check', action='store_true', help='measure and report only')
    g.add_argument('--align', action='store_true',
                   help="solve this take's correction + shear and write take_TS.align.json (base untouched)")
    g.add_argument('--apply', action='store_true', help='solve, back up and update the base stereo_extrinsics.json')
    ap.add_argument('take', help='either file of the pair (take_..._cam0.mkv or _cam1.mkv)')
    ap.add_argument('--calib-dir', default=CALIB_DIR,
                    help='folder with the calibration JSONs (default: the repo\'s calibration/)')
    ap.add_argument('--frames', type=int, default=4, help='frame pairs to measure, spread through the take')
    ap.add_argument('--pair-offset', default='auto',
                    help='frame offset between the files, as the stitcher\'s --pair-offset: >0 skips '
                         'cam1 frames, <0 cam0 frames. auto (default): from the capture timestamps')
    ap.add_argument('--keep-yaw', action='store_true', help='only correct tilt and roll')
    ap.add_argument('--base', action='store_true', help="--check: ignore the take's .align.json")
    a = ap.parse_args()

    pp = pair_paths(os.path.expanduser(a.take))
    if not pp:
        sys.exit('ERROR: %s is not a _cam0/_cam1 take file' % a.take)
    f0, f1 = pp
    for f in (f0, f1):
        if not os.path.exists(f):
            sys.exit('ERROR: missing %s' % f)
    i0, i1 = probe(f0), probe(f1)
    if (i0['w'], i0['h']) != (i1['w'], i1['h']):
        sys.exit('ERROR: the two files differ in size')
    try:
        K0, D0, K1, D1, R, ext = load_calib(a.calib_dir)
    except (OSError, ValueError) as e:
        sys.exit('ERROR: calibration: %s' % e)
    if a.pair_offset == 'auto':
        po = pair_offset(f0, f1, i0, i1)
        offset = po['offset']
        print('Pair offset: %d frames (%s)' % (offset, po['detail']))
    else:
        offset = int(a.pair_offset)
        print('Pair offset: %d frames (given)' % offset)
    skip0, skip1 = skips(offset)
    ap_path = align_path(f0)
    calib_label = os.path.join(a.calib_dir, 'stereo_extrinsics.json')
    if a.check and not a.base and os.path.exists(ap_path):       # check what the stitcher will use
        R = np.array(json.load(open(ap_path))['rotation_matrix'], float)
        calib_label = ap_path + ' (this take)'
    try:
        rig = Rig(K0, D0, K1, D1, R, i0['w'], i0['h'])
    except ValueError as e:
        sys.exit('ERROR: ' + str(e))
    nfr = min(i0['frames'] - skip0, i1['frames'] - skip1)
    picks = [int(nfr * (k + 1) / (a.frames + 1)) for k in range(a.frames)]
    print('Take: %s (+ partner), %d frames; measuring frames %s' % (os.path.basename(f0), nfr, picks))
    print('Calibration: %s' % calib_label)
    t0 = time.time()
    pairs = []
    for n in picks:
        a0, a1 = decode(f0, i0, n + skip0), decode(f1, i1, n + skip1)
        if a0 is None or a1 is None:
            sys.exit('ERROR: could not decode frame %d of the pair' % n)
        L, Rs = (a1, a0) if rig.swap else (a0, a1)
        Lh = highpass(cv2.remap(L, rig.lx, rig.ly, cv2.INTER_LINEAR))
        pairs.append((Lh, Rs))
    try:
        before = measure(rig, pairs, rig.R0)
    except MeasureError as e:
        sys.exit('ERROR: ' + str(e))
    report(before, 'Current calibration:')
    verdict = 'needs --align' if needs_fix(before) else 'aligned'
    print('  -> %s' % verdict)
    if a.check:
        return

    # ---- solve: targets are zero vertical offset, zero far tilt, zero far-field horizontal
    def targets(m):
        return np.array([m['dy_seam'], m['tilt_far'], m['far_dx']])
    nvar = 2 if a.keep_yaw else 3
    rows = [0, 1] if a.keep_yaw else [0, 1, 2]
    step = math.radians(0.15)

    def jacobian(x0, m0):
        J = np.zeros((3, nvar))
        for k in range(nvar):
            d = x0.copy()
            d[k] += step
            J[:, k] = (targets(measure(rig, pairs, rig.corrected(*d))) - targets(m0)) / step
        return J

    def done(tv):
        return abs(tv[0]) < 0.15 and abs(tv[1]) < 0.3 and (a.keep_yaw or abs(tv[2]) < 0.2)

    def cost(tv):   # px-ish: seam offset, tilt per 1000 px, far-field horizontal
        return abs(tv[0]) + abs(tv[1]) + (0 if a.keep_yaw else abs(tv[2]))

    x = np.zeros(3)
    m = before
    best = (cost(targets(m)), x.copy(), m)
    J = jacobian(x, m) if not done(targets(m)) else None
    for it in range(10):
        tv = targets(m)
        print('  iter %d: pitch %+.3f roll %+.3f yaw %+.3f deg -> seam %+.2f px, tilt %+.2f, far-dx %+.2f'
              % (it, *np.degrees(x), *tv))
        if done(tv):
            break
        dx = np.linalg.lstsq(J[rows], -tv[rows], rcond=None)[0]
        x_try = x.copy()
        x_try[:nvar] += dx
        try:
            m_try = measure(rig, pairs, rig.corrected(*x_try))
        except MeasureError:
            print('  (a step left the measurable overlap; keeping the best result so far)')
            break
        x, m = x_try, m_try
        if cost(targets(m)) < best[0]:
            best = (cost(targets(m)), x.copy(), m)
        if it == 0:                      # re-linearise near the answer (roll couples into far-dx)
            J = jacobian(x, m)
    _, x, m = best
    if not done(targets(m)):
        print('  note: stopped short of full convergence; the best result is used')
    after = m
    R_new = rig.file_rotation(rig.corrected(*x))
    corr = {'pitch': round(math.degrees(x[0]), 4), 'roll': round(math.degrees(x[1]), 4),
            'yaw': round(math.degrees(x[2]), 4)}
    if a.align:
        out = {
            'version': 1,
            'what': 'Per-take camera alignment. The stitcher and Studio use rotation_matrix in place of the '
                    'base calibration for this take, and shift_top/shift_bottom as the default shear. '
                    'Made by studio/refine_extrinsics.py --align from parallax-free measurements; '
                    'the shear is the measured parallax at the seam for this rig position.',
            'take': take_name(f0),
            'date': time.strftime('%Y-%m-%d %H:%M'),
            'frames_measured': picks,
            'pair_offset': offset,
            'rotation_matrix': R_new.tolist(),
            'shift_top': round(after['shift_top'], 1),
            'shift_bottom': round(after['shift_bottom'], 1),
            'corrections_from_base_deg': corr,
            'base_calibration': calib_label,
            'base_rotation_matrix': ext['rotation_matrix'],
            'before': {k: round(before[k], 3) for k in ('dy_seam', 'tilt_far', 'far_dx')},
            'after': {k: round(after[k], 3) for k in ('dy_seam', 'tilt_far', 'far_dx')},
        }
        with open(ap_path + '.tmp', 'w') as f:
            json.dump(out, f, indent=2)
        os.replace(ap_path + '.tmp', ap_path)
        print('Corrections from the base: pitch %+.3f, roll %+.3f, yaw %+.3f deg (%.0f s)'
              % (*np.degrees(x), time.time() - t0))
        report(after, 'With this take\'s alignment:')
        print('Wrote %s' % ap_path)
        print('The stitcher and Studio now use it for this take: shear %.1f / %.1f is filled in automatically.'
              % (out['shift_top'], out['shift_bottom']))
        return

    # ---- write (with a backup and a history entry)
    path = os.path.join(a.calib_dir, 'stereo_extrinsics.json')
    stamp = time.strftime('%Y%m%d-%H%M%S')
    shutil.copy2(path, path + '.bak-' + stamp)
    e = cv2.RQDecomp3x3(R_new)[0]
    hist = ext.get('refinement_history', [])
    hist.append({
        'date': time.strftime('%Y-%m-%d %H:%M'),
        'take': os.path.basename(f0),
        'frames': picks,
        'corrections_deg': corr,
        'before': {k: round(before[k], 3) for k in ('dy_seam', 'tilt_far', 'far_dx')},
        'after': {k: round(after[k], 3) for k in ('dy_seam', 'tilt_far', 'far_dx')},
        'previous_rotation_matrix': ext['rotation_matrix'],
    })
    ext['rotation_matrix'] = R_new.tolist()
    ext['rotation_euler_deg'] = {'pitch_x': round(float(e[0]), 3), 'yaw_toe_y': round(float(e[1]), 3),
                                 'roll_z': round(float(e[2]), 3)}
    ext['toe_in_angle_deg'] = round(abs(float(e[1])), 3)
    ext['total_rotation_deg'] = round(float(np.degrees(np.linalg.norm(cv2.Rodrigues(R_new)[0]))), 3)
    ext['refinement_history'] = hist
    with open(path + '.tmp', 'w') as f:
        json.dump(ext, f, indent=2)
    os.replace(path + '.tmp', path)
    print('Corrections: pitch %+.3f, roll %+.3f, yaw %+.3f deg (%.0f s)'
          % (*np.degrees(x), time.time() - t0))
    report(after, 'After:')
    print('Wrote %s (previous version: %s)' % (path, os.path.basename(path) + '.bak-' + stamp))


if __name__ == '__main__':
    main()
