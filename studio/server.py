#!/usr/bin/env python3
"""
Veery Studio - one local web app for the takes folder: align, stitch, edit.

    studio/.venv/bin/python studio/server.py [--takes ~/Desktop/veery-takes] [--port 8100]
    (or double-click studio/studio.command)

Pages:
  /                 the takes folder: camera pairs (Align / Stitch) and stitched videos (Edit),
                    with every job's live status
  /stitch/<take>    one pair: preview any frame, set shear / rotation / crop box, stitch
  /edit/<video>     the virtual-camera editor (Director) for a stitched video
Heavy work runs as jobs - each one process, the same command you would type:
  align  -> studio/refine_extrinsics.py --align   (two at a time, the rest queue)
  stitch -> stitching/build/StitchPipeline        (one at a time)
  render -> stitching/build/Director --render     (one at a time)
The pages poll /api/jobs for progress; jobs keep running when a page is closed.
"""
import argparse, atexit, faulthandler, json, os, re, shlex, shutil, signal, sys, tempfile, threading, time
import traceback, webbrowser
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

import rig
from jobs import Busy, Job, JobManager, align_parser, render_parser, stitch_parser
from library import Library, project_path
from preview import Previews

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(HERE, 'static')
BIN_DIR = os.path.join(rig.REPO, 'stitching', 'build')
VERSION = 1
LOG_PATH = os.path.join(HERE, 'studio.log')
_logf = None


def log(msg):
    """One line in studio/studio.log (and the terminal): starts, stops and why, jobs, errors."""
    line = '%s  %s' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg)
    try:
        print(line, flush=True)
    except OSError:                       # the terminal window is gone; the file still works
        pass
    if _logf:
        try:
            _logf.write(line + '\n')
            _logf.flush()
        except OSError:
            pass


def open_log():
    global _logf
    if os.path.exists(LOG_PATH) and os.path.getsize(LOG_PATH) > (2 << 20):
        os.replace(LOG_PATH, LOG_PATH + '.1')
    _logf = open(LOG_PATH, 'a', buffering=1)
    faulthandler.enable(file=_logf)       # a hard crash (segfault) still leaves a trace here
    sys.excepthook = lambda t, v, tb: log('CRASH (main thread):\n' + ''.join(traceback.format_exception(t, v, tb)))
    threading.excepthook = lambda a: log('error in thread %s:\n%s' % (
        a.thread.name if a.thread else '?', ''.join(traceback.format_exception(a.exc_type, a.exc_value, a.exc_traceback))))
    atexit.register(lambda: log('process exiting'))


