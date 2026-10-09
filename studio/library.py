"""
library.py - what is in the takes folder: camera pairs and stitched videos.

  * a camera PAIR is take_<name>_cam0.<ext> + take_<name>_cam1.<ext> (both present);
  * a STITCHED video is an .mp4/.mov whose metadata carries Studio's stitch record
    (JSON in the "comment" tag, {"kind": "stitch", ...}) - only those open in the editor.
Probing is cached per file (path, size, mtime), so a rescan costs a directory listing.
The frame-timing check (every frame's timestamp, for gaps) reads the whole file's
packets, so it runs in the background and appears in the list when ready.
"""
import json, os, re, subprocess, threading
import rig

VIDEO_EXT = ('.mp4', '.mov', '.m4v')
STITCH_TAG = 'stitch'


def _stat_key(path):
    st = os.stat(path)
    return (st.st_size, st.st_mtime)


class Library:
    def __init__(self, folder):
        self.folder = os.path.abspath(os.path.expanduser(folder))
        self.lock = threading.Lock()
        self._probe = {}          # path -> (key, info)
        self._meta = {}           # path -> (key, stitch record or None)
        self._timing = {}         # path -> (key, report)
        self._timing_busy = set()

    # ------------------------------------------------------------ names -> files
    def pair(self, take):
        """(cam0, cam1) paths for a take name, or None. Names are matched against the
        folder listing - never joined from user input - so nothing outside it is reachable."""
        if not re.fullmatch(r'take_[A-Za-z0-9_-]+', take or ''):
            return None
        found = {}
        for f in os.listdir(self.folder):
            m = rig.TAKE_RE.match(f)
            if m and m.group(1) == take:
                found.setdefault(m.group(2), os.path.join(self.folder, f))
        return (found['0'], found['1']) if '0' in found and '1' in found else None

    def video(self, name):
        """Path of a video in the folder by its file name, or None."""
        if not name or '/' in name or '\\' in name or name.startswith('.'):
            return None
        if not name.lower().endswith(VIDEO_EXT) or rig.TAKE_RE.match(name):
            return None
        p = os.path.join(self.folder, name)
        return p if os.path.isfile(p) else None

    def out_path(self, name, exts=('.mp4',)):
        """Validated output file path in the folder for a bare file name, or None."""
        if not name or '/' in name or '\\' in name or name.startswith('.') or not name.lower().endswith(exts):
            return None
        if rig.TAKE_RE.match(name):
            return None
        return os.path.join(self.folder, name)

    # ------------------------------------------------------------ cached probes
    def probe(self, path):
        key = _stat_key(path)
        with self.lock:
            c = self._probe.get(path)
            if c and c[0] == key:
                return c[1]
        info = probe_video(path)
        with self.lock:
            self._probe[path] = (key, info)
        return info

    def stitch_record(self, path):
        """The stitch record stored in a video's metadata, or None."""
        key = _stat_key(path)
        with self.lock:
            c = self._meta.get(path)
            if c and c[0] == key:
                return c[1]
        rec = read_stitch_record(path)
        with self.lock:
            self._meta[path] = (key, rec)
        return rec

    def timing(self, path, fps):
        """Frame-timing report if ready; starts it in the background otherwise."""
        key = _stat_key(path)
        with self.lock:
            c = self._timing.get(path)
            if c and c[0] == key:
                return c[1]
            if path in self._timing_busy:
                return None
            self._timing_busy.add(path)

        def work():
            try:
                rep = rig.timing_report(path, fps)
            except Exception as e:
                rep = dict(error=str(e))
            with self.lock:
                self._timing[path] = (key, rep)
                self._timing_busy.discard(path)
        threading.Thread(target=work, daemon=True).start()
        return None

    # ------------------------------------------------------------ the listing
    def scan(self):
        names = sorted(os.listdir(self.folder))
        pairs = {}
        for f in names:
            m = rig.TAKE_RE.match(f)
            if m:
                pairs.setdefault(m.group(1), {})[m.group(2)] = os.path.join(self.folder, f)
        takes, incomplete = [], []
        for take, cams in sorted(pairs.items(), reverse=True):
            if set(cams) != {'0', '1'}:
                incomplete.append(dict(take=take, have=[os.path.basename(p) for p in cams.values()]))
                continue
            takes.append(self.take_entry(take, cams['0'], cams['1']))
        videos = []
        for f in names:
            p = self.video(f)
            if not p:
                continue
            try:
                rec = self.stitch_record(p)
            except Exception:
                rec = None
            if not rec:
                continue
            info = self.probe(p)
            st = os.stat(p)
            videos.append(dict(name=f, size=st.st_size, mtime=st.st_mtime, info=info, stitch=rec,
                               project=os.path.exists(project_path(p))))
        videos.sort(key=lambda v: v['mtime'], reverse=True)
        return dict(folder=self.folder, takes=takes, incomplete=incomplete, videos=videos)

    def take_entry(self, take, f0, f1):
        i0, i1 = self.probe(f0), self.probe(f1)
        al = rig.take_alignment(f0)
        align = None
        if al:
            align = {k: al.get(k) for k in ('date', 'shift_top', 'shift_bottom', 'corrections_from_base_deg',
                                            'before', 'after', 'frames_measured', 'pair_offset')}
            align['file'] = os.path.basename(al['_path'])
        t0 = self.timing(f0, i0.get('fps') or 30)
        t1 = self.timing(f1, i1.get('fps') or 30)
        size = os.path.getsize(f0) + os.path.getsize(f1)
        return dict(take=take, cam0=os.path.basename(f0), cam1=os.path.basename(f1), size=size,
                    mtime=max(os.path.getmtime(f0), os.path.getmtime(f1)),
                    cam0_info=i0, cam1_info=i1, align=align,
                    timing=None if t0 is None or t1 is None else dict(cam0=t0, cam1=t1))


# ---------------------------------------------------------------- helpers
def probe_video(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                          'stream=width,height,r_frame_rate,nb_frames,codec_name,color_range:format=duration',
                          '-of', 'json', path], capture_output=True, text=True).stdout
    try:
        j = json.loads(out)
        s = j['streams'][0]
        num, den = s.get('r_frame_rate', '30/1').split('/')
        fps = float(num) / float(den) if float(den) else 30.0
        dur = float(j.get('format', {}).get('duration') or 0)
        nb = s.get('nb_frames')
        frames = int(nb) if str(nb or '').isdigit() else int(round(dur * fps))
        return dict(width=s.get('width'), height=s.get('height'), fps=fps, frames=frames, duration=dur,
                    codec=s.get('codec_name'), full_range=s.get('color_range') == 'pc', exact_frames=str(nb or '').isdigit())
    except (ValueError, KeyError, IndexError):
        return dict(error='unreadable')


def read_stitch_record(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format_tags=comment',
                          '-of', 'default=nw=1:nk=1', path], capture_output=True, text=True).stdout.strip()
    if not out.startswith('{'):
        return None
    try:
        rec = json.loads(out)
    except ValueError:
        return None
    # 'kind' names the record; the first stitches keyed it by the project's name instead,
    # so any key whose value is the stitch tag counts
    return rec if isinstance(rec, dict) and STITCH_TAG in rec.values() else None


def project_path(video):
    stem, _ = os.path.splitext(video)
    return stem + '.director.json'
