// director.cpp - "virtual camera" editor for a stitched panorama.
//
// Two passes, both in a browser page (director.html) served by this program:
//   1. Cut   - mark each point of the game with In / Out (like LosslessCut).
//              Only marked points end up in the output.
//   2. Frame - per point, steer a 16:9 box over the full-width panorama while it
//              plays. One take covers the whole point (In..Out); a redo replaces
//              the whole take. The box path is smoothed per take.
// The edit lives in a project file next to the video (<video>.director.json),
// saved by the page as you work. Rendering crops the box out of every frame of
// every point and encodes the result at the output size (default 2560x1440):
// one video with the points in order, or one file per point.
//
// Usage:
//   Director                         open the editor, pick a video in the page
//   Director --video stitched.mp4    open the editor on that video
//   Director --render project.json --out edit.mp4 [--per-point] [--unframed skip|wide]
//            [--codec h264|hevc] [--quality high|standard]
//                                    render headless from a saved project
// Needs ffmpeg + ffprobe on PATH (decode with frame-accurate seeks, encode).

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <csignal>
#include <algorithm>
#include <iomanip>
#include <cctype>
#include <chrono>
#include <map>
#include <ctime>
#include "json.hpp"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  using socket_t = SOCKET;
  #define CLOSESOCK closesocket
  #define popen _popen
  #define pclose _pclose
  #define PIPE_RMODE "rb"
  #define PIPE_WMODE "wb"
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using socket_t = int;
  #define CLOSESOCK close
  #define INVALID_SOCKET (-1)
  #define PIPE_RMODE "r"
  #define PIPE_WMODE "w"
#endif
#ifdef __APPLE__
  #include <mach-o/dyld.h>
#endif

using json = nlohmann::json;
using namespace std;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- small helpers

static string runCapture(const string &cmd)
{
    string out;
    FILE *p = popen(cmd.c_str(), PIPE_RMODE);
    if (!p) return "";
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static string q(const string &s) { return "\"" + s + "\""; }

static string devNull()
{
#ifdef _WIN32
    return " >NUL 2>&1";
#else
    return " >/dev/null 2>&1";
#endif
}

static string exePath()
{
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    return n > 0 ? string(buf, n) : string();
#elif __APPLE__
    char buf[4096]; uint32_t size = sizeof(buf);
    return _NSGetExecutablePath(buf, &size) == 0 ? string(buf) : string();
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return string(buf); }
    return string();
#endif
}

static string pickFile(bool save, const string &defName = "")
{
#ifdef _WIN32
    string cls = save ? "SaveFileDialog" : "OpenFileDialog";
    string extra = save ? ("$d.FileName='" + defName + "';") : "";
    return runCapture("powershell -NoProfile -Command \"Add-Type -AssemblyName System.Windows.Forms;"
                      "$o=New-Object System.Windows.Forms.Form -Property @{TopMost=$true};"
                      "$d=New-Object System.Windows.Forms." + cls + ";" + extra +
                      "if($d.ShowDialog($o) -eq 'OK'){$d.FileName}\" 2>NUL");
#elif __APPLE__
    if (save)
        return runCapture("osascript -e 'POSIX path of (choose file name with prompt "
                          "\"Save the edited video as\" default name \"" + defName + "\")' 2>/dev/null");
    return runCapture("osascript -e 'POSIX path of (choose file with prompt "
                      "\"Select the stitched video to edit\")' 2>/dev/null");
#else
    if (save)
        return runCapture("zenity --file-selection --save --confirm-overwrite --filename=\"" + defName + "\" 2>/dev/null");
    return runCapture("zenity --file-selection --title=\"Select the stitched video\" 2>/dev/null");
#endif
}

static void openBrowser(const string &url)
{
#ifdef _WIN32
    system(("start \"\" \"" + url + "\"").c_str());
#elif __APPLE__
    system(("open \"" + url + "\"").c_str());
#else
    system(("xdg-open \"" + url + "\" >/dev/null 2>&1 &").c_str());
#endif
}

static string argVal(int argc, char **argv, const string &key, const string &def)
{
    for (int i = 1; i < argc - 1; i++) if (key == argv[i]) return argv[i + 1];
    return def;
}
static bool hasArg(int argc, char **argv, const string &key)
{
    for (int i = 1; i < argc; i++) if (key == argv[i]) return true;
    return false;
}