class Studio:
    def __init__(self, takes, stitcher, director):
        self.lib = Library(takes)
        self.stitcher, self.director = stitcher, director
        self.jobs = JobManager({'align': 2, 'stitch': 1, 'render': 1}, reject_when_busy=('stitch', 'render'))
        self.jobs.on_finish = lambda j: log('job %s %s%s' % (j.id, j.status, ': ' + j.error if j.error else ''))
        self.previews = Previews()
        self.runtime = tempfile.mkdtemp(prefix='veery-studio-')
        self.save_lock = threading.Lock()
        self.last_backup = {}

    def tools(self):
        """Which binaries exist, and whether the stitcher can record its settings in the video."""
        def has(p):
            return os.path.isfile(p) and os.access(p, os.X_OK)
        meta = False
        if has(self.stitcher):
            with open(self.stitcher, 'rb') as f:
                meta = b'--metadata-file' in f.read()
        return dict(stitcher=self.stitcher, stitcher_ok=has(self.stitcher), stitcher_metadata=meta,
                    director=self.director, director_ok=has(self.director))

    # ------------------------------------------------------------ jobs
    def start_align(self, takes):
        started, errors = [], []
        for take in takes:
            pair = self.lib.pair(take)
            if not pair:
                errors.append('%s: no such pair' % take)
                continue
            if self.jobs.active('align', take):
                errors.append('%s: already aligning' % take)
                continue
            if self.jobs.active('stitch', take):
                errors.append('%s: stitching now - align it after' % take)
                continue
            cmd = [sys.executable, '-u', os.path.join(HERE, 'refine_extrinsics.py'), '--align', pair[0]]

            def done(job, f0=pair[0]):
                al = rig.take_alignment(f0)
                if job.status == 'done' and al:
                    job.result = {k: al.get(k) for k in ('date', 'shift_top', 'shift_bottom',
                                                         'corrections_from_base_deg', 'before', 'after',
                                                         'pair_offset')}
                    job.result['file'] = os.path.basename(al['_path'])
                    c = al.get('corrections_from_base_deg') or {}
                    job.stage = 'aligned · pitch %+.2f° roll %+.2f° yaw %+.2f° · shear %.1f / %.1f' % (
                        c.get('pitch', 0), c.get('roll', 0), c.get('yaw', 0),
                        al.get('shift_top', 0), al.get('shift_bottom', 0))
            job = self.jobs.submit(Job('align', take, 'Align ' + take, cmd, cwd=self.runtime,
                                       parser=align_parser, on_done=done))
            log('job %s: %s' % (job.id, job.title))
            started.append(job.id)
        return started, errors

    def start_stitch(self, body):
        take = body.get('take', '')
        pair = self.lib.pair(take)
        if not pair:
            raise ValueError('no such pair: %s' % take)
        tools = self.tools()
        if not tools['stitcher_ok']:
            raise ValueError('StitchPipeline not found at %s - build it (see README)' % self.stitcher)
        if self.jobs.active('align', take):
            raise ValueError('this take is being aligned - stitch it when that finishes')
        pv = self.previews.get(take, *pair)
        s = body.get('settings', {})
        degrees = float(s.get('degrees', 0))
        top, bottom = float(s.get('shift_top', 0)), float(s.get('shift_bottom', 0))
        crop = s.get('crop')
        if crop is not None:
            x, y, w, h = (int(round(float(v))) for v in crop)
            x, y = max(0, min(x, pv.rig.OW - 1)), max(0, min(y, pv.rig.OH - 1))
            w, h = max(2, min(w, pv.rig.OW - x)), max(2, min(h, pv.rig.OH - y))
            crop = [x, y, w, h]
        start = max(0, int(s.get('start') or 0))
        end = s.get('end')
        end = None if end in (None, '') else min(pv.total - 1, int(end))
        if end is not None and end < start:
            raise ValueError('the out point is before the in point')
        out = self.lib.out_path(body.get('out', '').strip())
        if not out:
            raise ValueError('the output must be a file name ending in .mp4 (it is saved in the takes folder)')
        if os.path.exists(out) and not body.get('overwrite'):
            return dict(exists=os.path.basename(out))

        cmd = [self.stitcher, '--source', pair[0], '--calib-dir', rig.CALIB_DIR,
               '--pair-offset', str(pv.pairing['offset']),
               '--degrees', '%g' % degrees, '--shift-top', '%g' % top, '--shift-bottom', '%g' % bottom]
        if crop:
            cmd += ['--crop', '%d,%d,%d,%d' % tuple(crop)]
        if start:
            cmd += ['--start', str(start)]
        if end is not None:
            cmd += ['--end', str(end)]
        cmd += ['--out-file', out]
        record = {
            'veery': 'stitch', 'version': VERSION, 'take': take,
            'sources': [os.path.basename(pair[0]), os.path.basename(pair[1])],
            'stitched': datetime.now().isoformat(timespec='seconds'),
            'calibration': {
                'base': os.path.relpath(pv.info()['calibration'], rig.REPO),
                'take_alignment': pv.info()['align']['file'] if pv.align else None,
                'corrections_from_base_deg': pv.align.get('corrections_from_base_deg') if pv.align else None,
                'rotation_matrix': [[round(v, 9) for v in row] for row in pv.R_used.tolist()],
            },
            'pairing': pv.pairing,
            'panorama': [pv.rig.OW, pv.rig.OH],
            'settings': {'degrees': degrees, 'crop': crop, 'shift_top': top, 'shift_bottom': bottom,
                         'start': start, 'end': end, 'bands': 6, 'exposure': True, 'smart_seam': True},
            'command': shlex.join(cmd),
        }
        meta = os.path.join(self.runtime, 'stitch-%d.ffmeta' % int(time.time() * 1000))
        with open(meta, 'w') as f:
            f.write(';FFMETADATA1\ntitle=%s\ncomment=%s\n' % (_ffmeta(take), _ffmeta(json.dumps(record))))
        cmd += ['--metadata-file', meta]
        total = (end if end is not None else pv.total - 1) - start + 1

        def done(job):
            if job.status == 'done' and os.path.exists(out):
                job.result = dict(output=os.path.basename(out), frames=total)
                job.stage = 'stitched → %s' % os.path.basename(out)
            elif job.status == 'cancelled' and os.path.exists(out):
                os.remove(out)                     # a cut-off MP4 has no index and will not play
                job.stage = 'cancelled (the partial output was removed)'
        job = Job('stitch', take, 'Stitch %s → %s' % (take, os.path.basename(out)), cmd, cwd=self.runtime,
                  parser=stitch_parser, on_done=done,
                  meta=dict(output=os.path.basename(out), end_frame=(end if end is not None else pv.total - 1),
                            frames=total))
        self.jobs.submit(job)
        log('job %s: %s' % (job.id, job.title))
        return dict(job=job.id)

    def start_render(self, body):
        name = body.get('video', '')
        path = self.lib.video(name)
        if not path or not self.lib.stitch_record(path):
            raise ValueError('not a stitched video: %s' % name)
        if not self.tools()['director_ok']:
            raise ValueError('Director not found at %s - build it (see README)' % self.director)
        proj = body.get('project')
        if not isinstance(proj, dict):
            raise ValueError('no project')
        out = self.lib.out_path(body.get('out', '').strip() or os.path.splitext(name)[0] + '_edit.mp4')
        if not out:
            raise ValueError('the output must be a file name ending in .mp4 (it is saved in the takes folder)')
        if out == path:
            raise ValueError('the output cannot replace the video being edited')
        if os.path.exists(out) and not body.get('overwrite') and not body.get('perPoint'):
            return dict(exists=os.path.basename(out))
        self.save_project(path, proj)               # render exactly what is saved
        codec = body.get('codec') if body.get('codec') in ('h264', 'hevc') else 'h264'
        quality = body.get('quality') if body.get('quality') in ('high', 'standard') else 'high'
        cmd = [self.director, '--render', project_path(path), '--video', path, '--out', out,
               '--codec', codec, '--quality', quality,
               '--unframed', 'wide' if body.get('unframed') == 'wide' else 'skip']
        if body.get('perPoint'):
            cmd.append('--per-point')

        def done(job):
            outs = [l for l in job.log if l.startswith(self.lib.folder)]
            if job.status == 'done':
                job.result = dict(outputs=[os.path.basename(l) for l in outs] or [os.path.basename(out)])
                job.stage = 'rendered → ' + ', '.join(job.result['outputs'])
        job = Job('render', name, 'Render %s → %s' % (name, os.path.basename(out)), cmd, cwd=self.runtime,
                  parser=render_parser, on_done=done, meta=dict(output=os.path.basename(out)))
        self.jobs.submit(job)
        log('job %s: %s' % (job.id, job.title))
        return dict(job=job.id)

    # ------------------------------------------------------------ the editor's project file
    def video_info(self, path):
        info = self.lib.probe(path)
        pp = project_path(path)
        j = dict(loaded=True, path=path, name=os.path.basename(path), width=info.get('width'),
                 height=info.get('height'), fps=info.get('fps'), frames=info.get('frames'),
                 projectPath=pp, stitch=self.lib.stitch_record(path))
        try:
            j['project'] = json.load(open(pp)) if os.path.exists(pp) else None
        except (OSError, ValueError):
            j['project'], j['projectError'] = None, 'could not parse ' + pp
        return j

    def save_project(self, path, proj):
        """Same rules as the Director: only the edit for THIS video is accepted, a
        timestamped backup at most every 5 minutes (20 kept), atomic write."""
        info = self.lib.probe(path)
        name = os.path.basename(path)
        if proj.get('video') != name or proj.get('frames') != info.get('frames'):
            raise Conflict('this edit is for %s, but %s is open - not saved' % (proj.get('video', '?'), name))
        pp = project_path(path)
        with self.save_lock:
            if os.path.exists(pp) and time.time() - self.last_backup.get(pp, 0) > 300:
                self.last_backup[pp] = time.time()
                stem = os.path.splitext(os.path.basename(pp))[0]          # "<video>.director"
                bdir = os.path.join(os.path.dirname(pp), stem + '-backups')
                os.makedirs(bdir, exist_ok=True)
                shutil.copy2(pp, os.path.join(bdir, '%s.%s.json' % (stem, time.strftime('%Y%m%d-%H%M%S'))))
                old = sorted(f for f in os.listdir(bdir) if f.endswith('.json'))
                for f in old[:-20]:
                    os.remove(os.path.join(bdir, f))
            with open(pp + '.tmp', 'w') as f:
                json.dump(proj, f, indent=1)
            os.replace(pp + '.tmp', pp)
        return pp


