"""
jobs.py - long-running commands (align, stitch, render) as background jobs.

Each job is one process - the same command you would type in a terminal - started
by the server. The server keeps the process, reads everything it prints into the
job's log, and turns its progress lines into a percent; the pages poll
GET /api/jobs for that. Jobs run in a pseudo-terminal so the programs print line by
line as they would in a terminal (into a pipe, C++ output would arrive in 4 KB lumps).

Limits per kind: a kind with reject_when_busy refuses a new job while one is
running (stitch, render); the others queue and start as slots free up (align).
"""
import collections, itertools, os, pty, re, signal, subprocess, threading, time


class Busy(Exception):
    pass


class Job:
    def __init__(self, kind, target, title, cmd, cwd=None, parser=None, on_done=None, meta=None):
        self.kind, self.target, self.title, self.cmd, self.cwd = kind, target, title, cmd, cwd
        self.parser = parser or (lambda job, line: True)
        self.on_done = on_done
        self.meta = meta or {}
        self.id = None
        self.status = 'queued'               # queued | running | done | failed | cancelled
        self.created = time.time()
        self.started = self.finished = None
        self.percent = None                  # None = no percentage known (yet)
        self.stage = 'queued'
        self.log = collections.deque(maxlen=5000)
        self.result = None
        self.error = None
        self.returncode = None
        self.proc = None
        self.cancel_requested = False
        self.closed = False                  # set once the process is gone and on_done has run
        self.state = {}                      # parser scratch (rates, last sample, ...)

    def summary(self, tail=0):
        d = dict(id=self.id, kind=self.kind, target=self.target, title=self.title, status=self.status,
                 created=self.created, started=self.started, finished=self.finished,
                 percent=self.percent, stage=self.stage, result=self.result, error=self.error,
                 returncode=self.returncode, meta=self.meta)
        if tail:
            d['log_tail'] = list(self.log)[-tail:]
        return d

    def detail(self):
        d = self.summary()
        d['log'] = list(self.log)
        d['cmd'] = self.cmd
        return d


class JobManager:
    def __init__(self, limits, reject_when_busy=()):
        self.limits = dict(limits)
        self.reject = set(reject_when_busy)
        self.jobs = collections.OrderedDict()
        self.lock = threading.RLock()
        self.ids = itertools.count(1)
        self.on_finish = None                  # called with each job once it has ended

    # ------------------------------------------------------------ queries
    def active(self, kind=None, target=None):
        with self.lock:
            return [j for j in self.jobs.values() if j.status in ('queued', 'running')
                    and (kind is None or j.kind == kind) and (target is None or j.target == target)]

    def get(self, jid):
        with self.lock:
            return self.jobs.get(jid)

    def list(self, tail=3):
        with self.lock:
            return [j.summary(tail) for j in self.jobs.values()]

    # ------------------------------------------------------------ control
    def submit(self, job):
        with self.lock:
            if job.kind in self.reject and self.active(job.kind):
                raise Busy(self.active(job.kind)[0])
            job.id = '%s-%d' % (job.kind, next(self.ids))
            self.jobs[job.id] = job
            # keep the history bounded: forget the oldest finished jobs
            done = [k for k, j in self.jobs.items() if j.status not in ('queued', 'running')]
            for k in done[:max(0, len(self.jobs) - 200)]:
                del self.jobs[k]
            self._pump(job.kind)
        return job

    def cancel(self, jid):
        with self.lock:
            job = self.jobs.get(jid)
            if not job:
                return None
            if job.status == 'queued':
                job.status, job.stage, job.finished = 'cancelled', 'cancelled before it started', time.time()
            elif job.status == 'running' and job.proc:
                job.cancel_requested = True
                job.stage = 'cancelling…'
                self._signal(job, signal.SIGTERM)
                threading.Timer(5.0, lambda: self._signal(job, signal.SIGKILL)
                                if job.status == 'running' else None).start()
        return job

    def shutdown(self, wait=8.0):
        """Stop every running job and wait for them to wind up (so a cancelled stitch's
        partial file is still cleaned up before the server exits)."""
        stopping = []
        with self.lock:
            for job in self.jobs.values():
                if job.status == 'queued':
                    job.status, job.stage = 'cancelled', 'cancelled (Studio stopped)'
                if job.status == 'running':
                    job.cancel_requested = True
                    self._signal(job, signal.SIGTERM)
                    stopping.append(job)
        end = time.time() + wait
        while time.time() < end and not all(j.closed for j in stopping):
            time.sleep(0.1)

    # ------------------------------------------------------------ internals
    @staticmethod
    def _signal(job, sig):
        try:
            os.killpg(job.proc.pid, sig)      # the whole group: the program and its ffmpeg children
        except (ProcessLookupError, PermissionError, OSError):
            pass

    def _pump(self, kind):
        running = [j for j in self.jobs.values() if j.kind == kind and j.status == 'running']
        queued = [j for j in self.jobs.values() if j.kind == kind and j.status == 'queued']
        for job in queued[:max(0, self.limits.get(kind, 1) - len(running))]:
            self._start(job)

    def _start(self, job):
        job.status, job.started, job.stage = 'running', time.time(), 'starting'
        job.log.append('$ ' + ' '.join(job.cmd))
        master, slave = pty.openpty()
        try:
            job.proc = subprocess.Popen(job.cmd, cwd=job.cwd, stdin=subprocess.DEVNULL, stdout=slave,
                                        stderr=slave, start_new_session=True, close_fds=True)
        except OSError as e:
            os.close(master)
            os.close(slave)
            job.status, job.error, job.finished = 'failed', 'could not start: %s' % e, time.time()
            job.stage = job.error
            return
        os.close(slave)
        threading.Thread(target=self._reader, args=(job, master), daemon=True).start()

    def _reader(self, job, fd):
        buf = b''
        while True:
            try:
                chunk = os.read(fd, 65536)
            except OSError:                    # EIO: the program (and every child) closed the terminal
                chunk = b''
            if not chunk:
                break
            buf += chunk
            parts = re.split(rb'[\r\n]', buf)
            buf = parts.pop()
            for p in parts:
                self._line(job, p)
        if buf:
            self._line(job, buf)
        os.close(fd)
        rc = job.proc.wait()
        with self.lock:
            job.returncode = rc
            job.finished = time.time()
            if job.cancel_requested:
                job.status, job.stage = 'cancelled', 'cancelled'
            elif rc == 0:
                job.status = 'done'
            else:
                job.status = 'failed'
                job.error = job.error or 'exited with code %d' % rc
        try:
            if job.on_done:
                job.on_done(job)
        except Exception as e:                 # a result that cannot be read is a failed job
            job.status, job.error = 'failed', 'finished, but: %s' % e
        with self.lock:
            if job.status == 'done':
                job.percent = 100
            if job.status != 'done' and job.error:
                job.stage = job.error
            job.closed = True
            if self.on_finish:
                self.on_finish(job)
            self._pump(job.kind)

    def _line(self, job, raw):
        line = raw.decode('utf-8', 'replace').rstrip()
        if not line.strip():
            return
        try:
            keep = job.parser(job, line)
        except Exception:
            keep = True
        if keep:
            job.log.append(line)