static string urlDecode(const string &s)
{
    string o;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '%' && i + 2 < s.size()) { o.push_back((char)stoi(s.substr(i + 1, 2), nullptr, 16)); i += 2; }
        else if (s[i] == '+') o.push_back(' ');
        else o.push_back(s[i]);
    }
    return o;
}

static string qparam(const string &query, const string &key)
{
    string k = key + "=";
    for (size_t p = query.find(k); p != string::npos; p = query.find(k, p + 1))
    {
        if (p != 0 && query[p - 1] != '&') continue;
        size_t s = p + k.size(), e = query.find('&', s);
        return urlDecode(query.substr(s, e == string::npos ? string::npos : e - s));
    }
    return "";
}

// ---------------------------------------------------------------- video + project

struct VideoInfo
{
    string path;
    int width = 0, height = 0, frames = 0;
    double fps = 30.0;
    bool fullRange = false;        // "pc" range: black is Y=0 instead of 16
    bool ok() const { return width > 0 && height > 0 && frames > 0; }
};

// Exact size / rate / frame count from the container (ffprobe), falling back to a
// counted decode when the container doesn't record nb_frames.
static VideoInfo probeVideo(const string &path)
{
    VideoInfo v; v.path = path;
    string out = runCapture("ffprobe -v error -select_streams v:0 -show_entries "
                            "stream=width,height,r_frame_rate,nb_frames,color_range -of json " + q(path));
    try
    {
        json j = json::parse(out);
        auto &s = j.at("streams").at(0);
        v.width = s.value("width", 0); v.height = s.value("height", 0);
        string r = s.value("r_frame_rate", "30/1");
        size_t sl = r.find('/');
        double num = stod(r.substr(0, sl)), den = sl == string::npos ? 1.0 : stod(r.substr(sl + 1));
        if (den > 0) v.fps = num / den;
        if (s.contains("nb_frames")) v.frames = stoi(s["nb_frames"].get<string>());
        v.fullRange = s.value("color_range", string()) == "pc";
    }
    catch (...) { return v; }
    if (v.frames <= 0)
    {
        string c = runCapture("ffprobe -v error -count_frames -select_streams v:0 -show_entries "
                              "stream=nb_read_frames -of csv=p=0 " + q(path));
        try { v.frames = stoi(c); } catch (...) {}
    }
    return v;
}

static string projectPathFor(const string &video)
{
    fs::path p(video);
    return (p.parent_path() / (p.stem().string() + ".director.json")).string();
}