class Conflict(Exception):
    pass


def _ffmeta(s):
    """Escape a value for an FFMETADATA file."""
    return re.sub(r'([=;#\\\n])', r'\\\1', s)


# ---------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    studio = None

    def log_message(self, fmt, *args):     # quiet: polling would flood the terminal
        pass

    # ------------------------------------------------------------ responses
    def send(self, code, body, ctype='application/json', headers=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def page(self, name):
        with open(os.path.join(STATIC, name), 'rb') as f:
            self.send(200, f.read(), 'text/html; charset=utf-8')

    def body(self):
        n = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(n) if n else b''
        return json.loads(raw or b'{}')

    def serve_file(self, path, ctype):
        """Byte ranges, so the editor's <video> can seek anywhere in a multi-GB file.
        Open-ended ranges are answered 16 MB at a time; the browser asks for the rest."""
        size = os.path.getsize(path)
        rng = re.match(r'bytes=(\d*)-(\d*)', self.headers.get('Range', ''))
        a, b, partial = 0, size - 1, bool(rng)
        if rng:
            if rng.group(1):
                a = int(rng.group(1))
                b = min(int(rng.group(2)), size - 1) if rng.group(2) else min(size - 1, a + (16 << 20) - 1)
            elif rng.group(2):
                a = max(0, size - int(rng.group(2)))
            if a >= size or a > b:
                self.send(416, b'', 'text/plain', {'Content-Range': 'bytes */%d' % size})
                return
        self.send_response(206 if partial else 200)
        self.send_header('Content-Type', ctype)
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Length', str(b - a + 1))
        if partial:
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (a, b, size))
        self.end_headers()
        if self.command == 'HEAD':
            return
        with open(path, 'rb') as f:
            f.seek(a)
            left = b - a + 1
            while left > 0:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)

    # ------------------------------------------------------------ routing
    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        self.route('GET')

    def do_POST(self):
        self.route('POST')

    def route(self, method):
        st = self.studio
        u = urlparse(self.path)
        parts = [unquote(p) for p in u.path.strip('/').split('/') if p]
        q = parse_qs(u.query)
        try:
            # ---- pages
            if method == 'GET' and not parts:
                return self.page('index.html')
            if method == 'GET' and len(parts) == 2 and parts[0] == 'stitch':
                if not st.lib.pair(parts[1]):
                    return self.send(404, _notfound('No camera pair named %s in %s.' % (parts[1], st.lib.folder)),
                                     'text/html; charset=utf-8')
                return self.page('stitch.html')
            if method == 'GET' and len(parts) == 2 and parts[0] == 'edit':
                p = st.lib.video(parts[1])
                if not p or not st.lib.stitch_record(p):
                    return self.send(404, _notfound('%s is not a stitched video in %s (only videos stitched by '
                                                    'Studio carry the record the editor needs).'
                                                    % (parts[1], st.lib.folder)), 'text/html; charset=utf-8')
                return self.page('edit.html')
            if method == 'GET' and len(parts) == 2 and parts[0] == 'static':
                f = os.path.join(STATIC, os.path.basename(parts[1]))
                if os.path.isfile(f):
                    ctype = {'.css': 'text/css', '.js': 'text/javascript'}.get(os.path.splitext(f)[1], 'text/plain')
                    with open(f, 'rb') as fh:
                        return self.send(200, fh.read(), ctype + '; charset=utf-8')
            if not parts or parts[0] != 'api':
                return self.send(404, _notfound('Not found.'), 'text/html; charset=utf-8')

            api = parts[1:]
            # ---- library + jobs
            if method == 'GET' and api == ['library']:
                lib = st.lib.scan()
                lib['tools'] = st.tools()
                return self.send(200, lib)
            if method == 'GET' and api == ['jobs']:
                return self.send(200, dict(jobs=st.jobs.list(tail=3), now=time.time()))
            if method == 'GET' and len(api) == 2 and api[0] == 'jobs':
                job = st.jobs.get(api[1])
                return self.send(200, job.detail()) if job else self.send(404, dict(error='no such job'))
            if method == 'POST' and len(api) == 3 and api[0] == 'jobs' and api[2] == 'cancel':
                job = st.jobs.cancel(api[1])
                return self.send(200, job.summary()) if job else self.send(404, dict(error='no such job'))
            if method == 'POST' and api == ['jobs', 'align']:
                started, errors = st.start_align(self.body().get('takes', []))
                return self.send(200, dict(jobs=started, errors=errors))
            if method == 'POST' and api == ['jobs', 'stitch']:
                return self.send(200, st.start_stitch(self.body()))
            if method == 'POST' and api == ['jobs', 'render']:
                return self.send(200, st.start_render(self.body()))

            # ---- one take (the stitch page)
            if len(api) >= 3 and api[0] == 'takes':
                pair = st.lib.pair(api[1])
                if not pair:
                    return self.send(404, dict(error='no such pair'))
                pv = st.previews.get(api[1], *pair)
                if method == 'GET' and api[2] == 'info':
                    info = pv.info()
                    base = '%s_stitched' % api[1]
                    name, k = base + '.mp4', 2
                    while os.path.exists(os.path.join(st.lib.folder, name)):
                        name, k = '%s_%d.mp4' % (base, k), k + 1
                    info['default_out'] = name
                    info['tools'] = st.tools()
                    return self.send(200, info)
                if method == 'GET' and api[2] == 'frame' and len(api) == 5 and api[4] in ('L.jpg', 'R.jpg'):
                    L, R = pv.frame(int(api[3]))
                    return self.send(200, L if api[4] == 'L.jpg' else R, 'image/jpeg')

            # ---- one stitched video (the edit page)
            if len(api) >= 3 and api[0] == 'videos':
                path = st.lib.video(api[1])
                if not path or not st.lib.stitch_record(path):
                    return self.send(404, dict(error='not a stitched video'))
                if method == 'GET' and api[2] == 'info':
                    return self.send(200, st.video_info(path))
                if method == 'GET' and api[2] == 'stream':
                    return self.serve_file(path, 'video/mp4')
                if method == 'POST' and api[2] == 'save':
                    return self.send(200, dict(saved=st.save_project(path, self.body())))
            return self.send(404, dict(error='not found'))
        except Busy as e:
            j = e.args[0]
            return self.send(409, dict(error='%s is running (%s%%) - one at a time' % (
                j.title, j.percent if j.percent is not None else '…'), job=j.id))
        except Conflict as e:
            return self.send(409, dict(error=str(e)))
        except (ValueError, KeyError, TypeError) as e:
            return self.send(400, dict(error=str(e)))
        except (BrokenPipeError, ConnectionResetError):
            return None
        except Exception as e:                  # report, don't kill the connection silently
            log('error handling %s %s:\n%s' % (method, self.path, traceback.format_exc()))
            try:
                return self.send(500, dict(error='%s: %s' % (type(e).__name__, e)))
            except OSError:
                return None


