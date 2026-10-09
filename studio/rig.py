"""
rig.py - the camera rig's take files, calibration and stitch geometry, in Python.

Shared by refine_extrinsics.py (alignment) and the Studio server (the stitch
page's preview). The geometry is the stitcher's (native/stitch_pipeline.cpp)
line for line, so what the preview shows is what StitchPipeline renders:
  * a cylinder canvas in the LEFT camera's frame, radius = its focal length;
  * equidistant (Kannala-Brandt) fisheye projection into each camera;
  * cam1 is the left camera when the extrinsic yaw is negative (R is then
    transposed), exactly as the stitcher swaps.
Frame pairing follows the stitcher's --pair-offset convention (cam0/cam1 terms):
offset > 0 skips that many frames of cam1, < 0 skips frames of cam0.
"""
import json, math, os, re, subprocess
import numpy as np
import cv2

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CALIB_DIR = os.path.join(REPO, 'calibration')
# Written into every take by the recorder (rock5t-camera/recorder/server.py). Matched
# anywhere in the tag, so takes recorded under the tag's older, longer name count too.
SHARED_CLOCK_TAG = 'shared-clock'

TAKE_RE = re.compile(r'^(take_[A-Za-z0-9_-]+?)_cam([01])\.(mkv|mp4|mov)$')


# ---------------------------------------------------------------- take files
def pair_paths(path):
    """Either file of a pair -> (cam0 path, cam1 path). None if not a _cam0/_cam1 name."""
    for tag in ('_cam0.', '_cam1.'):
        i = path.rfind(tag)
        if i >= 0:
            pre, post = path[:i], path[i + 6:]
            return pre + '_cam0.' + post, pre + '_cam1.' + post
    return None


def align_path(f0):
    """take_TS_cam0.mkv -> take_TS.align.json (next to it)."""
    i = f0.rfind('_cam0.')
    return f0[:i] + '.align.json'


def take_name(f0):
    return os.path.basename(f0)[:os.path.basename(f0).rfind('_cam0.')]


# ---------------------------------------------------------------- probing + decoding
def _run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def probe(path):
    """Size, rate, colour tags and frame count of a camera file."""
    out = _run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                'stream=width,height,r_frame_rate,nb_frames,color_range,color_space,start_time',
                '-of', 'json', path])
    s = json.loads(out)['streams'][0]
    num, den = s.get('r_frame_rate', '30/1').split('/')
    frames = int(s['nb_frames']) if str(s.get('nb_frames', '')).isdigit() else 0
    if frames <= 0:
        c = _run(['ffprobe', '-v', 'error', '-count_packets', '-select_streams', 'v:0',
                  '-show_entries', 'stream=nb_read_packets', '-of', 'csv=p=0', path])
        frames = int(c.strip() or 0)
    return dict(w=s['width'], h=s['height'], fps=float(num) / float(den), frames=frames,
                start=float(s.get('start_time') or 0.0),
                range='pc' if s.get('color_range') == 'pc' else 'tv',
                matrix='bt601' if s.get('color_space') in ('smpte170m', 'bt470bg') else 'bt709')


def decode(path, info, n):
    """Frame n (exact) as BGR, converted with the file's own range + matrix. None if unreadable.
    ffmpeg's input -ss is frame-accurate when decoding; the half-frame back-off makes the
    first frame kept n rather than the one after it."""
    t = max(0.0, (n - 0.5) / info['fps'])
    vf = ('scale=in_range=%s:in_color_matrix=%s:flags=accurate_rnd+full_chroma_int+full_chroma_inp,'
          'format=bgr24' % (info['range'], info['matrix']))
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-ss', '%.6f' % t, '-i', path, '-frames:v', '1',
                          '-vf', vf, '-f', 'rawvideo', '-pix_fmt', 'bgr24', '-'], capture_output=True).stdout
    if len(raw) != info['w'] * info['h'] * 3:
        return None
    return np.frombuffer(raw, np.uint8).reshape(info['h'], info['w'], 3)


def frame_times(path):
    """Every frame's timestamp (s), sorted - read from the packets, nothing is decoded."""
    out = _run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'packet=pts_time',
                '-of', 'csv=p=0', path])
    ts = []
    for x in out.split():
        try:
            ts.append(float(x.strip().rstrip(',')))
        except ValueError:
            pass
    return sorted(ts)


def timing_report(path, fps):
    """Frame count + gaps longer than 1.5 frame intervals (missing time: everything after
    a gap is shifted against the other camera)."""
    ts = frame_times(path)
    gaps = [(round(ts[i], 3), round((ts[i + 1] - ts[i]) * 1000, 1))
            for i in range(len(ts) - 1) if ts[i + 1] - ts[i] > 1.5 / fps]
    return dict(frames=len(ts), gaps=len(gaps), first_gaps=gaps[:5])