static bool writeFileAtomic(const string &path, const string &data)
{
    string tmp = path + ".tmp";
    { ofstream f(tmp, ios::binary | ios::trunc); if (!f) return false; f << data; if (!f) return false; }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

// Before overwriting the project, keep a timestamped copy of what's on disk - at
// most one per 5 minutes (and always on the first save of a session), newest 20
// kept - in <stem>.director-backups/ next to it. Autosave runs every second, so
// this is what lets an edit be wound back to how it was some minutes ago.
static void backupProject(const string &pp)
{
    static std::mutex m;
    static map<string, std::chrono::steady_clock::time_point> last;
    lock_guard<mutex> lk(m);
    std::error_code ec;
    if (!fs::exists(pp, ec)) return;
    auto now = std::chrono::steady_clock::now();
    auto it = last.find(pp);
    if (it != last.end() && now - it->second < std::chrono::minutes(5)) return;
    last[pp] = now;
    fs::path src(pp);
    string stem = src.stem().string();                       // "<video>.director"
    fs::path dir = src.parent_path() / (stem + "-backups");
    fs::create_directories(dir, ec);
    time_t tt = time(nullptr);
    char ts[32]; strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", localtime(&tt));
    fs::copy_file(src, dir / (stem + "." + ts + ".json"), fs::copy_options::overwrite_existing, ec);
    vector<fs::path> all;
    for (auto &e : fs::directory_iterator(dir, ec)) if (e.path().extension() == ".json") all.push_back(e.path());
    sort(all.begin(), all.end());                           // timestamps sort by name
    for (size_t i = 0; i + 20 < all.size(); i++) fs::remove(all[i], ec);
}

static string readFile(const string &path)
{
    ifstream f(path, ios::binary);
    if (!f) return "";
    ostringstream s; s << f.rdbuf(); return s.str();
}

// ---------------------------------------------------------------- render

struct RenderOpts
{
    string out;                 // output file (or base name with --per-point)
    bool perPoint = false;      // one file per point instead of one video
    bool unframedWide = false;  // unframed points: left out by default (or a wide shot)
    string codec = "h264";      // "h264" (plays everywhere) | "hevc" (same quality, smaller)
    string quality = "high";    // "high" | "standard"
};

struct Progress
{
    std::mutex mu;
    bool busy = false, done = false;
    long long total = 0, cur = 0;
    string msg, result;
};
static Progress g_prog;

static bool encoderWorks(const string &enc)
{
    return system(("ffmpeg -hide_banner -loglevel error -f lavfi -i color=c=black:s=256x144:d=0.1 "
                   "-c:v " + enc + " -f null -" + devNull()).c_str()) == 0;
}

// The output is a normal 16:9 video (<= 4096 wide), so hardware H.264 fits and plays
// everywhere. Bitrate ~0.3 bits/pixel: 2560x1440@30 -> ~33 Mbit/s.
static string g_vencOverride, g_vencOpts;   // --venc / --vopts (testing and power use)
static bool g_fullRange = false;            // source range, carried through to the output tag

// Encoder per codec: hardware where it works (VideoToolbox on a Mac, NVENC elsewhere),
// else the software encoder. Checked once per name.
static string pickEncoder(const string &codec)
{
    static map<string, string> cache;
    auto it = cache.find(codec);
    if (it != cache.end()) return it->second;
    bool hevc = codec == "hevc";
#ifdef __APPLE__
    string hw = hevc ? "hevc_videotoolbox" : "h264_videotoolbox";
#else
    string hw = hevc ? "hevc_nvenc" : "h264_nvenc";
#endif
    string enc = encoderWorks(hw) ? hw : (hevc ? "libx265" : "libx264");
    cache[codec] = enc;
    return enc;
}

// Bitrates measured on real footage against an exact crop (1:1 box, 2026-10-04):
//   HW H.264  0.30 bpp (33 Mbit/s @1440p30) 45.3 dB   0.55 bpp (61 Mbit/s) 48.1 dB
//   HW HEVC   0.30 bpp 46.8 dB                          0.40 bpp (44 Mbit/s) 48.2 dB
// "high" = ~48 dB (visually transparent on grass); "standard" = the smaller ~45 dB.
static string encodeCmd(int W, int H, double fps, const string &out, const RenderOpts &o)
{
    string enc = !g_vencOverride.empty() ? g_vencOverride : pickEncoder(o.codec);
    bool hevc = enc.find("hevc") != string::npos || enc.find("265") != string::npos;
    bool high = o.quality != "standard";
    double bpp = hevc ? (high ? 0.40 : 0.22) : (high ? 0.55 : 0.30);
    long long br = (long long)(bpp * W * H * fps);
    ostringstream c;
    // Frames arrive already in the source's own YUV 4:2:0 (see renderProject), so they
    // are tagged rather than converted: no colour-matrix guesswork, no rounding.
    c << "ffmpeg -y -hide_banner -loglevel error -f rawvideo -pixel_format yuv420p -video_size "
      << W << "x" << H << " -framerate " << fps << " -color_range " << (g_fullRange ? "pc" : "tv")
      << " -colorspace bt709 -i - -an -c:v " << enc;
    if (!g_vencOpts.empty()) c << " " << g_vencOpts;
    else if (enc == "libx264") c << " -preset slow -crf " << (high ? 16 : 20);
    else if (enc == "libx265") c << " -preset medium -crf " << (high ? 18 : 22);
    else c << " -b:v " << br;
    if (hevc) c << " -tag:v hvc1";                       // QuickTime/Safari only open the hvc1 tag
    c << " -pix_fmt yuv420p -color_range " << (g_fullRange ? "pc" : "tv")
      << " -colorspace bt709 -color_primaries bt709 -color_trc bt709 -movflags +faststart " << q(out);
    return c.str();
}

// Decode `count` frames starting EXACTLY at frame `first` (ffmpeg's input -ss is
// frame-accurate when decoding; the half-frame back-off makes the first kept
// frame `first` rather than the one after it). Frames stay in the source's own
// YUV 4:2:0: converting to RGB and back cost ~6 dB and shifted colours, because the
// two conversions didn't agree on the colour matrix (measured 2026-10-04).
static FILE *openDecode(const VideoInfo &v, int first, int count)
{
    double t = max(0.0, (first - 0.5) / v.fps);
    ostringstream c;
    c << "ffmpeg -hide_banner -loglevel error";
#ifdef __APPLE__
    c << " -hwaccel videotoolbox";
#endif
    c << " -ss " << std::fixed << std::setprecision(6) << t << " -i " << q(v.path)
      << " -frames:v " << count << " -f rawvideo -pix_fmt yuv420p -";
    return popen(c.str().c_str(), PIPE_RMODE);
}

// A YUV 4:2:0 frame as three plane views over one contiguous buffer.
struct Planes
{
    cv::Mat buf, Y, U, V;
    void alloc(int w, int h)
    {
        int cw = (w + 1) / 2, ch = (h + 1) / 2;
        buf.create(1, w * h + 2 * cw * ch, CV_8U);
        Y = cv::Mat(h, w, CV_8U, buf.data);
        U = cv::Mat(ch, cw, CV_8U, buf.data + (size_t)w * h);
        V = cv::Mat(ch, cw, CV_8U, buf.data + (size_t)w * h + (size_t)cw * ch);
    }
    size_t bytes() const { return buf.total(); }
};

// Scale one plane: dst(u,v) = src(s*u + ox, s*v + oy). Bicubic, so sub-pixel
// positions stay crisp (bilinear at a half-pixel offset is a visible blur). When
// shrinking (s > 1) the source region is low-passed first so fine grass and lines
// don't alias into shimmer; only the region the box reads is filtered.
static void scalePlane(const cv::Mat &src, cv::Mat &dst, double s, double ox, double oy, int border)
{
    cv::Matx23d M(s, 0, ox, 0, s, oy);
    if (s > 1.01)
    {
        int pad = (int)ceil(3 * s) + 3;
        cv::Rect r((int)floor(ox) - pad, (int)floor(oy) - pad,
                   (int)ceil(s * dst.cols) + 2 * pad, (int)ceil(s * dst.rows) + 2 * pad);
        r &= cv::Rect(0, 0, src.cols, src.rows);
        if (r.area() > 0)
        {
            cv::Mat lp;
            cv::GaussianBlur(src(r), lp, cv::Size(), 0.45 * sqrt(s * s - 1));
            M = cv::Matx23d(s, 0, ox - r.x, 0, s, oy - r.y);
            cv::warpAffine(lp, dst, M, dst.size(), cv::INTER_CUBIC | cv::WARP_INVERSE_MAP,
                           cv::BORDER_CONSTANT, cv::Scalar(border));
            return;
        }
    }
    cv::warpAffine(src, dst, M, dst.size(), cv::INTER_CUBIC | cv::WARP_INVERSE_MAP,
                   cv::BORDER_CONSTANT, cv::Scalar(border));
}

// One output frame: the box (centre x, bottom y, width; video pixels) scaled to the
// output size with a sub-pixel affine, so slow pans glide instead of stepping a
// whole pixel at a time. Luma and chroma are scaled separately. Chroma follows the
// standard 4:2:0 siting (horizontally on even luma columns, vertically between
// rows), so a whole-pixel 1:1 box is an exact copy. Outside the panorama is black.
static void cropFrame(const Planes &src, double cx, double by, double w,
                      int OW, int OH, Planes &dst, bool fullRange)
{
    double s = w / OW, h = w * OH / OW;
    double x0 = cx - w / 2, y0 = by - h;
    scalePlane(src.Y, dst.Y, s, x0, y0, fullRange ? 0 : 16);
    double cox = x0 / 2, coy = (y0 + 0.5 * s - 0.5) / 2;
    scalePlane(src.U, dst.U, s, cox, coy, 128);
    scalePlane(src.V, dst.V, s, cox, coy, 128);
}

static string safeName(const string &s)
{
    string o;
    for (char c : s) o.push_back(isalnum((unsigned char)c) || c == '-' || c == '_' ? c : '_');
    return o;
}

// Render a project. Points in time order; each point's take gives one box per frame
// (In..Out). Unframed points get a wide shot or are skipped. Returns "" or an error.
static string renderProject(const json &proj, const VideoInfo &v, const RenderOpts &o)
{
    int OW = proj.value("output", json::object()).value("width", 2560);
    int OH = proj.value("output", json::object()).value("height", 1440);
    OW &= ~1; OH &= ~1;
    struct Job { string name; int in, out; const json *path; };
    vector<Job> jobs;
    static const json noPoints = json::array();
    const json &points = proj.contains("points") && proj["points"].is_array() ? proj["points"] : noPoints;
    for (auto &p : points)                     // a reference: jobs keep pointers into these takes
    {
        Job j{p.value("name", "Point"), p.value("in", 0), p.value("out", 0), nullptr};
        if (j.out < j.in || j.in < 0 || j.out >= v.frames) continue;
        if (p.contains("take") && p["take"].is_object() && p["take"].contains("path")
            && (int)p["take"]["path"].size() == j.out - j.in + 1)
            j.path = &p["take"]["path"];
        else if (!o.unframedWide) continue;
        jobs.push_back(j);
    }
    sort(jobs.begin(), jobs.end(), [](const Job &a, const Job &b) { return a.in < b.in; });
    if (jobs.empty()) return "ERROR: nothing to render (no points" + string(o.unframedWide ? "" : " with a take") + ")";

    long long total = 0;
    for (auto &j : jobs) total += j.out - j.in + 1;
    { lock_guard<mutex> lk(g_prog.mu); g_prog.total = total; g_prog.cur = 0; }

    g_fullRange = v.fullRange;
    Planes src, dst;
    src.alloc(v.width, v.height);
    dst.alloc(OW, OH);
    const size_t fbytes = src.bytes();
    FILE *enc = nullptr;
    fs::path base(o.out);
    vector<string> written;
    auto openEnc = [&](const string &file) {
        enc = popen(encodeCmd(OW, OH, v.fps, file, o).c_str(), PIPE_WMODE);
        if (enc) written.push_back(file);
        return enc != nullptr;
    };
    if (!o.perPoint && !openEnc(o.out)) return "ERROR: cannot start the encoder (is ffmpeg on PATH?)";

    const double wideW = min((double)v.width, v.height * (double)OW / OH);
    int idx = 0;
    for (auto &j : jobs)
    {
        ++idx;
        if (o.perPoint)
        {
            char num[16]; snprintf(num, sizeof(num), "%02d", idx);
            string file = (base.parent_path() / (base.stem().string() + "_" + num + "_" + safeName(j.name)
                                                 + base.extension().string())).string();
            if (!openEnc(file)) return "ERROR: cannot start the encoder for " + file;
        }
        int n = j.out - j.in + 1;
        { lock_guard<mutex> lk(g_prog.mu); g_prog.msg = j.name + " (" + to_string(n) + " frames)"; }
        FILE *dec = openDecode(v, j.in, n);
        if (!dec) { if (enc) pclose(enc); return "ERROR: cannot start the decoder (is ffmpeg on PATH?)"; }
        for (int k = 0; k < n; k++)
        {
            if (fread(src.buf.data, 1, fbytes, dec) != fbytes)
            { pclose(dec); if (enc) pclose(enc); return "ERROR: video ended early in " + j.name + " (frame " + to_string(j.in + k) + ")"; }
            double cx, by, w;
            if (j.path) { auto &s = (*j.path)[k]; cx = s[0]; by = s[1]; w = s[2]; }
            else { w = wideW; cx = v.width / 2.0; by = v.height; }
            cropFrame(src, cx, by, w, OW, OH, dst, v.fullRange);
            if (fwrite(dst.buf.data, 1, dst.bytes(), enc) != dst.bytes())
            { pclose(dec); pclose(enc); return "ERROR: encoder closed early (see ffmpeg output above)"; }
            lock_guard<mutex> lk(g_prog.mu); g_prog.cur++;
        }
        pclose(dec);
        if (o.perPoint) { if (pclose(enc) != 0) return "ERROR: encoder failed"; enc = nullptr; }
    }
    if (enc && pclose(enc) != 0) return "ERROR: encoder failed";
    string r;
    for (auto &w : written) r += (r.empty() ? "" : "\n") + w;
    return r;
}

// ---------------------------------------------------------------- http server

struct Request { string method, path, query, body; long long rangeA = -1, rangeB = -1; };

static bool sendAll(socket_t s, const char *d, size_t n)
{
    while (n > 0)
    {
        int k = (int)send(s, d, (int)min(n, (size_t)1 << 20), 0);
        if (k <= 0) return false;
        d += k; n -= (size_t)k;
    }
    return true;
}

static void respond(socket_t s, const string &status, const string &ctype, const string &body)
{
    string h = "HTTP/1.1 " + status + "\r\nContent-Type: " + ctype + "\r\nContent-Length: "
             + to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    sendAll(s, h.data(), h.size());
    sendAll(s, body.data(), body.size());
}

static bool readRequest(socket_t s, Request &r)
{
    string buf; char tmp[65536];
    size_t hdrEnd;
    while ((hdrEnd = buf.find("\r\n\r\n")) == string::npos)
    {
        int n = (int)recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, n);
        if (buf.size() > (1 << 20)) return false;
    }
    string head = buf.substr(0, hdrEnd);
    r.body = buf.substr(hdrEnd + 4);
    istringstream hs(head);
    string line, target, ver;
    getline(hs, line);
    istringstream(line) >> r.method >> target >> ver;
    size_t qm = target.find('?');
    r.path = target.substr(0, qm);
    r.query = qm == string::npos ? "" : target.substr(qm + 1);
    long long clen = 0;
    while (getline(hs, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':');
        if (c == string::npos) continue;
        string k = line.substr(0, c), val = line.substr(c + 1);
        while (!val.empty() && val[0] == ' ') val.erase(0, 1);
        transform(k.begin(), k.end(), k.begin(), ::tolower);
        if (k == "content-length") clen = stoll(val);
        if (k == "range" && val.rfind("bytes=", 0) == 0)
        {
            string spec = val.substr(6);
            size_t d = spec.find('-');
            if (d != string::npos)
            {
                if (d > 0) r.rangeA = stoll(spec.substr(0, d));
                if (d + 1 < spec.size()) r.rangeB = stoll(spec.substr(d + 1));
            }
        }
    }
    while ((long long)r.body.size() < clen)
    {
        int n = (int)recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        r.body.append(tmp, n);
    }
    return true;
}