def _notfound(msg):
    return ('<!doctype html><meta charset="utf-8"><title>Not found</title>'
            '<body style="font:15px system-ui;background:#14161a;color:#e6e8eb;padding:60px">'
            '<p>%s</p><p><a style="color:#4aa3ff" href="/">← Back to the takes</a></p>' % msg)


def main():
    ap = argparse.ArgumentParser(description='Veery Studio - align, stitch and edit takes in the browser')
    ap.add_argument('--takes', default=os.environ.get('VEERY_TAKES', '~/Desktop/veery-takes'),
                    help='the takes folder (default ~/Desktop/veery-takes, or $VEERY_TAKES)')
    ap.add_argument('--port', type=int, default=8100)
    ap.add_argument('--stitcher', default=os.environ.get('VEERY_STITCHER', os.path.join(BIN_DIR, 'StitchPipeline')))
    ap.add_argument('--director', default=os.environ.get('VEERY_DIRECTOR', os.path.join(BIN_DIR, 'Director')))
    ap.add_argument('--no-browser', action='store_true', help='do not open the browser')
    a = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)
    open_log()
    takes = os.path.abspath(os.path.expanduser(a.takes))
    if not os.path.isdir(takes):
        sys.exit('ERROR: takes folder %s does not exist' % takes)
    for tool in ('ffmpeg', 'ffprobe'):
        if not shutil.which(tool):
            sys.exit('ERROR: %s is not on PATH' % tool)

    Handler.studio = studio = Studio(takes, a.stitcher, a.director)
    srv = None
    for port in range(a.port, a.port + 10):
        try:
            srv = ThreadingHTTPServer(('127.0.0.1', port), Handler)
            break
        except OSError:
            continue
    if not srv:
        sys.exit('ERROR: no free port from %d' % a.port)
    srv.daemon_threads = True
    url = 'http://127.0.0.1:%d/' % srv.server_address[1]
    t = studio.tools()
    log('Veery Studio started at %s (pid %d)' % (url, os.getpid()))
    print('Veery Studio at %s  (Ctrl+C to stop; log: %s)' % (url, LOG_PATH))
    print('  takes:    %s' % takes)
    print('  stitcher: %s%s' % (a.stitcher, '' if t['stitcher_ok'] else '  [missing - build it]'))
    if t['stitcher_ok'] and not t['stitcher_metadata']:
        print('            [old build: rebuild it so stitched videos record their settings]')
    print('  director: %s%s' % (a.director, '' if t['director_ok'] else '  [missing - build it]'))

    def stop(signum=None, frame=None):
        name = signal.Signals(signum).name if signum else '?'
        why = {'SIGINT': 'Ctrl+C', 'SIGHUP': 'its Terminal window closed', 'SIGTERM': 'asked to quit (kill / logout)'}
        log('stopping: %s (%s)' % (name, why.get(name, 'signal')))
        studio.jobs.shutdown()
        threading.Thread(target=srv.shutdown, daemon=True).start()
    # Ctrl+C, kill, or closing the Terminal window: stop the jobs too (they run in their own
    # process groups, so nothing else would)
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)
    if not a.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    srv.serve_forever()
    studio.jobs.shutdown()
    log('Studio stopped.')


if __name__ == '__main__':
    main()
