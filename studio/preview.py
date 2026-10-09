"""
preview.py - the stitch page's preview: one take's two cameras warped onto the
stitcher's cylinder canvas, at any frame.

Each camera is warped onto the full canvas
(un-rotated, un-sheared) and the page applies shear, rotation and the crop box on
top, live. The canvas, the warp and the frame pairing are the stitcher's (see
rig.py), so the box drawn here is the box StitchPipeline renders.
"""
import os, threading
from concurrent.futures import ThreadPoolExecutor
import cv2
import numpy as np
import rig

JPEG_QUALITY = 85


class Preview:
    def __init__(self, take, f0, f1):
        self.take, self.f0, self.f1 = take, f0, f1
        self.i0, self.i1 = rig.probe(f0), rig.probe(f1)
        if (self.i0['w'], self.i0['h']) != (self.i1['w'], self.i1['h']):
            raise ValueError('the two camera files differ in size')
        K0, D0, K1, D1, R, self.ext = rig.load_calib()
        self.align = rig.take_alignment(f0)
        self.align_key = _mtime(rig.align_path(f0))
        if self.align:
            R = np.array(self.align['rotation_matrix'], float)
        self.R_used = R                                   # cam0 -> cam1, what the stitcher will use
        self.rig = rig.Rig(K0, D0, K1, D1, R, self.i0['w'], self.i0['h'], band=False)
        self.maps, self.seam, self.ox0, self.ox1 = self.rig.full_maps()
        self.pairing = rig.pair_offset(f0, f1, self.i0, self.i1)
        self.skip0, self.skip1 = rig.skips(self.pairing['offset'])
        self.total = max(1, min(self.i0['frames'] - self.skip0, self.i1['frames'] - self.skip1))
        self.lock = threading.Lock()
        self.cache = {}                                    # frame -> (left jpeg, right jpeg); a few kept
        self.pool = ThreadPoolExecutor(2)

    def info(self):
        a = self.align
        return dict(take=self.take, cam0=os.path.basename(self.f0), cam1=os.path.basename(self.f1),
                    ow=self.rig.OW, oh=self.rig.OH, seam=self.seam, ox0=self.ox0, ox1=self.ox1,
                    total=self.total, fps=self.i0['fps'], swap=self.rig.swap,
                    pairing=self.pairing,
                    align=None if not a else dict(file=os.path.basename(a['_path']), date=a.get('date'),
                                                  shift_top=a.get('shift_top', 0), shift_bottom=a.get('shift_bottom', 0),
                                                  corrections=a.get('corrections_from_base_deg')),
                    calibration=os.path.join(rig.CALIB_DIR, 'stereo_extrinsics.json'))

    def frame(self, n):
        """(left jpeg, right jpeg) of pair frame n, each camera warped onto the canvas."""
        n = max(0, min(self.total - 1, int(n)))
        with self.lock:                                    # the page asks for L and R at once: decode once
            if n in self.cache:
                return self.cache[n]
            fa = self.pool.submit(rig.decode, self.f0, self.i0, n + self.skip0)
            fb = self.pool.submit(rig.decode, self.f1, self.i1, n + self.skip1)
            a0, a1 = fa.result(), fb.result()
            if a0 is None or a1 is None:
                raise ValueError('cannot decode frame %d' % n)
            L, R = (a1, a0) if self.rig.swap else (a0, a1)
            out = []
            for img, side in ((L, 'L'), (R, 'R')):
                m1, m2 = self.maps[side]
                w = cv2.remap(img, m1, m2, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)
                out.append(cv2.imencode('.jpg', w, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])[1].tobytes())
            if len(self.cache) >= 4:
                self.cache.pop(next(iter(self.cache)))
            self.cache[n] = tuple(out)
            return self.cache[n]


def _mtime(p):
    try:
        return os.path.getmtime(p)
    except OSError:
        return None


class Previews:
    """One preview kept in memory at a time (its tables are a few hundred MB); rebuilt
    when another take is opened or the take's alignment file changes."""

    def __init__(self):
        self.lock = threading.Lock()
        self.cur = None

    def get(self, take, f0, f1):
        with self.lock:
            p = self.cur
            if p and p.take == take and p.f0 == f0 and p.align_key == _mtime(rig.align_path(f0)):
                return p
            self.cur = None                                  # free the old tables first
            self.cur = Preview(take, f0, f1)
            return self.cur