// Byte-range file serving so the page's <video> can seek anywhere in a multi-GB
// file. Open-ended ranges are answered in chunks; the browser asks for the rest.
static void serveFile(socket_t s, const Request &r, const string &path, const string &ctype)
{
    ifstream f(path, ios::binary);
    if (!f) { respond(s, "404 Not Found", "text/plain", "no video"); return; }
    f.seekg(0, ios::end);
    long long size = f.tellg();
    long long a = 0, b = size - 1;
    bool partial = r.rangeA >= 0 || r.rangeB >= 0;
    if (partial)
    {
        if (r.rangeA >= 0) { a = r.rangeA; if (r.rangeB >= 0) b = min(r.rangeB, size - 1); }
        else { a = max(0LL, size - r.rangeB); }                  // suffix range: last N bytes
        if (r.rangeB < 0) b = min(size - 1, a + (16LL << 20) - 1);
        if (a >= size || a > b)
        {
            string h = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + to_string(size)
                     + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(s, h.data(), h.size());
            return;
        }
    }
    long long len = b - a + 1;
    ostringstream h;
    h << "HTTP/1.1 " << (partial ? "206 Partial Content" : "200 OK") << "\r\nContent-Type: " << ctype
      << "\r\nAccept-Ranges: bytes\r\nContent-Length: " << len;
    if (partial) h << "\r\nContent-Range: bytes " << a << "-" << b << "/" << size;
    h << "\r\nConnection: close\r\n\r\n";
    string hs = h.str();
    if (!sendAll(s, hs.data(), hs.size())) return;
    if (r.method == "HEAD") return;
    f.seekg(a);
    vector<char> buf(1 << 20);
    while (len > 0)
    {
        f.read(buf.data(), (streamsize)min<long long>(len, (long long)buf.size()));
        streamsize got = f.gcount();
        if (got <= 0 || !sendAll(s, buf.data(), (size_t)got)) return;
        len -= got;
    }
}

