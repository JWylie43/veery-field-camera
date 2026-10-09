// director.cpp - renders a Director edit: the virtual-camera cut of a stitched panorama.
//
// The edit itself is made in Studio's editor page (studio/, /edit/<video>):
//   1. Cut   - mark each point of the game with In / Out.
//   2. Frame - per point, steer a 16:9 box over the full-width panorama while it
//              plays. One take covers the whole point (In..Out).
// The page saves the edit next to the video (<video>.director.json). This program
// renders it: it crops the box out of every frame of every point and encodes the
// result at the output size (default 2560x1440) - one video with the points in
// order, or one file per point. Studio runs it as a render job.
//
// Usage:
//   Director --render project.json --out edit.mp4 [--video stitched.mp4] [--per-point]
//            [--unframed skip|wide] [--codec h264|hevc] [--quality high|standard]
// Progress goes to stderr every 2 s ("  cur/total frames  point"), the output
// path(s) to stdout. Needs ffmpeg + ffprobe on PATH (frame-accurate decode, encode).

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


using json = nlohmann::json;
using namespace std;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- small helpers

static string runCapture(const string &cmd)
{
    string out;
    FILE *p = popen(cmd.c_str(), "r");
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
    return " >/dev/null 2>&1";
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
    long long total = 0, cur = 0;
    string msg;
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
    return popen(c.str().c_str(), "r");
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
        enc = popen(encodeCmd(OW, OH, v.fps, file, o).c_str(), "w");
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

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);   // a closed encoder pipe must not kill us
    string proj = argVal(argc, argv, "--render", "");
    if (proj.empty())
    {
        cerr << "usage: Director --render project.json --out edit.mp4 [--video stitched.mp4] [--per-point]\n"
             << "       [--unframed skip|wide] [--codec h264|hevc] [--quality high|standard]\n"
             << "       (the edit is made in Studio's editor - see studio/README.md)\n";
        return 2;
    }
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