# ---------------------------------------------------------------- progress parsers
def _clock(s):
    s = int(max(0, s))
    return '%d:%02d' % (s // 60, s % 60) if s < 3600 else '%d:%02d:%02d' % (s // 3600, s // 60 % 60, s % 60)


STITCH_PCT = re.compile(r'^\s*(\d+)%\s+\(frame (\d+)\)')


def stitch_parser(job, line):
    """StitchPipeline prints '  43%  (frame 1234)' every 30 frames."""
    m = STITCH_PCT.match(line)
    if not m:
        if line.startswith(('panorama ', 'render ', 'encoder:', 'alignment:', '  paired input', 'crop ',
                            'rotate ', 'shear ')):
            job.stage = line.strip()
        return True
    pct, frame = int(m.group(1)), int(m.group(2))
    now = time.time()
    st = job.state
    if 'f0' not in st:
        st['f0'], st['t0'] = frame, now
    job.percent = pct
    rate = (frame - st['f0']) / (now - st['t0']) if now > st['t0'] + 1 else 0
    end = job.meta.get('end_frame')
    eta = ' · %s left' % _clock((end - frame) / rate) if rate > 0 and end else ''
    job.stage = 'frame %d%s%s' % (frame, ' · %.0f fps' % rate if rate else '', eta)
    return False                               # progress lines stay out of the log


ALIGN_STEPS = [(re.compile(r'^Pair offset'), 5, 'pairing the two files'),
               (re.compile(r'^Take:'), 8, 'decoding the measured frames'),
               (re.compile(r'^\s*Current calibration:'), 30, 'measured with the current calibration'),
               (re.compile(r'^\s*iter (\d+)'), None, None),
               (re.compile(r'^Corrections'), 92, 'solved'),
               (re.compile(r'^Wrote'), 99, 'written')]


def align_parser(job, line):
    for rx, pct, stage in ALIGN_STEPS:
        m = rx.match(line)
        if m:
            if pct is None:                    # solver iteration k (at most 10)
                k = int(m.group(1))
                job.percent = min(90, 35 + 6 * k)
                job.stage = 'solving · iteration %d' % (k + 1)
            else:
                job.percent, job.stage = pct, stage
            break
    if line.startswith('ERROR'):
        job.error = line
    return True


RENDER_PCT = re.compile(r'^\s*(\d+)/(\d+) frames\s*(.*)$')


def render_parser(job, line):
    """Director --render prints '  cur/total frames  point name' every 2 s."""
    m = RENDER_PCT.match(line)
    if not m:
        return True
    cur, total = int(m.group(1)), int(m.group(2))
    now = time.time()
    st = job.state
    if 'c0' not in st:
        st['c0'], st['t0'] = cur, now
    rate = (cur - st['c0']) / (now - st['t0']) if now > st['t0'] + 1 else 0
    job.percent = round(100.0 * cur / total, 1) if total else None
    job.stage = '%d / %d frames%s%s%s' % (cur, total, ' · %.0f fps' % rate if rate else '',
                                         ' · %s left' % _clock((total - cur) / rate) if rate > 0 else '',
                                         ' · ' + m.group(3).strip() if m.group(3).strip() else '')
    return False