// The page itself: director.html next to the executable, else in the source folder
// (so editing the page needs no rebuild during development).
static string pagePath()
{
    fs::path e = fs::path(exePath()).parent_path();
    for (fs::path c : {e / "director.html", e.parent_path() / "director.html", fs::path(DIRECTOR_SRC_DIR) / "director.html"})
        if (fs::exists(c)) return c.string();
    return "";
}

struct Server
{
    std::mutex mu;
    VideoInfo video;

    string infoJson()
    {
        lock_guard<mutex> lk(mu);
        json j;
        j["loaded"] = video.ok();
        if (video.ok())
        {
            j["path"] = video.path; j["name"] = fs::path(video.path).filename().string();
            j["width"] = video.width; j["height"] = video.height;
            j["fps"] = video.fps; j["frames"] = video.frames;
            string pp = projectPathFor(video.path);
            j["projectPath"] = pp;
            string txt = readFile(pp);
            try { j["project"] = txt.empty() ? json(nullptr) : json::parse(txt); }
            catch (...) { j["project"] = nullptr; j["projectError"] = "could not parse " + pp; }
        }
        return j.dump();
    }

    string load(const string &path)
    {
        VideoInfo v = probeVideo(path);
        if (!v.ok()) return "cannot read video (is ffprobe on PATH?): " + path;
        lock_guard<mutex> lk(mu);
        video = v;
        cout << "video: " << v.path << "  " << v.width << "x" << v.height << " @ " << v.fps
             << " fps, " << v.frames << " frames\n";
        return "";
    }