# ---------------------------------------------------------------- pairing
def has_shared_clock(path):
    return SHARED_CLOCK_TAG in _run(['ffprobe', '-v', 'error', '-show_entries', 'format_tags:stream_tags',
                                     '-of', 'default=nw=1', path])


def pair_offset(f0, f1, i0=None, i1=None):
    """Frame offset between the two files, as the stitcher's --pair-offset (cam0/cam1 terms).
    Shared-clock takes: exact, from the capture start timestamps. Older takes: estimated by
    cross-correlating per-frame brightness over the first seconds.
    Returns dict(offset, method, detail)."""
    i0 = i0 or probe(f0)
    i1 = i1 or probe(f1)
    if has_shared_clock(f0) and has_shared_clock(f1):
        frames = (i1['start'] - i0['start']) * i0['fps']        # cam1 starts this many frames later
        off = -int(round(frames))                               # <0 skips that many frames of cam0
        resid = frames - round(frames)
        detail = 'capture timestamps: cam0 starts %.3fs, cam1 %.3fs (off-grid residual %+.2f frame)' % (
            i0['start'], i1['start'], resid)
        if abs(resid) > 0.25:
            detail += ' - residual > 1/4 frame, the cameras do not look genlocked'
        return dict(offset=off, method='timestamps', detail=detail)
    off, corr, margin = estimate_offset(f0, f1, i0['fps'])
    return dict(offset=off, method='brightness',
                detail='brightness correlation %.3f, margin %.3f%s' % (
                    corr, margin, ' (weak - cameras free-running?)' if corr < 0.5 else ''))


def _luma_series(path, count):
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-frames:v', str(count),
                          '-vf', 'scale=32:18:flags=area,format=gray', '-f', 'rawvideo', '-'],
                         capture_output=True).stdout
    n = len(raw) // (32 * 18)
    return np.frombuffer(raw[:n * 32 * 18], np.uint8).reshape(n, -1).mean(1)


def estimate_offset(f0, f1, fps, max_shift=15, seconds=5.0):
    """(offset, correlation, margin). A best match of cam0 frame i+s with cam1 frame i
    means cam0 started s frames early, so s frames of cam0 are skipped: offset = -s."""
    want = int(seconds * fps) + 2 * max_shift
    a, b = _luma_series(f0, want), _luma_series(f1, want)
    n = min(len(a), len(b))
    if n < 30:
        return 0, 0.0, 0.0

    def score(s):
        x, y = (a[s:n], b[:n - s]) if s >= 0 else (a[:n + s], b[-s:n])
        if len(x) < 20 or x.std() == 0 or y.std() == 0:
            return -2.0
        return float(np.corrcoef(x, y)[0, 1])
    scored = sorted(((score(s), s) for s in range(-max_shift, max_shift + 1)), reverse=True)
    best, s = scored[0]
    runner = next((v for v, t in scored[1:] if abs(t - s) > 1), -2.0)
    return -s, best, best - runner


def skips(offset):
    """Frames skipped at the start of (cam0, cam1) for a pair offset."""
    return max(0, -offset), max(0, offset)


# ---------------------------------------------------------------- calibration
def load_calib(d=CALIB_DIR):
    def intr(name):
        j = json.load(open(os.path.join(d, name + '_intrinsics.json')))
        if j.get('model') != 'fisheye':
            raise ValueError('%s is not a fisheye calibration' % name)
        return np.array(j['camera_matrix'], float), np.array(j['distortion_coefficients'], float).ravel()
    K0, D0 = intr('cam0')
    K1, D1 = intr('cam1')
    ext = json.load(open(os.path.join(d, 'stereo_extrinsics.json')))
    return K0, D0, K1, D1, np.array(ext['rotation_matrix'], float), ext