    void handle(socket_t s)
    {
        Request r;
        if (!readRequest(s, r)) { CLOSESOCK(s); return; }
        try { route(s, r); }
        catch (const std::exception &e) { respond(s, "500 Internal Server Error", "text/plain", e.what()); }
        CLOSESOCK(s);
    }

    void route(socket_t s, const Request &r)
    {
        const string &p = r.path;
        if (p == "/")
        {
            string pp = pagePath();
            if (pp.empty()) { respond(s, "500 Internal Server Error", "text/plain", "director.html not found"); return; }
            respond(s, "200 OK", "text/html; charset=utf-8", readFile(pp));
        }
        else if (p == "/info") respond(s, "200 OK", "application/json", infoJson());
        else if (p == "/open")
        {
            string path = qparam(r.query, "path");
            if (path.empty()) path = pickFile(false);
            if (path.empty()) { respond(s, "200 OK", "application/json", "{\"cancelled\":true}"); return; }
            string err = load(path);
            if (!err.empty()) { respond(s, "200 OK", "application/json", json({{"error", err}}).dump()); return; }
            respond(s, "200 OK", "application/json", infoJson());
        }
        else if (p == "/video")
        {
            string path; { lock_guard<mutex> lk(mu); path = video.path; }
            serveFile(s, r, path, "video/mp4");
        }
        else if (p == "/save" && r.method == "POST")
        {
            json j = json::parse(r.body);                     // validate before touching the file
            string pp, name; int frames;
            { lock_guard<mutex> lk(mu); if (!video.ok()) { respond(s, "400 Bad Request", "text/plain", "no video"); return; }
              pp = projectPathFor(video.path); name = fs::path(video.path).filename().string(); frames = video.frames; }
            // Only accept the project for the video that is loaded now: a tab left open on
            // another video (or its save-on-close) must not overwrite this video's edit.
            if (j.value("video", string()) != name || j.value("frames", -1) != frames)
            {
                respond(s, "409 Conflict", "text/plain", "this edit is for " + j.value("video", string("?")) +
                        ", but " + name + " is open - not saved");
                return;
            }
            backupProject(pp);
            if (!writeFileAtomic(pp, j.dump(1))) { respond(s, "500 Internal Server Error", "text/plain", "cannot write " + pp); return; }
            respond(s, "200 OK", "application/json", json({{"saved", pp}}).dump());
        }
        else if (p == "/pickout")
        {
            string def; { lock_guard<mutex> lk(mu); def = fs::path(video.path).stem().string() + "_edit.mp4"; }
            string out = pickFile(true, def);
            respond(s, "200 OK", "application/json", json({{"path", out}}).dump());
        }
        else if (p == "/render" && r.method == "POST")
        {
            json body = json::parse(r.body);                   // parse first: a bad request must not leave us "busy"
            {
                lock_guard<mutex> lk(g_prog.mu);
                if (g_prog.busy) { respond(s, "200 OK", "application/json", "{\"error\":\"already rendering\"}"); return; }
                g_prog.busy = true; g_prog.done = false; g_prog.result.clear(); g_prog.msg = "starting"; g_prog.cur = 0; g_prog.total = 0;
            }
            RenderOpts o;
            o.out = body.value("out", string());
            o.perPoint = body.value("perPoint", false);
            o.unframedWide = body.value("unframed", string("skip")) == "wide";
            o.codec = body.value("codec", string("h264"));
            o.quality = body.value("quality", string("high"));
            VideoInfo v; { lock_guard<mutex> lk(mu); v = video; }
            json proj = body.value("project", json::object());
            if (o.out.empty()) o.out = (fs::path(v.path).parent_path() / (fs::path(v.path).stem().string() + "_edit.mp4")).string();
            std::thread([proj, v, o]() {
                cout << "render -> " << o.out << (o.perPoint ? " (one file per point)" : "") << "\n";
                string res;
                try { res = renderProject(proj, v, o); }
                catch (const std::exception &e) { res = string("ERROR: ") + e.what(); }
                cout << "render: " << res << "\n";
                lock_guard<mutex> lk(g_prog.mu);
                g_prog.result = res; g_prog.done = true; g_prog.busy = false;
            }).detach();
            respond(s, "200 OK", "application/json", "{\"started\":true}");
        }
        else if (p == "/progress")
        {
            lock_guard<mutex> lk(g_prog.mu);
            json j = {{"busy", g_prog.busy}, {"done", g_prog.done}, {"cur", g_prog.cur}, {"total", g_prog.total},
                      {"msg", g_prog.msg}, {"result", g_prog.result}};
            respond(s, "200 OK", "application/json", j.dump());
        }
        else if (p == "/quit")
        {
            respond(s, "200 OK", "text/plain", "bye");
            // Exit outright: the accept loop is blocked and other connection threads may
            // still hold the server, so a normal return would tear it down under them.
            cout << "Director stopped.\n" << flush;
            std::thread([]() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); std::_Exit(0); }).detach();
        }
        else respond(s, "404 Not Found", "text/plain", "not found");
    }
};

static int runServer(int port, const string &initVideo)
{
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    static Server srv;                         // outlives the detached connection threads
    if (!initVideo.empty())
    {
        string err = srv.load(initVideo);
        if (!err.empty()) cerr << err << "\n";
    }
    socket_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) { cerr << "socket() failed\n"; return 1; }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int bound = -1;
    for (int p = port; p < port + 10; ++p)
    {
        addr.sin_port = htons((unsigned short)p);
        if (::bind(ls, (sockaddr *)&addr, sizeof(addr)) == 0) { bound = p; break; }
    }
    if (bound < 0 || listen(ls, 32) != 0) { cerr << "Could not bind a port\n"; CLOSESOCK(ls); return 1; }
    string url = "http://127.0.0.1:" + to_string(bound) + "/";
    cout << "Director running at " << url << "  (Ctrl+C or Quit to stop)\n";
    if (pagePath().empty()) cerr << "warning: director.html not found next to the executable or in " << DIRECTOR_SRC_DIR << "\n";
    openBrowser(url);
    for (;;)
    {
        socket_t c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
#ifdef SO_NOSIGPIPE
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&yes, sizeof(yes));
#endif
        std::thread([c]() { srv.handle(c); }).detach();
    }
}

int main(int argc, char **argv)
{
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);   // a closed browser socket or encoder pipe must not kill us
#endif
    string proj = argVal(argc, argv, "--render", "");
    if (!proj.empty())
    {
        json j;
        try { j = json::parse(readFile(proj)); }
        catch (...) { cerr << "cannot read project " << proj << "\n"; return 1; }
        string vid = argVal(argc, argv, "--video", "");
        if (vid.empty())                                    // project sits next to its video
            vid = (fs::path(proj).parent_path() / j.value("video", string())).string();
        VideoInfo v = probeVideo(vid);
        if (!v.ok()) { cerr << "cannot read video " << vid << "\n"; return 1; }
        g_vencOverride = argVal(argc, argv, "--venc", "");
        g_vencOpts = argVal(argc, argv, "--vopts", "");
        RenderOpts o;
        o.out = argVal(argc, argv, "--out", (fs::path(vid).parent_path() / (fs::path(vid).stem().string() + "_edit.mp4")).string());
        o.perPoint = hasArg(argc, argv, "--per-point");
        o.unframedWide = argVal(argc, argv, "--unframed", "skip") == "wide";
        o.codec = argVal(argc, argv, "--codec", "h264");
        o.quality = argVal(argc, argv, "--quality", "high");
        std::atomic<bool> fin{false};
        std::thread t([&]() {
            while (!fin) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                lock_guard<mutex> lk(g_prog.mu);
                if (g_prog.total) cerr << "\r  " << g_prog.cur << "/" << g_prog.total << " frames  " << g_prog.msg << "      ";
            }
        });
        string res = renderProject(j, v, o);
        fin = true; t.join();
        cerr << "\n";
        cout << res << "\n";
        return res.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    return runServer(stoi(argVal(argc, argv, "--port", "8091")), argVal(argc, argv, "--video", ""));
}