def take_alignment(f0):
    """The take's .align.json (dict) if it has one, else None."""
    p = align_path(f0)
    if not os.path.exists(p):
        return None
    try:
        j = json.load(open(p))
        j['_path'] = p
        return j
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------- the stitcher's geometry
class Rig:
    """Cylinder canvas exactly as stitch_pipeline.cpp builds it (left camera = canvas
    frame; with a negative yaw the stitcher swaps to cam1-left and uses R^T).
    band=True also prepares the overlap band refine_extrinsics.py measures in."""

    def __init__(self, K0, D0, K1, D1, R, w, h, band=True):
        self.swap = math.atan2(R[2, 0], R[2, 2]) < 0
        if self.swap:
            K0, D0, K1, D1, R = K1, D1, K0, D0, R.T
        self.KL, self.DL, self.KR, self.DR, self.R0 = K0, D0, K1, D1, R      # R0: left->right
        self.w, self.h = w, h
        self.fcyl = K0[0, 0]
        yaw = math.atan2(R[2, 0], R[2, 2])
        pad = math.radians(3)
        self.tmin = min(-w / (2 * K0[0, 0]), yaw - w / (2 * K1[0, 0])) - pad
        tmax = max(w / (2 * K0[0, 0]), yaw + w / (2 * K1[0, 0])) + pad
        self.OW = min(int((tmax - self.tmin) * self.fcyl), 12000)
        self.OH = min(int(2 * math.tan(h / (2 * K0[1, 1])) * self.fcyl), 4000)
        if not band:
            return
        # overlap band + seam (median overlap column), found on a coarse grid
        xs = np.arange(0, self.OW, 4)
        ys = np.arange(0, self.OH, 8)
        okL = self._map(self.KL, self.DL, np.eye(3), xs, ys)[2]
        okR = self._map(self.KR, self.DR, self.R0, xs, ys)[2]
        cols = xs[(okL & okR).any(0)]
        if len(cols) == 0:
            raise ValueError('the cameras do not overlap with this calibration')
        self.seam = int(cols[len(cols) // 2])
        self.X0 = max(0, int(cols[0]) - 8)
        self.X1 = min(self.OW, int(cols[-1]) + 8)
        ts = self.tmin + self.seam / self.fcyl
        self.ax_pitch = np.array([math.cos(ts), 0.0, -math.sin(ts)])   # horizontal, across the seam view
        self.ax_roll = np.array([math.sin(ts), 0.0, math.cos(ts)])     # the seam view direction
        self.ax_yaw = np.array([0.0, 1.0, 0.0])                        # canvas vertical
        xb, yb = np.arange(self.X0, self.X1), np.arange(self.OH)
        self.lx, self.ly, self.okL = self._map(self.KL, self.DL, np.eye(3), xb, yb)

    def _map(self, K, D, Rm, xs, ys):
        TH, HV = np.meshgrid(self.tmin + xs / self.fcyl, (ys - self.OH / 2) / self.fcyl)
        d = np.stack([np.sin(TH), HV, np.cos(TH)], -1) @ Rm.T
        z = d[..., 2]
        ok = z > 1e-6
        z = np.where(ok, z, 1)
        xn, yn = d[..., 0] / z, d[..., 1] / z
        r = np.sqrt(xn ** 2 + yn ** 2)
        t = np.arctan(r)
        t2 = t * t
        s = np.where(r > 1e-12, t * (1 + D[0] * t2 + D[1] * t2 ** 2 + D[2] * t2 ** 3 + D[3] * t2 ** 4)
                     / np.maximum(r, 1e-12), 1)
        u = K[0, 0] * xn * s + K[0, 2]
        v = K[1, 1] * yn * s + K[1, 2]
        ok &= (u >= 0) & (u < self.w) & (v >= 0) & (v < self.h)
        return np.where(ok, u, -1).astype(np.float32), np.where(ok, v, -1).astype(np.float32), ok

    def full_maps(self, rows=128):
        """Remap tables over the whole canvas for both cameras (the stitcher's
        buildStitchMaps), as fixed-point pairs for cv2.remap, plus the seam and overlap
        columns computed the way the stitcher does (a column overlaps when each camera
        is valid somewhere in it; the seam is the middle overlapping column)."""
        xs = np.arange(self.OW)
        maps = {}
        colL = np.zeros(self.OW, bool)
        colR = np.zeros(self.OW, bool)
        for side, K, D, Rm in (('L', self.KL, self.DL, np.eye(3)), ('R', self.KR, self.DR, self.R0)):
            mx = np.empty((self.OH, self.OW), np.float32)
            my = np.empty((self.OH, self.OW), np.float32)
            col = colL if side == 'L' else colR
            for y0 in range(0, self.OH, rows):
                ys = np.arange(y0, min(self.OH, y0 + rows))
                u, v, ok = self._map(K, D, Rm, xs, ys)
                mx[y0:y0 + len(ys)], my[y0:y0 + len(ys)] = u, v
                col |= ok.any(0)
            maps[side] = cv2.convertMaps(mx, my, cv2.CV_16SC2)
        cols = np.nonzero(colL & colR)[0]
        seam = int(cols[len(cols) // 2]) if len(cols) else self.OW // 2
        ox0 = int(cols[0]) if len(cols) else 0
        ox1 = int(cols[-1]) + 1 if len(cols) else self.OW
        return maps, seam, ox0, ox1

    def corrected(self, p, r, y):
        """Left->right rotation after the small corrections (radians)."""
        rot = lambda ax, a: cv2.Rodrigues((ax * a).reshape(3, 1))[0]
        return self.R0 @ rot(self.ax_pitch, p) @ rot(self.ax_roll, r) @ rot(self.ax_yaw, y)

    def file_rotation(self, R_lr):
        """Back to stereo_extrinsics.json terms (cam0 -> cam1)."""
        return R_lr.T if self.swap else R_lr
