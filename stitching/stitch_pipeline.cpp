// stitch_pipeline.cpp - calibration-driven cylindrical stitch (C++), NO feature detection.
//
// Reads the Veery rig's calibration (cam0/cam1 fisheye intrinsics + stereo
// extrinsics from calibration/) and stitches the two camera feeds into a
// cylindrical panorama, aligning them from the extrinsic rotation R. No BRISK /
// matcher / findHomography anywhere.
//
// EVERY input is a PAIR - the rig writes one file per camera and this stitcher has
// no single-file mode. Pass EITHER file of a pair and its partner is found next to
// it, or give both explicitly as "cam0path::cam1path". See "dual input" below.
//
// The calibration must be FISHEYE (equidistant). The lenses are 110 deg with -16%
// barrel; a pinhole+polynomial fit leaves a uniform ~2.6px residual, so
// calibrate.py writes "model":"fisheye" and loadIntrinsics rejects anything else.
//
// Modes:
//   image source (.jpg/.png/...) -> stitch the one pose      -> pano.jpg
//   video source (.mp4/.mkv/...) -> loop frames [start..end] -> stitched_video.mp4
//   --tune  -> launch an interactive browser tuner (see below)
//
// cam1 alignment (folded into cam1's remap table, with the rotation and crop):
//   --shift-top N     horizontal shift of the TOP rows   (aligns the FAR edge)
//   --shift-bottom N  horizontal shift of the BOTTOM rows (aligns the NEAR edge)
//   --shift-y N       vertical shift of the whole image
// If top != bottom this is a vertical SHEAR: the per-row horizontal shift is
// interpolated between the two, so a receding field (near at the bottom, far at the
// top) lines up along a straight vertical seam. (--shift-x N sets top=bottom=N.)
//
// --tune warps the first frame once, starts a localhost web server, opens a browser
// to a live tuner where you adjust those values and click "Stitch all frames" to run
// the full stitch (progress bar + done). One command; UI opens itself.
//
// Video renders as a pipeline in ONE process (see FramePipeline): decode -> remap +
// exposure -> seam -> blend -> encode, several frames in flight, worker threads taking
// whichever stage has work. The smart seam is one continuous chain over the whole
// range, and there is a single output file - nothing to split or join.
//
// Two-file takes (the recorder writes one file per camera):
//   --source take_..._cam0.mkv   (or _cam1.mkv) finds the partner and pairs them in memory.
//   --pair-offset auto  (DEFAULT) takes the frame offset from the files' capture
//              timestamps when the recorder tagged them as one shared-clock take
//              (exact). Otherwise (older takes) it estimates it by
//              cross-correlating per-frame brightness over the first seconds - the same
//              method as pair_check.py, so no separate step is needed. Prints the
//              offset, its correlation and its margin. --pair-offset N pins a value
//              (0 disables). While the cameras FREE-RUN no single offset is right for a
//              whole take (they drift apart); with XVS genlock it is a true constant.
//
// Output size:
//   The panorama size is DERIVED from the calibration, not configured: the cylinder's
//   radius in pixels equals cam0's focal length, so the centre of frame is sampled
//   about 1:1. That is why the numbers look arbitrary (2104px focal -> 6774 wide).
//   --scale F  renders the cylinder at F times that radius: SAME field of view, fewer
//              pixels - it lowers pixel density, it does not crop (that is --crop).
//              Implemented by shrinking the radius before the maps are built, so the
//              frame is rendered once at the smaller size rather than downscaled after.
//   ffmpeg on PATH is the encode path (hardware HEVC/H.264); without it OpenCV's writer is used.
//
// Warp device: the stitching/warp always runs on the CPU (the OpenCL/GPU warp was
// measured slower - see the note in main). The video ENCODER still uses the GPU
// (hardware HEVC, chooseVideoEncoder). Uses core/imgproc/imgcodecs/videoio; OpenCV 4.x/5.x.
//
// Encoding (changed 2026-09-19): HEVC/H.265 at every size, tagged hvc1, with the
// bitrate defaulting to "auto" - sized from the output pixel rate at ~0.20 bpp rather
// than a fixed number, so a crop or a --scale gets a sensible rate by itself.
// --bitrate 90M still pins an explicit rate. H.264 remains only as a fallback.
//
// Build:  cmake -S . -B build && cmake --build build

#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <csignal>
#include "json.hpp"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>          // GetModuleFileNameA (locate our own exe)
  using socket_t = SOCKET;
  #define CLOSESOCK closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using socket_t = int;
  #define CLOSESOCK close
  #define INVALID_SOCKET (-1)
#endif

#ifdef __APPLE__
  #include <mach-o/dyld.h>      // _NSGetExecutablePath (locate our own exe)
#endif

#ifdef _WIN32
  #define popen _popen
  #define pclose _pclose
  #define PIPE_WMODE "wb"        // binary: Windows text mode would mangle raw frames with CRLF
#else
  #define PIPE_WMODE "w"         // POSIX popen only takes "r"/"w"; no 'b' flag
#endif

using json = nlohmann::json;
using namespace std;
using namespace cv;
namespace fs = std::filesystem;

static int runShell(string cmd);   // forward decl (defined near main); the encoder helpers below use it

// ---- video-encoder selection (set from CLI in main) ---------------------
// The stitch always re-encodes, so we hand raw frames to ffmpeg and let it use a
// hardware H.264 encoder when one is available - detected generically, not tied to
// any specific GPU. See chooseVideoEncoder().
static string g_vencExplicit;      // --venc NAME: force a specific encoder
static bool   g_forceCpu = false;  // --cpu / --no-hwenc: force software libx264
static string g_vbitrate  = "auto"; // --bitrate: explicit rate (e.g. "90M"), or "auto"
// "auto" sizes the bitrate from the OUTPUT pixel rate instead of a fixed number, so a
// crop, a --scale or a different rig all get a sensible rate without being re-tuned.
// Target bits-per-pixel: Joe's VMAF runs put good H.264 at ~0.26 bpp (25M on the old
// 2660x1199 pano). HEVC buys ~40% at equal quality, so 0.20 bpp HEVC sits a little
// ABOVE that validated point - right for a master that gets re-encoded downstream.
static double bppTarget(const string &venc)
{
    bool hevc = venc.find("hevc") != string::npos || venc.find("265") != string::npos;
    return hevc ? 0.20 : 0.30;
}
static string resolveBitrate(const string &venc, int W, int H, double fps)
{
    if (g_vbitrate != "auto") return g_vbitrate;
    if (fps <= 0) fps = 30.0;
    double bps = (double)W * H * fps * bppTarget(venc);
    long mbit = lround(bps / 1e6);
    mbit = max(8L, min(200L, mbit));          // sane floor/ceiling
    return to_string(mbit) + "M";
}

// A camera's projection model: maps cylinder points back to its source pixels.
struct CamModel
{
    double fx = 0, fy = 0, cx = 0, cy = 0;
    double r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // camera-from-left rotation, row-major
    vector<double> D;
    int w = 0, h = 0;                            // source frame size
};

// The full cylindrical canvas: its geometry (so any canvas point can be re-projected
// into either camera) plus float remap tables over the whole canvas, which the tuner's
// live preview draws from.
struct StitchMaps
{
    Mat mapLx, mapLy, mapRx, mapRy;
    int OW = 0, OH = 0;
    int seam = 0;
    int ox0 = 0, ox1 = 0;   // overlap column range [ox0, ox1) where both cameras are valid
    CamModel camL, camR;
    double fcyl = 1, thetaMin = 0;   // canvas column x <-> angle thetaMin + x / fcyl
};

// The output view, in full-canvas coordinates: rotate the whole canvas by `degrees`
// (clockwise as shown) about its centre, then cut out the box. w/h <= 0 = whole canvas.
struct ViewBox
{
    int x = 0, y = 0, w = 0, h = 0;
    double degrees = 0;
};

// Per-render remap tables (see buildRenderMaps): shear + rotation + crop folded into
// ONE table per camera, so a frame is one remap per camera straight into output pixels.
// Each camera only covers the columns it can contribute to - left [0, sx1), right
// [sx0, OW) - where [sx0, sx1) is the strip the seam is blended across.
struct RenderMaps
{
    int OW = 0, OH = 0;           // output (crop box) size
    Mat mapL1, mapL2;             // left table over [0, sx1)   (fixed point)
    Mat mapR1, mapR2;             // right table over [sx0, OW) (fixed point)
    int sx0 = 0, sx1 = 0;         // blend strip (widest the seam can need)
    int margin = 0;               // blend reach either side of the seam (from the band count)
    int bx0 = 0, bx1 = 0;         // seam band: columns where both cameras overlap on some row
    Mat overlap;                  // CV_8U over the band: 1 where both cameras are valid
    vector<float> home;           // per row: the seam's home column in output coordinates
};

// Right-image alignment (per-row horizontal shear + vertical shift) + seam blend.
struct Align
{
    double shiftTop = 0, shiftBottom = 0, shiftY = 0;
    int bands = 0;       // multi-band (Laplacian) blend levels; 0 = hard seam
    bool exposure = false; // match right image brightness/color to left (per channel)
    bool smartSeam = false; // route the seam around moving objects (min-difference path)
};

// Shared progress state for the --tune server (stitch runs on a worker thread).
static std::atomic<int> g_percent{0};
static std::atomic<bool> g_busy{false};
static std::atomic<bool> g_done{false};
static std::mutex g_mu;
static string g_result;
// The exact equivalent CLI command for the last/active stitch (shown in the tuner UI
// and console) so you can reproduce a tuned render manually.
static string g_cmd;

// The rig's CIL391 lenses (110 deg, -16% barrel) are fisheyes and only fit the
// equidistant model - a pinhole+polynomial fit leaves a uniform ~2.6px residual.
// calibrate.py therefore writes "model":"fisheye" into every intrinsics file and
// this stitcher accepts nothing else: a mismatched model does not fail loudly, it
// silently yields a plausible-looking panorama built from the wrong projection.
static void loadIntrinsics(const string &path, Mat &K, vector<double> &D)
{
    ifstream f(path);
    if (!f.is_open()) { cerr << "Cannot open " << path << endl; exit(1); }
    json j; f >> j;
    if (!j.contains("model") || j["model"].get<string>() != "fisheye")
    {
        cerr << "ERROR: " << path << " is not a fisheye calibration "
             << "(missing \"model\": \"fisheye\").\n"
             << "       This stitcher is equidistant-only. Re-solve it with "
             << "calibration/calibrate.py.\n";
        exit(1);
    }
    K = Mat::eye(3, 3, CV_64F);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            K.at<double>(r, c) = j["camera_matrix"][r][c].get<double>();
    D.clear();
    for (auto &v : j["distortion_coefficients"])
        D.push_back(v.get<double>());
}

static Mat loadRotation(const string &path)
{
    ifstream f(path);
    if (!f.is_open()) { cerr << "Cannot open " << path << endl; exit(1); }
    json j; f >> j;
    Mat R(3, 3, CV_64F);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            R.at<double>(r, c) = j["rotation_matrix"][r][c].get<double>();
    return R;
}

// Equidistant (Kannala-Brandt) projection: radius grows with the ANGLE, not its
// tangent, which is what a fisheye lens actually does. Mirrors cv::fisheye.
static inline void applyFisheye(double x, double y, const vector<double> &D,
                                double &xd, double &yd)
{
    double k1 = D.size() > 0 ? D[0] : 0, k2 = D.size() > 1 ? D[1] : 0;
    double k3 = D.size() > 2 ? D[2] : 0, k4 = D.size() > 3 ? D[3] : 0;
    double r = sqrt(x * x + y * y);
    if (r < 1e-12) { xd = x; yd = y; return; }
    double th = atan(r);
    double t2 = th * th;
    double thd = th * (1 + k1 * t2 + k2 * t2 * t2 + k3 * t2 * t2 * t2 + k4 * t2 * t2 * t2 * t2);
    double scale = thd / r;
    xd = x * scale;
    yd = y * scale;
}

static CamModel camModel(const Mat &K, const vector<double> &D, const Mat &R_cam_from_left, int w, int h)
{
    CamModel c;
    c.fx = K.at<double>(0, 0); c.fy = K.at<double>(1, 1);
    c.cx = K.at<double>(0, 2); c.cy = K.at<double>(1, 2);
    for (int i = 0; i < 9; i++) c.r[i] = R_cam_from_left.at<double>(i / 3, i % 3);
    c.D = D; c.w = w; c.h = h;
    return c;
}

// Cylinder point (angle th, height hh) -> source pixel (u, v) of one camera. False if
// the ray is behind the camera or lands outside its frame.
static inline bool cylToCam(const CamModel &c, double th, double hh, float &u, float &v)
{
    double dx = sin(th), dy = hh, dz = cos(th);
    const double *r = c.r;
    double cxr = r[0] * dx + r[1] * dy + r[2] * dz;
    double cyr = r[3] * dx + r[4] * dy + r[5] * dz;
    double czr = r[6] * dx + r[7] * dy + r[8] * dz;
    if (czr <= 1e-6) return false;
    double xd, yd;
    applyFisheye(cxr / czr, cyr / czr, c.D, xd, yd);
    double uu = c.fx * xd + c.cx, vv = c.fy * yd + c.cy;
    if (uu < 0 || uu >= c.w || vv < 0 || vv >= c.h) return false;
    u = (float)uu; v = (float)vv;
    return true;
}

static void buildCylMap(const CamModel &c, const vector<double> &theta, const vector<double> &hval,
                        Mat &mapx, Mat &mapy, Mat &valid)
{
    int OW = (int)theta.size(), OH = (int)hval.size();
    mapx.create(OH, OW, CV_32F);
    mapy.create(OH, OW, CV_32F);
    valid = Mat::zeros(OH, OW, CV_8U);
    parallel_for_(Range(0, OH), [&](const Range &rows) {
        for (int yy = rows.start; yy < rows.end; yy++)
        {
            float *mx = mapx.ptr<float>(yy), *my = mapy.ptr<float>(yy);
            uchar *vv = valid.ptr<uchar>(yy);
            for (int xx = 0; xx < OW; xx++)
            {
                if (cylToCam(c, theta[xx], hval[yy], mx[xx], my[xx])) vv[xx] = 1;
                else mx[xx] = my[xx] = -1.f;
            }
        }
    });
}

// --scale renders the cylinder at a lower angular resolution (smaller radius)
// rather than downscaling a full-size render: same field of view, fewer pixels.
static double g_scale = 1.0;

static StitchMaps buildStitchMaps(const Mat &KL, const vector<double> &DL,
                                  const Mat &KR, const vector<double> &DR,
                                  const Mat &R, int w, int h, int seamArg)
{
    StitchMaps m;
    double fcyl = KL.at<double>(0, 0) * g_scale;
    // equidistant half-FOV: the edge angle is simply (w/2)/f radians
    double halfL = w / (2 * KL.at<double>(0, 0));
    double halfR = w / (2 * KR.at<double>(0, 0));
    double yawR = atan2(R.at<double>(2, 0), R.at<double>(2, 2));
    double pad = 3.0 * CV_PI / 180.0;
    double thetaMin = min(-halfL, yawR - halfR) - pad;
    double thetaMax = max(halfL, yawR + halfR) + pad;
    double vhalf = h / (2 * KL.at<double>(1, 1));
    m.OW = min((int)((thetaMax - thetaMin) * fcyl), 12000);
    m.OH = min((int)(2 * tan(vhalf) * fcyl), 4000);
    vector<double> theta(m.OW), hval(m.OH);
    for (int i = 0; i < m.OW; i++) theta[i] = thetaMin + i / fcyl;
    for (int i = 0; i < m.OH; i++) hval[i] = (i - m.OH / 2.0) / fcyl;

    m.camL = camModel(KL, DL, Mat::eye(3, 3, CV_64F), w, h);
    m.camR = camModel(KR, DR, R, w, h);
    m.fcyl = fcyl; m.thetaMin = thetaMin;
    Mat okL, okR;
    buildCylMap(m.camL, theta, hval, m.mapLx, m.mapLy, okL);
    buildCylMap(m.camR, theta, hval, m.mapRx, m.mapRy, okR);

    vector<int> overlapCols;
    for (int x = 0; x < m.OW; x++)
    {
        bool lc = false, rc = false;
        for (int y = 0; y < m.OH && !(lc && rc); y++)
        { if (okL.at<uchar>(y, x)) lc = true; if (okR.at<uchar>(y, x)) rc = true; }
        if (lc && rc) overlapCols.push_back(x);
    }
    m.seam = seamArg >= 0 ? seamArg
             : (overlapCols.empty() ? m.OW / 2 : overlapCols[overlapCols.size() / 2]);
    m.ox0 = overlapCols.empty() ? 0 : overlapCols.front();
    m.ox1 = overlapCols.empty() ? m.OW : overlapCols.back() + 1;

    cout << "panorama " << m.OW << "x" << m.OH
         << ", right yaw " << yawR * 180.0 / CV_PI << " deg, hard seam @ " << m.seam
         << (overlapCols.empty() ? "  [!! no overlap]" : "")
         << (g_scale != 1.0 ? "  (--scale " + to_string(g_scale).substr(0, 4) + ")" : "")
         << "\n";
    // The size above is DERIVED, not chosen: the cylinder's radius in pixels is the
    // left camera's focal length, so the pano samples the source ~1:1 at frame centre.
    // Past 4096 wide, hardware H.264 is out and we fall back to HEVC - which most
    // browsers and YouTube uploads would rather not have.
    if (m.OW > 4096 && g_scale == 1.0)
        cout << "  note: > 4096 wide, so this encodes as HEVC. --scale "
             << std::fixed << std::setprecision(2) << (4096.0 / m.OW)
             << std::defaultfloat << " keeps the full field of view at "
             << (int)(m.OW * (4096.0 / m.OW)) << "x" << (int)(m.OH * (4096.0 / m.OW))
             << " and re-enables H.264. (So does a --crop under 4096 wide - the crop is\n"
             << "  applied before the encoder is chosen, so it counts against the limit.)\n";
    return m;
}

// Clamp a view box to the canvas; an empty box means the whole canvas.
static ViewBox clampBox(const StitchMaps &m, ViewBox b)
{
    if (b.w <= 0 || b.h <= 0) { b.x = 0; b.y = 0; b.w = m.OW; b.h = m.OH; }
    b.x = max(0, min(b.x, m.OW - 1));
    b.y = max(0, min(b.y, m.OH - 1));
    b.w = max(1, min(b.w, m.OW - b.x));
    b.h = max(1, min(b.h, m.OH - b.y));
    return b;
}

// Build the render tables for one output view. Each output pixel's source is found by
// walking the whole chain backwards in one go:
//     output box -> undo the rotation (about the canvas centre) -> undo cam1's
//     shear/shift (right only) -> cylinder -> fisheye -> source pixel
// so a frame is resampled exactly ONCE, and only the box is ever rendered - rotating
// no longer means stitching a larger region and warping it again.
//
// The seam's home column is a vertical line on the canvas: it belongs to the cameras
// (where their views overlap), not to the output. It is carried through the same
// rotation, so in the output it tilts with the picture and the seam stays on the same
// camera-relative line at any angle - exactly what the tuner preview draws.
static RenderMaps buildRenderMaps(const StitchMaps &m, ViewBox b, const Align &a)
{
    b = clampBox(m, b);
    RenderMaps r;
    r.OW = b.w; r.OH = b.h;
    const int OW = r.OW, OH = r.OH;
    const double ph = b.degrees * CV_PI / 180.0, c = cos(ph), s = sin(ph);
    const double CX = m.OW / 2.0, CY = m.OH / 2.0, h0 = m.OH / 2.0;
    // cam1 shear: content at canvas (X, Y) moves to (X + shiftTop + k*Y, Y + shiftY).
    // The slope is defined over the full canvas height, so it is the same cropped or not.
    const double k = (m.OH > 1) ? (a.shiftBottom - a.shiftTop) / (m.OH - 1) : 0.0;

    Mat lx(OH, OW, CV_32F), ly(OH, OW, CV_32F), rx(OH, OW, CV_32F), ry(OH, OW, CV_32F);
    Mat okL = Mat::zeros(OH, OW, CV_8U), okR = Mat::zeros(OH, OW, CV_8U);
    parallel_for_(Range(0, OH), [&](const Range &rows) {
        for (int v = rows.start; v < rows.end; v++)
        {
            float *plx = lx.ptr<float>(v), *ply = ly.ptr<float>(v);
            float *prx = rx.ptr<float>(v), *pry = ry.ptr<float>(v);
            uchar *ol = okL.ptr<uchar>(v), *orr = okR.ptr<uchar>(v);
            double py = b.y + v - CY;
            for (int u = 0; u < OW; u++)
            {
                double px = b.x + u - CX;
                // output point shows the un-rotated canvas point C + A(P - C), A = [c s; -s c]
                double X = CX + c * px + s * py, Y = CY - s * px + c * py;
                if (cylToCam(m.camL, m.thetaMin + X / m.fcyl, (Y - h0) / m.fcyl, plx[u], ply[u])) ol[u] = 1;
                else plx[u] = ply[u] = -1.f;
                double Yr = Y - a.shiftY, Xr = X - a.shiftTop - k * Yr;
                if (cylToCam(m.camR, m.thetaMin + Xr / m.fcyl, (Yr - h0) / m.fcyl, prx[u], pry[u])) orr[u] = 1;
                else prx[u] = pry[u] = -1.f;
            }
        }
    });

    // Seam home line: canvas column m.seam, in output coordinates (one column per row).
    r.home.resize(OH);
    for (int v = 0; v < OH; v++)
        r.home[v] = (float)((m.seam - CX - s * (b.y + v - CY)) / c + CX - b.x);
    auto hm = minmax_element(r.home.begin(), r.home.end());
    int hx0 = max(0, min(OW - 1, (int)floor(*hm.first)));
    int hx1 = max(hx0 + 1, min(OW, (int)ceil(*hm.second) + 1));

    // Seam search band: columns where both cameras are valid on at least one row.
    Mat both = okL & okR, colAny;
    cv::reduce(both, colAny, 0, REDUCE_MAX);
    int bx0 = OW, bx1 = 0;
    for (int u = 0; u < OW; u++) if (colAny.at<uchar>(0, u)) { bx0 = min(bx0, u); bx1 = u + 1; }
    if (bx1 <= bx0) { bx0 = hx0; bx1 = hx1; }   // no overlap: a hard cut on the home line
    r.bx0 = bx0; r.bx1 = bx1;
    r.overlap = both.colRange(bx0, bx1).clone();

    // Blend strip. Where the mask is constant across a level's whole kernel footprint
    // the Laplacian blend reproduces the source exactly, so only the band (plus a
    // margin wide enough for the coarsest level to settle) is ever blended; outside it
    // each side is just its own camera. So each camera only needs rendering up to the
    // far edge of the strip.
    int margin = a.bands > 0 ? (4 << max(2, min(a.bands, 8))) : 0;
    r.margin = margin;
    r.sx0 = max(0, min(bx0, hx0) - margin);
    r.sx1 = min(OW, max(bx1, hx1) + margin);
    cout << "render " << OW << "x" << OH << ": overlap band " << bx0 << ".." << bx1
         << ", blend strip " << r.sx0 << ".." << r.sx1 << " (" << (r.sx1 - r.sx0) << " px)\n";

    // Fixed-point tables: OpenCV's remap is markedly faster with them (1/32 px steps).
    convertMaps(lx.colRange(0, r.sx1), ly.colRange(0, r.sx1), r.mapL1, r.mapL2, CV_16SC2);
    convertMaps(rx.colRange(r.sx0, OW), ry.colRange(r.sx0, OW), r.mapR1, r.mapR2, CV_16SC2);
    return r;
}

// Tuner preview: each camera warped onto the full (un-rotated, un-sheared) canvas; the
// browser applies shear/rotation/crop live on top.
static void warpPreview(const Mat &fL, const Mat &fR, const StitchMaps &m, Mat &wL, Mat &wR)
{
    remap(fL, wL, m.mapLx, m.mapLy, INTER_LINEAR, BORDER_CONSTANT);
    remap(fR, wR, m.mapRx, m.mapRy, INTER_LINEAR, BORDER_CONSTANT);
}

// Seam path for this frame: per row, the output column where the cut sits (left of it
// = left camera). Smart seam: min-cost top-to-bottom path through the KNOWN overlap so
// the cut weaves AROUND moving objects. Cost = image difference + a pull toward the
// home line (the tuner's draggable bar, else the overlap centre - tilted with any
// rotation) + a temporal term (stick to the previous frame's seam) so wind/noise
// doesn't make the seam jitter frame-to-frame - it only moves when a player forces it.
// The home pull only sets where the seam sits through flat regions. L/R are the
// strip-local camera images.
static vector<int> seamPath(const Mat &L, const Mat &R, const RenderMaps &rm, bool smart,
                            vector<int> &prevSeam)
{
    const int OH = rm.OH, x0 = rm.bx0 - rm.sx0, bw = rm.bx1 - rm.bx0;
    vector<int> cut(OH);
    if (!smart || bw < 4)
    {
        for (int y = 0; y < OH; y++) cut[y] = max(rm.sx0, min(rm.sx1, (int)lround(rm.home[y])));
        return cut;
    }
    const float CB = 0.08f;   // pull toward the home line
    const float TW = 0.8f;    // temporal stickiness
    bool temporal = ((int)prevSeam.size() == OH);

    Mat gL, gR, cost;
    cvtColor(L.colRange(x0, x0 + bw), gL, COLOR_BGR2GRAY);
    cvtColor(R.colRange(x0, x0 + bw), gR, COLOR_BGR2GRAY);
    absdiff(gL, gR, cost); cost.convertTo(cost, CV_32F);
    cost.setTo(1e6f, rm.overlap == 0);      // keep the seam inside the real overlap
    for (int y = 0; y < OH; y++)
    {
        float *cp = cost.ptr<float>(y);
        float center = max((float)rm.bx0, min((float)(rm.bx1 - 1), rm.home[y]));
        for (int x = 0; x < bw; x++)
        {
            float gx = (float)(rm.bx0 + x);
            cp[x] += CB * fabsf(gx - center);
            if (temporal) cp[x] += TW * fabsf(gx - (float)prevSeam[y]);
        }
    }
    Mat M = cost.clone(), back(OH, bw, CV_32S);
    for (int y = 1; y < OH; y++)
    {
        const float *mp = M.ptr<float>(y - 1);
        float *mc = M.ptr<float>(y);
        int *bk = back.ptr<int>(y);
        for (int x = 0; x < bw; x++)
        {
            float best = mp[x]; int bx = x;
            if (x > 0 && mp[x - 1] < best) { best = mp[x - 1]; bx = x - 1; }
            if (x < bw - 1 && mp[x + 1] < best) { best = mp[x + 1]; bx = x + 1; }
            mc[x] += best; bk[x] = bx;
        }
    }
    int cur = 0;
    { const float *last = M.ptr<float>(OH - 1); for (int x = 1; x < bw; x++) if (last[x] < last[cur]) cur = x; }
    for (int y = OH - 1; y >= 0; y--)
    {
        cut[y] = rm.bx0 + cur;
        if (y > 0) cur = back.ptr<int>(y)[cur];
    }
    prevSeam = cut;
    return cut;
}

// pyrUp split into row bands across threads (OpenCV runs it on one thread). Each band
// is upsampled from its own source rows plus a 2-row apron and the apron is cropped
// off, so the result is identical to a single pyrUp.
static void pyrUpPar(const Mat &src, Mat &dst, Size dsz)
{
    int n = max(1, min(getNumThreads(), src.rows / 32));
    if (n <= 1) { pyrUp(src, dst, dsz); return; }
    dst.create(dsz, src.type());
    const int odd = 2 * src.rows - dsz.height;   // 1 when the output height is odd
    parallel_for_(Range(0, n), [&](const Range &r) {
        for (int t = r.start; t < r.end; t++)
        {
            int y0 = src.rows * t / n, y1 = src.rows * (t + 1) / n;
            int a0 = max(0, y0 - 2), a1 = min(src.rows, y1 + 2);
            int th = 2 * (a1 - a0) - (a1 == src.rows ? odd : 0);
            Mat tmp;
            pyrUp(src.rowRange(a0, a1), tmp, Size(dsz.width, th));
            int d0 = 2 * y0, d1 = min(dsz.height, 2 * y1);
            tmp.rowRange(d0 - 2 * a0, d1 - 2 * a0).copyTo(dst.rowRange(d0, d1));
        }
    });
}

// One level of the blend, fused: out = B + w*(A - B), with w the mask in 1/256ths
// (0 = right, 256 = left). One pass reading the 1-channel mask once per pixel and
// applying it to all three channels. Exact at w = 0 / 256.
static void blendLevel16(const Mat &A, const Mat &B, const Mat &W, Mat &out)
{
    out.create(A.size(), CV_16SC3);
    parallel_for_(Range(0, A.rows), [&](const Range &r) {
        for (int y = r.start; y < r.end; y++)
        {
            const short *a = A.ptr<short>(y), *b = B.ptr<short>(y), *w = W.ptr<short>(y);
            short *o = out.ptr<short>(y);
            for (int x = 0; x < A.cols; x++)
            {
                int wt = w[x];
                for (int c = 0; c < 3; c++)
                {
                    int i = 3 * x + c;
                    o[i] = (short)(b[i] + (((a[i] - b[i]) * wt + 128) >> 8));
                }
            }
        }
    });
}

// Multi-band (Laplacian pyramid) blend of A (left) and B (right) across the seam mask
// W (CV_16S, 0 = right .. 256 = left). Low frequencies blend over a wide band (smooth
// tone) and high frequencies over a narrow band (edges stay sharp) - no ghosting/blur,
// unlike a linear feather.
//
// Integer throughout: the Gaussian levels stay 8-bit and the Laplacian (detail) levels
// are 16-bit, since detail is always within -255..255. Half the memory traffic of
// float and twice the values per SIMD instruction. Where the mask is constant the
// pyramid collapses back to the source exactly (same integer rounding both ways).
static Mat multiBandBlend(const Mat &A8, const Mat &B8, const Mat &W, int bands)
{
    bands = max(2, min(bands, 8));
    vector<Mat> gA{A8}, gB{B8}, gW{W};
    for (int i = 1; i < bands; i++)
    {
        Mat da, db, dw;
        pyrDown(gA[i - 1], da); pyrDown(gB[i - 1], db); pyrDown(gW[i - 1], dw);
        gA.push_back(da); gB.push_back(db); gW.push_back(dw);
    }
    // coarsest level: the Gaussian images themselves
    Mat ta, tb, res;
    gA[bands - 1].convertTo(ta, CV_16S);
    gB[bands - 1].convertTo(tb, CV_16S);
    blendLevel16(ta, tb, gW[bands - 1], res);
    for (int i = bands - 2; i >= 0; i--)
    {
        // Laplacian (detail) of each image at this level, blended, then added onto the
        // upsampled coarser result - building and collapsing in a single sweep.
        Mat ua, ub, la, lb, lev, up;
        pyrUpPar(gA[i + 1], ua, gA[i].size()); subtract(gA[i], ua, la, noArray(), CV_16S);
        pyrUpPar(gB[i + 1], ub, gB[i].size()); subtract(gB[i], ub, lb, noArray(), CV_16S);
        blendLevel16(la, lb, gW[i], lev);
        pyrUpPar(res, up, lev.size());
        add(up, lev, res);
    }
    Mat out; res.convertTo(out, CV_8UC3);
    return out;
}

// Per-channel gain so the right image's brightness/color matches the left, measured
// where both cameras see the scene near the home line (strip-local L/R). Returned as
// a LUT so applying it to the whole right image is a single pass.
static bool exposureLut(const Mat &L, const Mat &R, const RenderMaps &rm, Mat &lut)
{
    int W = min(200, rm.OW / 8);
    auto hm = minmax_element(rm.home.begin(), rm.home.end());
    int x0 = max(rm.bx0, (int)floor(*hm.first) - W), x1 = min(rm.bx1, (int)ceil(*hm.second) + W);
    if (x1 - x0 < 2) return false;
    Mat valid = rm.overlap.colRange(x0 - rm.bx0, x1 - rm.bx0);
    if (countNonZero(valid) < 100) return false;
    Rect band(x0 - rm.sx0, 0, x1 - x0, rm.OH);
    Scalar meanL = mean(L(band), valid), meanR = mean(R(band), valid);
    lut.create(1, 256, CV_8UC3);
    for (int c = 0; c < 3; c++)
    {
        double g = meanR[c] > 1e-3 ? meanL[c] / meanR[c] : 1.0;
        g = max(0.3, min(3.0, g));             // clamp to avoid extreme corrections
        for (int i = 0; i < 256; i++) lut.at<Vec3b>(0, i)[c] = saturate_cast<uchar>(i * g);
    }
    return true;
}

// One frame's working state as it moves through the stages below (and through the
// video pipeline, which runs the stages on different frames at once).
struct FrameWork
{
    Mat fL, fR;          // decoded camera frames
    Mat out;             // output frame; the left camera is rendered straight into it
    Mat wR;              // right camera, output columns [sx0, OW)
    vector<int> cut;     // seam column per row
};

// Stage 1 (any order): each camera remapped ONCE, only over the columns it contributes
// to, straight into output coordinates (already sheared, rotated and cropped); then
// the right camera's exposure matched to the left.
static void remapStage(FrameWork &f, const RenderMaps &rm, const Align &a)
{
    f.out.create(rm.OH, rm.OW, CV_8UC3);
    Mat outL = f.out.colRange(0, rm.sx1);
    remap(f.fL, outL, rm.mapL1, rm.mapL2, INTER_LINEAR, BORDER_CONSTANT);
    remap(f.fR, f.wR, rm.mapR1, rm.mapR2, INTER_LINEAR, BORDER_CONSTANT);
    Mat lut;
    if (a.exposure
        && exposureLut(f.out.colRange(rm.sx0, rm.sx1), f.wR.colRange(0, rm.sx1 - rm.sx0), rm, lut))
        LUT(f.wR, lut, f.wR);
}

// Stage 2 (strictly in frame order - the smart seam sticks to the previous frame's).
static void seamStage(FrameWork &f, const RenderMaps &rm, const Align &a, vector<int> &prevSeam)
{
    f.cut = seamPath(f.out.colRange(rm.sx0, rm.sx1), f.wR.colRange(0, rm.sx1 - rm.sx0),
                     rm, a.smartSeam, prevSeam);
}

// Stage 3 (any order): blend across this frame's seam and fill in the right camera.
// Only THIS frame's seam needs blending: left of the path's leftmost column the mask
// is all left camera, right of its rightmost all right camera, so the blend runs on
// [path min - margin, path max + margin] - usually far narrower than the strip, which
// has to allow for the seam being anywhere in the overlap.
static void blendStage(FrameWork &f, const RenderMaps &rm, const Align &a)
{
    const int OW = rm.OW, OH = rm.OH;
    auto pr = minmax_element(f.cut.begin(), f.cut.end());
    int q0 = max(rm.sx0, *pr.first - rm.margin), q1 = min(rm.sx1, *pr.second + rm.margin);
    if (q1 < OW) f.wR.colRange(q1 - rm.sx0, f.wR.cols).copyTo(f.out.colRange(q1, OW));
    if (q1 <= q0) return;
    Rect sub(q0, 0, q1 - q0, OH);
    Mat A = f.out(sub), B = f.wR(Rect(q0 - rm.sx0, 0, q1 - q0, OH));
    if (a.bands > 0)
    {
        Mat W(OH, q1 - q0, CV_16S, Scalar(0));
        for (int y = 0; y < OH; y++)
        {
            int n = max(0, min(f.cut[y] - q0, q1 - q0));
            if (n > 0) W(Rect(0, y, n, 1)).setTo(256);
        }
        multiBandBlend(A, B, W, a.bands).copyTo(A);
    }
    else                                     // hard seam: right camera from the cut onward
    {
        for (int y = 0; y < OH; y++)
        {
            int n = max(0, min(f.cut[y] - q0, q1 - q0));
            if (n < q1 - q0) B(Rect(n, y, q1 - q0 - n, 1)).copyTo(A(Rect(n, y, q1 - q0 - n, 1)));
        }
    }
}

// One whole frame, stage after stage (stills; video runs the stages as a pipeline).
static void renderFrame(FrameWork &f, const RenderMaps &rm, const Align &a, vector<int> &prevSeam)
{
    remapStage(f, rm, a);
    seamStage(f, rm, a, prevSeam);
    blendStage(f, rm, a);
}

// ---------------------------------------------------------------- frame pipeline
// Worker threads and frame slots. The hardware HEVC encoder caps a 5923x1697 render at
// ~57 fps on an M5 Pro, and 2 workers already reach it; 4 leaves headroom. More
// workers only add threads competing for memory bandwidth.
static const int PIPE_WORKERS = 4, PIPE_SLOTS = 8;

// Video runs as a pipeline, several frames in flight at once:
//
//     decode  ->  remap + exposure  ->  seam path  ->  blend  ->  encode
//     (order)     (any order)           (order)        (any)      (order)
//
// Ordered stages take frames strictly by sequence number: decode reads the files in
// sequence, the smart seam needs the previous frame's seam, and the encoder needs
// frames in order. The others take any frame that is ready, so frame 6 may be remapped
// while frame 5 still is; it then simply waits at the seam stage until 5 has passed.
//
// A fixed set of worker threads each takes whatever task is ready, preferring the
// stage nearest the encoder (finish frames before starting new ones), so workers
// drift to wherever the work is - no per-stage thread counts. Frames live in a fixed
// set of slots: decode only starts a frame when a slot is free, so memory is bounded
// and a fast stage just waits for a slow one. One continuous seam history, one output.
class FramePipeline
{
public:
    // decode(f) fills f.fL/f.fR and returns false at the end; write(f, n) consumes the
    // finished frame n (0-based, in order) and returns false to stop the run.
    FramePipeline(int slots, int workers, const RenderMaps &rm, const Align &a,
                  std::function<bool(FrameWork &)> decode,
                  std::function<bool(FrameWork &, int)> write)
        : slots_(max(2, slots)), workers_(max(1, workers)), rm_(rm), a_(a),
          decode_(std::move(decode)), write_(std::move(write)) {}

    // Runs to the end (or until write/an error stops it). Returns "" or the error.
    string run()
    {
        vector<std::thread> ts;
        for (int i = 0; i < workers_; i++) ts.emplace_back([this]() { work(); });
        for (auto &t : ts) t.join();
        return error_;
    }

private:
    // a slot's state = the last stage it completed
    enum State { FREE, DECODED, REMAPPED, SEAMED, BLENDED };
    enum Task { NONE, DECODE, REMAP, SEAM, BLEND, WRITE };
    struct Slot { FrameWork f; int seq = -1; State state = FREE; bool busy = false; };

    // Next runnable task, nearest the encoder first. Called with mu_ held.
    Task pick(int &si)
    {
        if (stop_) return NONE;
        for (int i = 0; i < (int)s_.size(); i++)
            if (!s_[i].busy && s_[i].state == BLENDED && s_[i].seq == nextWrite_) { si = i; return WRITE; }
        for (int i = 0; i < (int)s_.size(); i++)
            if (!s_[i].busy && s_[i].state == SEAMED) { si = i; return BLEND; }
        for (int i = 0; i < (int)s_.size(); i++)
            if (!s_[i].busy && s_[i].state == REMAPPED && s_[i].seq == nextSeam_) { si = i; return SEAM; }
        for (int i = 0; i < (int)s_.size(); i++)
            if (!s_[i].busy && s_[i].state == DECODED) { si = i; return REMAP; }
        if (!eof_ && !decoding_)
            for (int i = 0; i < (int)s_.size(); i++)
                if (s_[i].state == FREE) { si = i; return DECODE; }
        return NONE;
    }

    bool finished() const   // nothing left to start or finish. Called with mu_ held.
    {
        if (stop_) return true;
        if (!eof_ || decoding_) return false;
        for (auto &s : s_) if (s.state != FREE) return false;
        return true;
    }

    void work()
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (s_.empty()) s_.resize(slots_);
        for (;;)
        {
            int si = -1;
            Task t = pick(si);
            if (t == NONE)
            {
                if (finished()) { cv_.notify_all(); return; }
                cv_.wait(lk);
                continue;
            }
            Slot &s = s_[si];
            s.busy = true;
            if (t == DECODE) { decoding_ = true; s.seq = nextDecode_++; }
            lk.unlock();
            bool ok = true;
            try
            {
                switch (t)
                {
                case DECODE: ok = decode_(s.f); break;
                case REMAP:  remapStage(s.f, rm_, a_); break;
                case SEAM:   seamStage(s.f, rm_, a_, prevSeam_); break;
                case BLEND:  blendStage(s.f, rm_, a_); break;
                case WRITE:  ok = write_(s.f, s.seq); break;
                default: break;
                }
            }
            catch (const std::exception &ex)
            {
                lk.lock();
                if (error_.empty()) error_ = string("ERROR: frame ") + to_string(s.seq) + ": " + ex.what();
                stop_ = true; s.busy = false;
                if (t == DECODE) decoding_ = false;
                cv_.notify_all();
                return;
            }
            lk.lock();
            s.busy = false;
            switch (t)
            {
            case DECODE:
                decoding_ = false;
                if (ok) s.state = DECODED;
                else { eof_ = true; s.state = FREE; s.seq = -1; nextDecode_--; }
                break;
            case REMAP: s.state = REMAPPED; break;
            case SEAM:  s.state = SEAMED; nextSeam_++; break;
            case BLEND: s.state = BLENDED; break;
            case WRITE:
                s.state = FREE; s.seq = -1; nextWrite_++;
                if (!ok) stop_ = true;
                break;
            default: break;
            }
            cv_.notify_all();
        }
    }

    const int slots_, workers_;
    const RenderMaps &rm_;
    const Align &a_;
    std::function<bool(FrameWork &)> decode_;
    std::function<bool(FrameWork &, int)> write_;
    std::mutex mu_;
    std::condition_variable cv_;
    vector<Slot> s_;
    vector<int> prevSeam_;   // the seam stage runs one frame at a time, in order
    int nextDecode_ = 0, nextSeam_ = 0, nextWrite_ = 0;
    bool decoding_ = false, eof_ = false, stop_ = false;
    string error_;
};

// Seek to frame n. Tries an indexed jump first (instant, on a properly-indexed file
// like a remuxed MKV) and only falls back to sequential grab for un-indexed files.
// This is what makes --start fast: it jumps straight to the frame instead of
// grab-skipping from frame 0. Un-indexed input -> slow grab (remux to fix).

// ---------------------------------------------------------------- dual input
// The rig records TWO independent files, one per camera. PairCapture opens both
// and hands back the LEFT and RIGHT frames of each pair as they decode - separately,
// so each remap reads straight from its own camera's frame with no copies between.
//
// Pairing is driven by the FILENAME so that the CLI and the tuner
// share it with no extra plumbing:
//     --source take_..._cam0.mkv       ->  also opens take_..._cam1.mkv (either half works;
//                                          so do the /calib page's cam0_NNN / cam1_NNN)
//     --source "a.mkv::b.mkv"          (explicit, any names)
// A source that resolves to neither is an ERROR: this stitcher has no
// single-file mode. Every input is a pair.
//
// g_pairOffset shifts one stream against the other: >0 skips N frames of the
// CAM1 file, <0 skips N of CAM0. With genlock the correct value is a small
// constant (the two gst pipelines start a few ms apart); free-running cameras
// drift and no constant is exactly right. pair_check.py estimates it.
static int g_pairOffset = 0;
static bool g_pairAuto = true;    // --pair-offset N disables; "auto" forces
static bool g_pairResolved = false;

// Which camera is the LEFT image is decided by the calibration, not the file
// names: if the extrinsic yaw puts cam1 to the LEFT of cam0 (as it does once the
// rig saves its upside-down sensors rotated 180 - see calibration/rotate180.py),
// main() sets this and the whole pipeline runs with cam1 as left, cam0 as right.
// File names, --pair-offset and printed offsets stay in cam0/cam1 terms.
static bool g_swapLR = false;

// g_pairOffset as the L/R streams see it (it is stored in cam0/cam1 terms)
static int pairOffsetLR() { return g_swapLR ? -g_pairOffset : g_pairOffset; }

// cam0 + cam1 file names for either half of a pair, in the two naming styles the
// rig produces: the recorder's take_..._cam0.mkv / take_..._cam1.mkv, and the
// /calib page's cam0_NNN.png / cam1_NNN.png. False if src is neither.
static bool pairNames(const string &src, string &c0, string &c1)
{
    for (const char *tag : {"_cam0.", "_cam1."})
    {
        size_t c = src.rfind(tag);
        if (c != string::npos)
        {
            string pre = src.substr(0, c), post = src.substr(c + 6);
            c0 = pre + "_cam0." + post; c1 = pre + "_cam1." + post;
            return true;
        }
    }
    size_t slash = src.find_last_of("/\\");
    size_t b = slash == string::npos ? 0 : slash + 1;
    if (src.compare(b, 5, "cam0_") == 0 || src.compare(b, 5, "cam1_") == 0)
    {
        string dir = src.substr(0, b), rest = src.substr(b + 5);
        c0 = dir + "cam0_" + rest; c1 = dir + "cam1_" + rest;
        return true;
    }
    return false;
}

static bool resolvePairPaths(const string &src, string &L, string &R)
{
    size_t d = src.find("::");
    if (d != string::npos)
    {
        L = src.substr(0, d); R = src.substr(d + 2);
        if (g_swapLR) std::swap(L, R);
        return true;
    }
    string c0, c1;
    if (pairNames(src, c0, c1))
    {
        bool h0 = std::filesystem::exists(c0), h1 = std::filesystem::exists(c1);
        if (h0 && h1) { L = c0; R = c1; if (g_swapLR) std::swap(L, R); return true; }
        cerr << "ERROR: " << src << " is one half of a pair but its partner\n"
             << "       " << (h0 ? c1 : c0) << " does not exist.\n";
        L.clear(); R.clear();
        return false;
    }
    cerr << "ERROR: cannot pair '" << src << "'.\n"
         << "       This rig records one file per camera and the stitcher needs "
         << "both. Pass\n"
         << "       either file of a pair (take_..._cam0.mkv / _cam1.mkv, or the /calib\n"
         << "       page's cam0_NNN.png / cam1_NNN.png - the partner is found next to\n"
         << "       it) or an explicit \"cam0path::cam1path\".\n";
    L.clear(); R.clear();
    return false;
}


// Estimate the frame offset between the two camera files by cross-correlating
// per-frame mean brightness. Runs once per process, on the first few seconds.
//
// Why brightness: it is the cheapest signal that both cameras share. Anything
// that changes the light (clouds, someone crossing, a pan) moves both series
// together, and the shift that lines them up is the frame offset.
//
// Caveat worth keeping in mind: while the cameras FREE-RUN this converges on
// whatever fits the analysed window, but the true offset drifts across a take,
// so no single number stays right. With XVS genlock the offset is a genuine
// constant and this is exact.
// EXACT offset for takes from the shared-clock recorder. veery_server.py records
// both cameras in ONE GStreamer pipeline (one clock, one base time), starts each
// file at its camera's first real frame and keeps that frame's real timestamp,
// and tags both files SHARED_CLOCK_TAG. With genlock the two start times differ
// by a whole number of frames, and that number IS the offset - read straight off
// the capture timestamps, no brightness guessing. Untagged (older) takes return
// false and fall back to estimatePairOffset below. Result is in cam0/cam1 terms.
// (matroskamux stores taginject's comment on the video TRACK - "COMMENTS" in the
// stream tags - so both tag levels are searched.)
static const char *SHARED_CLOCK_TAG = "veery-shared-clock";
static string runCmd(const string &cmd);

static bool sharedClockOffset(const string &cam0, const string &cam1, int &offset)
{
    for (const string &f : {cam0, cam1})
        if (runCmd("ffprobe -v error -show_entries format_tags:stream_tags -of default=nw=1 \""
                   + f + "\"").find(SHARED_CLOCK_TAG) == string::npos)
            return false;
    auto probe = [](const string &f, const string &entry) {
        return runCmd("ffprobe -v error -select_streams v:0 -show_entries stream=" + entry
                      + " -of default=nokey=1:noprint_wrappers=1 \"" + f + "\"");
    };
    double t0, t1, fps = 30.0;
    try { t0 = stod(probe(cam0, "start_time")); t1 = stod(probe(cam1, "start_time")); }
    catch (...) { return false; }
    string rate = probe(cam0, "r_frame_rate");                   // e.g. "30/1"
    size_t sl = rate.find('/');
    try { if (sl != string::npos) fps = stod(rate.substr(0, sl)) / stod(rate.substr(sl + 1)); }
    catch (...) {}
    double frames = (t1 - t0) * fps;                  // cam1 starts this many frames later
    offset = -(int)llround(frames);                   // <0 skips that many frames of cam0
    cout << "  pair-offset from capture timestamps: " << offset << " frames (start cam0 "
         << std::fixed << std::setprecision(3) << t0 << "s, cam1 " << t1
         << "s; off-grid residual " << (frames - llround(frames)) << " frame)"
         << std::defaultfloat << "\n";
    if (fabs(frames - llround(frames)) > 0.25)
        cout << "  [!! residual > 1/4 frame - the cameras do not look genlocked]\n";
    return true;
}

static int estimatePairOffset(const string &lp, const string &rp,
                              int maxShift = 15, double seconds = 5.0)
{
    VideoCapture ca(lp), cb(rp);
    if (!ca.isOpened() || !cb.isOpened()) return 0;
    double fps = ca.get(CAP_PROP_FPS); if (fps <= 0) fps = 30.0;
    int want = (int)(seconds * fps) + 2 * maxShift;

    auto series = [&](VideoCapture &c) {
        vector<double> v; Mat f, g;
        for (int i = 0; i < want; i++)
        {
            if (!c.read(f) || f.empty()) break;
            resize(f, g, Size(32, 18), 0, 0, INTER_AREA);
            cvtColor(g, g, COLOR_BGR2GRAY);
            v.push_back(mean(g)[0]);
        }
        return v;
    };
    vector<double> A = series(ca), B = series(cb);
    ca.release(); cb.release();
    size_t n = min(A.size(), B.size());
    if (n < 30) return 0;

    vector<double> xs, ys;
    auto score = [&](int sh) {
        xs.clear(); ys.clear();
        for (size_t i = 0; i < n; i++)
        {
            long ia = (long)i + (sh > 0 ? sh : 0);
            long ib = (long)i + (sh < 0 ? -sh : 0);
            if (ia >= (long)A.size() || ib >= (long)B.size()) break;
            xs.push_back(A[ia]); ys.push_back(B[ib]);
        }
        if (xs.size() < 20) return -2.0;
        double mx = 0, my = 0;
        for (size_t i = 0; i < xs.size(); i++) { mx += xs[i]; my += ys[i]; }
        mx /= xs.size(); my /= ys.size();
        double num = 0, dxx = 0, dyy = 0;
        for (size_t i = 0; i < xs.size(); i++)
        {
            double dx = xs[i] - mx, dy = ys[i] - my;
            num += dx * dy; dxx += dx * dx; dyy += dy * dy;
        }
        double den = sqrt(dxx * dyy);
        return den > 0 ? num / den : -2.0;
    };

    int best = 0; double bestScore = -2.0, runnerUp = -2.0;
    for (int sh = -maxShift; sh <= maxShift; sh++)
    {
        double v = score(sh);
        if (v > bestScore) { bestScore = v; best = sh; }
    }
    for (int sh = -maxShift; sh <= maxShift; sh++)
        if (abs(sh - best) > 1) runnerUp = max(runnerUp, score(sh));

    cout << "  auto pair-offset: " << (g_swapLR ? -best : best) << " frames (correlation "
         << std::fixed << std::setprecision(3) << bestScore
         << ", margin " << (bestScore - runnerUp) << ")";
    if (bestScore < 0.5) cout << "  [weak - cameras free-running?]";
    else if (bestScore - runnerUp < 0.05) cout << "  [ambiguous]";
    cout << std::defaultfloat << "\n";
    return best;
}

class PairCapture
{
public:
    PairCapture() = default;
    explicit PairCapture(const string &src) { open(src); }

    bool open(const string &src)
    {
        release();
        string L, R;
        if (!resolvePairPaths(src, L, R)) return false;   // already reported why
        if (!a_.open(L)) return false;
        if (!b_.open(R)) { a_.release(); return false; }
        if (g_pairAuto && !g_pairResolved)
        {
            const string &c0 = g_swapLR ? R : L, &c1 = g_swapLR ? L : R;
            int ts;
            if (sharedClockOffset(c0, c1, ts))
                g_pairOffset = ts;                        // exact, from timestamps
            else
            {
                int est = estimatePairOffset(L, R);       // in L/R terms
                g_pairOffset = g_swapLR ? -est : est;
            }
            g_pairResolved = true;
        }
        // apply the constant offset once, at open, by pre-skipping frames
        int off = pairOffsetLR();
        int skipB = off > 0 ? off : 0;
        int skipA = off < 0 ? -off : 0;
        for (int i = 0; i < skipA; i++) a_.grab();
        for (int i = 0; i < skipB; i++) b_.grab();
        cout << "  paired input: " << std::filesystem::path(L).filename().string()
             << " + " << std::filesystem::path(R).filename().string();
        if (g_pairOffset) cout << "  (offset " << g_pairOffset << " frames)";
        cout << "\n";
        return true;
    }

    bool isOpened() const { return a_.isOpened() && b_.isOpened(); }
    void release() { a_.release(); b_.release(); }

    double get(int prop) const
    {
        double va = const_cast<VideoCapture &>(a_).get(prop);
        double vb = const_cast<VideoCapture &>(b_).get(prop);
        // the pair is only as long as its shorter half
        if (prop == CAP_PROP_FRAME_COUNT) return min(va, vb);
        return va;
    }

    bool set(int prop, double v)
    {
        bool ok = a_.set(prop, v);
        // keep the streams' relative offset when seeking
        double vb = (prop == CAP_PROP_POS_FRAMES) ? v + pairOffsetLR() : v;
        return b_.set(prop, vb) && ok;
    }

    bool grab() { bool ok = a_.grab(); return b_.grab() && ok; }

    // retrieve() pairs with grab() for scrubbing: decode whatever grab() staged
    bool retrieve(Mat &fa, Mat &fb)
    {
        if (!a_.retrieve(fa) || fa.empty()) return false;
        if (!b_.retrieve(fb) || fb.empty()) return false;
        return samePair(fa, fb);
    }

    bool read(Mat &fa, Mat &fb)
    {
        if (!a_.read(fa) || fa.empty()) return false;
        if (!b_.read(fb) || fb.empty()) return false;
        return samePair(fa, fb);
    }

private:
    // both halves share one set of maps, so they must match
    static bool samePair(const Mat &fa, const Mat &fb)
    {
        if (fa.size() == fb.size() && fa.type() == fb.type()) return true;
        cerr << "pair mismatch: " << fa.cols << "x" << fa.rows
             << " vs " << fb.cols << "x" << fb.rows << "\n";
        return false;
    }

    VideoCapture a_, b_;
};

template <class Cap>
static bool seekFrame(Cap &cap, int n)
{
    if (n <= 0) return true;
    cap.set(CAP_PROP_POS_FRAMES, (double)n);          // attempt indexed seek
    int pos = (int)cap.get(CAP_PROP_POS_FRAMES);
    if (pos == n) { cout << "  seek: indexed jump to frame " << n << " (fast)\n"; return true; }
    if (pos < 0 || pos > n) { cap.set(CAP_PROP_POS_FRAMES, 0.0); pos = 0; }  // bogus -> restart
    if (pos == 0)
        cout << "  seek: no index - grab-skipping " << n
             << " frames (SLOW; remux the file to add an index for fast parallel seeks)\n";
    for (int i = pos; i < n; i++)                     // grab the remainder (or all, from 0)
        if (!cap.grab()) return false;
    return true;
}

// Read a STILL the same way PairCapture reads video: two files, one per camera.
static bool readPairedImage(const string &source, Mat &fa, Mat &fb)
{
    string L, R;
    if (!resolvePairPaths(source, L, R)) return false;   // already reported why
    fa = imread(L); fb = imread(R);
    if (fa.empty() || fb.empty()) return false;
    if (fa.size() != fb.size() || fa.type() != fb.type())
    {
        cerr << "pair mismatch: " << fa.cols << "x" << fa.rows
             << " vs " << fb.cols << "x" << fb.rows << "\n";
        return false;
    }
    return true;
}

static string stitchImageFile(const string &source, const RenderMaps &rm,
                              const Align &a, const string &outDir, const string &outFile = "")
{
    FrameWork f;
    if (!readPairedImage(source, f.fL, f.fR)) return "ERROR: cannot read image";
    vector<int> prevSeam;
    renderFrame(f, rm, a, prevSeam);
    Mat &pano = f.out;
    string out = !outFile.empty() ? outFile : (outDir + "/pano.jpg");
    imwrite(out, pano);
    return out;
}

// Quiet, side-effect-free test that ffmpeg exists and that a given encoder actually
// initializes on THIS machine (a hardware encoder can be listed yet fail to open).
static string devNull() {
#ifdef _WIN32
    return "> NUL 2>&1";
#else
    return "> /dev/null 2>&1";
#endif
}
static bool ffmpegAvailable() { return runShell("ffmpeg -version " + devNull()) == 0; }
static bool encoderInitializes(const string &name, int w = 64, int h = 64)
{
    // One-frame null encode AT THE REAL OUTPUT SIZE: hardware encoders have
    // dimension limits (h264_videotoolbox refuses beyond ~4096 wide), and this
    // rig's panorama is ~6774 wide. Probing at 64x64 approved an encoder that
    // then died on the first real frame. Found 2026-09-19.
    return runShell("ffmpeg -hide_banner -loglevel error -f lavfi "
                    "-i color=c=black:s=" + to_string(w) + "x" + to_string(h) +
                    ":r=30 -frames:v 1 -an -c:v " + name +
                    " -f null - " + devNull()) == 0;
}

// Pick the H.264 encoder for the ffmpeg output pipe. Precedence:
//   1. an explicit --venc NAME
//   2. unless --cpu: the first hardware encoder that initializes on this machine
//        macOS  -> VideoToolbox;  else NVENC, then AMD AMF, then Intel QuickSync
//   3. software libx264
// Nothing is hardcoded to a particular GPU - candidates are probed at runtime, so this
// works on any machine and quietly degrades to CPU when no hardware encoder is usable.
static string chooseVideoEncoder(int w = 64, int h = 64)
{
    if (!g_vencExplicit.empty()) return g_vencExplicit;
    if (g_forceCpu) return "libx264";
    // HEVC FIRST, at every size (chosen 2026-09-19). It is the only hardware option
    // past 4096 wide, it costs ~6% speed against H.264 at equal size (measured:
    // 88.5 vs 93.7 fps at 4096x1433 on VideoToolbox), and it gives the same quality
    // in ~40% fewer bits. H.264 stays as a fallback only for machines whose HEVC
    // block refuses the frame; the panorama is a master that Resolve re-encodes, so
    // H.264's wider playback support buys nothing here.
    vector<string> cands =
#ifdef __APPLE__
        {"hevc_videotoolbox", "h264_videotoolbox"};
#else
        {"hevc_nvenc", "hevc_amf", "hevc_qsv", "h264_nvenc", "h264_amf", "h264_qsv"};
#endif
    for (const auto &c : cands)
        if (encoderInitializes(c, w, h))
        {
            if (c.rfind("hevc", 0) != 0)
                cout << "note: this machine's HEVC hardware encoder refused "
                     << w << "x" << h << " - falling back to " << c << "\n";
            return c;
        }
    cout << "note: no hardware encoder accepts " << w << "x" << h
         << " - falling back to libx264 (CPU, slower)\n";
    return "libx264";
}

// The exact ffmpeg command that reads raw BGR frames on stdin and writes the MP4.
static string buildEncodeCmd(const string &venc, int W, int H, double fps, const string &out)
{
    auto q = [](const string &s) { return "\"" + s + "\""; };
    ostringstream c;
    c << "ffmpeg -y -hide_banner -loglevel error"
      << " -f rawvideo -pixel_format bgr24 -video_size " << W << "x" << H
      << " -framerate " << fps << " -i - -an"
      // Panorama W/H aren't guaranteed even, but H.264 4:2:0 needs even dims - pad up
      // to the next even size (adds at most a 1px black edge; a no-op when already even).
      << " -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\""
      << " -c:v " << venc << " -b:v " << resolveBitrate(venc, W, H, fps);
    // HEVC in MP4 defaults to the 'hev1' tag, which QuickTime, Safari and some
    // Resolve builds refuse to open. 'hvc1' is the same bitstream, tagged the way
    // Apple's stack expects. Verified: hevc_videotoolbox emits hev1 without this.
    if (venc.find("hevc") != string::npos || venc.find("265") != string::npos)
        c << " -tag:v hvc1";
    if (venc == "libx264") c << " -preset medium";
    // Pin output format so every encoder tags color the same way (avoids the AMF
    // bt470bg->bt709 drift) and stays broadly playable; faststart for progressive play.
    c << " -pix_fmt yuv420p -colorspace bt709 -color_primaries bt709 -color_trc bt709"
      << " -movflags +faststart " << q(out);
    return c.str();
}

static string stitchVideoFile(const string &source, const RenderMaps &rm,
                              const Align &a, int startFrame, int endFrame, int totalFrames,
                              const string &outDir, const string &outFile = "",
                              std::atomic<int> *prog = nullptr)
{
    PairCapture cap(source);
    if (!cap.isOpened()) return "ERROR: cannot open video";
    double fps = cap.get(CAP_PROP_FPS);
    if (fps <= 0) fps = 30.0;
    const int BIG = 1 << 30;
    int s = startFrame < 0 ? 0 : startFrame;
    int e = endFrame >= 0 ? endFrame : (totalFrames > 0 ? totalFrames - 1 : BIG);
    bool bounded = (e < BIG);
    string out = !outFile.empty() ? outFile : (outDir + "/stitched_video.mp4");

    // Encode via an ffmpeg pipe (hardware HEVC when available, else libx264). ffmpeg
    // is effectively required (it's also the HEVC path for wide panoramas). If it isn't
    // on PATH we fall back to OpenCV's own H.264 writer (avc1) so a bare install still
    // stitches - on macOS that path is itself VideoToolbox-backed.
    Size osz(rm.OW, rm.OH);
    string venc = chooseVideoEncoder(osz.width, osz.height);
    bool useFfmpeg = ffmpegAvailable();
    FILE *pipe = nullptr;
    VideoWriter writer;
    if (useFfmpeg)
    {
        cout << "encoder: " << venc << " (ffmpeg pipe, "
             << resolveBitrate(venc, osz.width, osz.height, fps)
             << (g_vbitrate == "auto" ? " auto" : "") << ")\n";
        pipe = popen(buildEncodeCmd(venc, osz.width, osz.height, fps, out).c_str(), PIPE_WMODE);
        if (!pipe) useFfmpeg = false;   // couldn't spawn - fall back below
    }
    if (!useFfmpeg)
    {
        cout << "encoder: OpenCV avc1 (ffmpeg unavailable - using built-in writer)\n";
        writer.open(out, VideoWriter::fourcc('a', 'v', 'c', '1'), fps, osz);
        if (!writer.isOpened()) return "ERROR: cannot open output video";
    }

    seekFrame(cap, s);
    // Run the frames through the pipeline (see FramePipeline). Slots = frames in
    // flight; workers = threads taking stage tasks.
    const int total = bounded ? (e - s + 1) : BIG;
    int decoded = 0, written = 0;
    auto decode = [&](FrameWork &f) {
        if (decoded >= total || !cap.read(f.fL, f.fR)) return false;   // range end / EOF
        ++decoded;
        return true;
    };
    auto write = [&](FrameWork &f, int n) {
        Mat &pano = f.out;                  // continuous 8UC3, ready to pipe
        int i = s + n;
        if (pipe)
        {
            size_t bytes = (size_t)pano.total() * pano.elemSize();
            if (fwrite(pano.data, 1, bytes, pipe) != bytes)
            { cerr << "encoder pipe closed early (frame " << i << ") - see ffmpeg output above\n"; return false; }
        }
        else writer.write(pano);
        ++written;
        if (bounded)
        {
            int pct = (int)(100.0 * written / total);
            if (prog) prog->store(pct);
            if (written % 30 == 0 || i == e) cout << "  " << pct << "%  (frame " << i << ")\n";
        }
        else if (written % 30 == 0) cout << "  frame " << i << "\n";
        return true;
    };
    string err = FramePipeline(PIPE_SLOTS, PIPE_WORKERS, rm, a, decode, write).run();
    if (!err.empty()) cerr << err << "\n";
    cap.release();
    if (pipe)
    {
        int rc = pclose(pipe);
        if (rc != 0) return "ERROR: ffmpeg encoder exited " + to_string(rc) + " (encoder=" + venc + ")";
    }
    else writer.release();
    if (!err.empty()) return err;
    return out + "  (" + to_string(written) + " frames)";
}

static string base64(const vector<uchar> &data)
{
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string out;
    int val = 0, bits = -6;
    for (uchar c : data)
    {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) { out.push_back(t[(val >> bits) & 0x3F]); bits -= 6; }
    }
    if (bits > -6) out.push_back(t[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// Defined later; needed by the tuner's on-demand source loader.
static bool isVideoFile(const string &path);
static int probeFrames(const string &source, double fps);

// Escape a string for embedding in JSON (handles Windows backslashes + quotes).
static string jsonEscape(const string &s)
{
    string o;
    for (char c : s)
    {
        if (c == '\\' || c == '"') { o.push_back('\\'); o.push_back(c); }
        else if (c == '\n') o += "\\n";
        else o.push_back(c);
    }
    return o;
}

// The tuner page. Starts with no source loaded; the browser's "Import source"
// button (and /state on load) fill in the preview dynamically.
static string tunerHtml()
{
    ostringstream h;
    h << "<!doctype html><html><head><meta charset='utf-8'><title>Stitch tuner</title>"
      << R"HTML(<style>
 body{margin:0;font-family:system-ui,sans-serif;background:#111;color:#eee}
 #bar{padding:10px;display:flex;gap:12px;align-items:center;flex-wrap:wrap;background:#1b1b1b;position:sticky;top:0;z-index:2}
 button{font-size:15px;padding:5px 10px;border:0;border-radius:6px;background:#2a6f9e;color:#fff;cursor:pointer}
 #stitch{background:#1f8a3b;font-weight:700} #quit{background:#8a3b1f} #finish{background:#555}
 .grp{display:flex;gap:6px;align-items:center;border:1px solid #333;padding:5px 9px;border-radius:8px;font-size:14px}
 .val{width:60px;text-align:center;font-variant-numeric:tabular-nums;font-size:15px;background:#222;color:#eee;border:1px solid #444;border-radius:5px;padding:3px}
 .path{width:230px;font-size:12px;background:#222;color:#9cf;border:1px solid #444;border-radius:5px;padding:3px}
 #import{background:#6a4fb3;font-weight:700} button:disabled{opacity:.45;cursor:not-allowed}
 #wrap{overflow:auto} canvas{display:block;max-width:100%;background:#000}
 #status{padding:6px 10px;color:#9cf} .hint{color:#888;font-size:12px}
</style></head><body>
<div id="bar">
  <div class="grp"><button id="import">Import source…</button><input class="path" id="srcpath" type="text" readonly placeholder="no file loaded"></div>
  <div class="grp">Output <input class="path" id="outpath" type="text" readonly placeholder="not chosen yet"><button id="chooseout">Choose…</button></div>
  <div class="grp">Shift far (top) <button id="tl">&#9664;</button><input class="val" id="tv" type="number" value="0"><button id="tr">&#9654;</button></div>
  <div class="grp">Shift near (bottom) <button id="bl">&#9664;</button><input class="val" id="bv" type="number" value="0"><button id="br">&#9654;</button></div>
  <div class="grp">Shift vertical <button id="yl">&#9664;</button><input class="val" id="yv" type="number" value="0" step="0.5"><button id="yr">&#9654;</button></div>
  <div class="grp">Rotate&deg; <button id="rl">&#9664;</button><input class="val" id="rot" type="number" value="0" step="0.5"><button id="rr">&#9654;</button></div>
  <div class="grp"><label><input type="checkbox" id="showseam" checked> show seam line</label>
                   <label><input type="checkbox" id="crop" checked> crop to box</label> <span class="hint" id="cropdim"></span></div>
  <div class="grp"><label><input type="checkbox" id="blend"> overlap blend</label></div>
  <!-- Fixed defaults, not exposed: seam at the middle of the overlap,
       smart seam (routes around moving objects), 6-band blend, exposure match. -->
  <div class="grp" id="framegrp">Frame <button id="fprev">&#9664;</button><input type="range" id="frange" min="0" value="0" style="vertical-align:middle;width:140px"><input class="val" id="fval" type="number" value="0"><span id="ftot" style="color:#9cf">/ ?</span><button id="fnext">&#9654;</button></div>
  <button id="stitch" disabled>Stitch all frames</button>
  <button id="quit">Quit</button>
  <span class="hint">&#8592;/&#8594; shift both</span>
</div>
<div id="status">Click "Import source…" to choose a video or image.</div>
<div id="prog" style="padding:0 10px 10px;display:none">
  <progress id="pb" max="100" value="0" style="width:280px;height:16px;vertical-align:middle"></progress>
  <span id="pct" style="margin-left:8px">0%</span>
  <button id="finish" style="display:none;margin-left:12px">Finish &amp; stop</button>
</div>
<div id="cmdwrap" style="display:none;padding:0 10px 10px">
  <div style="font-size:.8em;color:#9cf;margin-bottom:4px">Equivalent CLI command for these settings (click to select, then copy):</div>
  <textarea id="cmdbox" readonly onclick="this.select()" style="width:100%;height:64px;font-family:monospace;font-size:.78em;background:#111;color:#dfe;border:1px solid #444;border-radius:6px;padding:6px;box-sizing:border-box"></textarea>
</div>
<div id="wrap"><canvas id="c"></canvas></div>
<script>
// Dynamic state — filled in by /state (on load) or /import (button).
let OW=0, OH=0, SEAM0=0, TOTAL=1, VIDEO=false, loaded=false;
const cv=document.getElementById('c'), ctx=cv.getContext('2d');
const stepv=()=>{ return 1; };   // arrows nudge by 1
const st=t=>{ document.getElementById('status').textContent=t; };
const tv=document.getElementById('tv'), bv=document.getElementById('bv'), yv=document.getElementById('yv');
const stitchBtn=document.getElementById('stitch');
let sTop=0, sBot=0, sY=0, seam=0, pending=0;   // seam fixed at the middle of the overlap
let rot=0, showSeam=true;   // rot = whole-panorama rotation (deg); showSeam toggles the red line
const clmp=(v,lo,hi)=>{ return Math.max(lo,Math.min(hi,v)); };
// Crop box (in OW/OH panorama coords). cropOn toggles it; drag body to move,
// drag the top-left / bottom-right handles to resize.
let cropOn=true, cropX=0, cropY=0, cropW=0, cropH=0, dragMode=null, dragStart=null, cropStart=null;
const imgL=new Image(), imgR=new Image();
function both(){ if(--pending<=0){ pending=0; render(); } }
imgL.onload=imgR.onload=both;
function drawRight(){
  const k=(OH>1)?(sBot-sTop)/(OH-1):0;         // per-row shear slope
  ctx.save(); ctx.transform(1,0,k,1,sTop,sY); ctx.drawImage(imgR,0,0); ctx.restore();
}
function render(){
  if(!loaded) return;
  sTop=+tv.value||0; sBot=+bv.value||0; sY=+yv.value||0;   // the seam stays fixed
  ctx.setTransform(1,0,0,1,0,0); ctx.globalAlpha=1; ctx.clearRect(0,0,OW,OH);
  // Preview the whole-panorama rotation the same way the engine does: rotate about the
  // canvas centre. The crop box stays axis-aligned (drawn after we restore).
  ctx.save();
  if(rot){ ctx.translate(OW/2,OH/2); ctx.rotate(rot*Math.PI/180); ctx.translate(-OW/2,-OH/2); }
  if(document.getElementById('blend').checked){
    ctx.globalAlpha=0.5; ctx.drawImage(imgL,0,0); drawRight(); ctx.globalAlpha=1;
  } else {
    ctx.drawImage(imgL,0,0);
    ctx.save(); ctx.beginPath(); ctx.rect(seam,0,OW-seam,OH); ctx.clip(); drawRight(); ctx.restore();
  }
  if(showSeam){
    ctx.strokeStyle='#f33'; ctx.lineWidth=2;
    ctx.beginPath(); ctx.moveTo(seam,0); ctx.lineTo(seam,OH); ctx.stroke();
  }
  ctx.restore();
  if(cropOn) drawCrop();
}
function drawCrop(){
  if(cropW<=0){ cropX=Math.round(OW*0.05); cropY=Math.round(OH*0.05); cropW=Math.round(OW*0.9); cropH=Math.round(OH*0.9); }
  cropX=clmp(cropX,0,OW-1); cropY=clmp(cropY,0,OH-1);
  cropW=clmp(cropW,1,OW-cropX); cropH=clmp(cropH,1,OH-cropY);
  ctx.save();
  ctx.fillStyle='rgba(0,0,0,0.55)';                       // dim everything outside the box
  ctx.fillRect(0,0,OW,cropY);
  ctx.fillRect(0,cropY+cropH,OW,OH-(cropY+cropH));
  ctx.fillRect(0,cropY,cropX,cropH);
  ctx.fillRect(cropX+cropW,cropY,OW-(cropX+cropW),cropH);
  ctx.strokeStyle='#ff0'; ctx.lineWidth=2; ctx.strokeRect(cropX,cropY,cropW,cropH);
  const hs=Math.max(10,OW*0.01); ctx.fillStyle='#ff0';
  ctx.fillRect(cropX-hs/2,cropY-hs/2,hs,hs);              // top-left handle
  ctx.fillRect(cropX+cropW-hs/2,cropY+cropH-hs/2,hs,hs);  // bottom-right handle
  ctx.restore();
  document.getElementById('cropdim').textContent=Math.round(cropW)+'x'+Math.round(cropH);
}
const nudge=(el,d)=>{ el.value=(+el.value||0)+d; changed(); };
tl.onclick=()=>{ nudge(tv,-stepv()); }; tr.onclick=()=>{ nudge(tv,stepv()); };
bl.onclick=()=>{ nudge(bv,-stepv()); }; br.onclick=()=>{ nudge(bv,stepv()); };
// vertical shift: positive moves the right camera's picture down; arrows nudge 0.5 px
yl.onclick=()=>{ nudge(yv,-0.5); }; yr.onclick=()=>{ nudge(yv,0.5); };
[tv,bv,yv].forEach((el)=>{ el.oninput=changed; });
// Whole-panorama rotation (levels a tilted field) + show/hide the red seam line.
const rotEl=document.getElementById('rot');
const setRot=v=>{ rot=Math.round(v*10)/10; if(rotEl) rotEl.value=rot; changed(); };
if(rotEl){ rotEl.oninput=()=>{ rot=+rotEl.value||0; changed(); }; }
const rlb=document.getElementById('rl'), rrb=document.getElementById('rr');
if(rlb){ rlb.onclick=()=>{ setRot((+rotEl.value||0)-0.5); }; }
if(rrb){ rrb.onclick=()=>{ setRot((+rotEl.value||0)+0.5); }; }
const ssEl=document.getElementById('showseam');
if(ssEl){ ssEl.onchange=()=>{ showSeam=ssEl.checked; render(); }; }
document.getElementById('blend').onchange=render;
// Crop box: drag body to move, drag the yellow corner handles to resize.
const toCanvas=(e)=>{ return { x: e.offsetX * OW / cv.clientWidth, y: e.offsetY * OH / cv.clientHeight }; };
cv.onmousedown=(e)=>{
  if(!loaded || !cropOn) return;
  const p=toCanvas(e);
  const hs=Math.max(14, OW*0.016);
  const nBR=Math.abs(p.x-(cropX+cropW))<hs && Math.abs(p.y-(cropY+cropH))<hs;
  const nTL=Math.abs(p.x-cropX)<hs && Math.abs(p.y-cropY)<hs;
  if(nBR) dragMode='br'; else if(nTL) dragMode='tl';
  else if(p.x>cropX && p.x<cropX+cropW && p.y>cropY && p.y<cropY+cropH) dragMode='move';
  else dragMode=null;
  if(dragMode){ dragStart=p; cropStart={x:cropX,y:cropY,w:cropW,h:cropH}; e.preventDefault(); }
};
cv.onmousemove=(e)=>{
  if(!dragMode) return;
  const p=toCanvas(e);
  const dx=p.x-dragStart.x, dy=p.y-dragStart.y;
  if(dragMode==='move'){ cropX=clmp(cropStart.x+dx,0,OW-cropW); cropY=clmp(cropStart.y+dy,0,OH-cropH); }
  else if(dragMode==='br'){ cropW=clmp(cropStart.w+dx,20,OW-cropX); cropH=clmp(cropStart.h+dy,20,OH-cropY); }
  else if(dragMode==='tl'){
    const nx=clmp(cropStart.x+dx,0,cropStart.x+cropStart.w-20), ny=clmp(cropStart.y+dy,0,cropStart.y+cropStart.h-20);
    cropW=cropStart.w+(cropStart.x-nx); cropH=cropStart.h+(cropStart.y-ny); cropX=nx; cropY=ny;
  }
  render();
};
addEventListener('mouseup',()=>{ if(dragMode){ dragMode=null; showCmd(); } });
document.getElementById('crop').onchange=(e)=>{
  cropOn=e.target.checked;
  if(!cropOn) document.getElementById('cropdim').textContent='';
  changed();
};
addEventListener('keydown',e=>{
  if(e.target.tagName==='INPUT') return;      // let typing in the boxes work normally
  const d=stepv();
  if(e.key==='ArrowLeft'){tv.value=(+tv.value||0)-d; bv.value=(+bv.value||0)-d; changed(); e.preventDefault();}
  else if(e.key==='ArrowRight'){tv.value=(+tv.value||0)+d; bv.value=(+bv.value||0)+d; changed(); e.preventDefault();}
});
// frame scrubbing (video only)
const frange=document.getElementById('frange'), fval=document.getElementById('fval');
const ftxt=n=>{ return 'Frame '+n+(TOTAL>1?(' / '+TOTAL):''); };
function loadFrame(n){
  if(!loaded) return;
  const FMAX = TOTAL>1 ? TOTAL-1 : 100000;
  n=Math.max(0,Math.min(FMAX,parseInt(n)||0)); frange.value=n; fval.value=n;
  st('Loading '+ftxt(n)+'…');
  fetch('/frame?n='+n).then(r=>{return r.json();}).then(d=>{
    if(d.error){ st('frame error: '+d.error); return; }
    pending=2; imgL.src=d.left; imgR.src=d.right; st(ftxt(n));
  }).catch(e=>{ st('frame load error: '+e); });
}
document.getElementById('fprev').onclick=()=>{ loadFrame((+frange.value||0)-1); };
document.getElementById('fnext').onclick=()=>{ loadFrame((+frange.value||0)+1); };
frange.onchange=()=>{ loadFrame(frange.value); };
fval.onchange=()=>{ loadFrame(fval.value); };

// Apply a loaded source (from /state or /import): size the canvas, wire the
// frame slider, show the first frame, and enable stitching.
function applyLoad(d){
  loaded=true; OW=d.ow; OH=d.oh; SEAM0=d.seam; TOTAL=d.total; VIDEO=d.video; seam=SEAM0;
  rot=0; { const r=document.getElementById('rot'); if(r) r.value=0; }   // reset rotation for a new source
  cropW=0;   // re-initialise the crop box to the new frame size on next draw
  cv.width=OW; cv.height=OH;
  document.getElementById('srcpath').value=d.source||'';
  document.getElementById('outpath').value=d.output||'';
  const known=TOTAL>1, FMAX=known?TOTAL-1:100000;
  frange.max=FMAX; frange.value=0; fval.value=0; fval.max=FMAX;
  document.getElementById('ftot').textContent = known ? ('/ '+TOTAL) : '/ ?';
  document.getElementById('framegrp').style.display = VIDEO ? '' : 'none';
  stitchBtn.disabled=false;
  pending=2; imgL.src=d.left; imgR.src=d.right;
  st('Loaded. Align the far (top) and near (bottom) edges, then Stitch.');
  showCmd();
}
document.getElementById('import').onclick=async()=>{
  st('Choose an input file…');
  try{
    const d=await (await fetch('/import')).json();
    if(d.cancelled){ st('Import cancelled.'); return; }
    if(d.error){ st('Import error: '+d.error); return; }
    applyLoad(d);
  }catch(e){ st('Import failed: '+e); }
};
// On page load, adopt a source that was preloaded via --source (if any).
(async()=>{
  try{ const d=await (await fetch('/state')).json(); if(d.loaded) applyLoad(d); }catch(e){}
})();

let polling=null;
const pb=document.getElementById('pb'), pct=document.getElementById('pct');
// fixed defaults: seam at the middle of the overlap, smart seam, 6-band blend, exposure match
const params=()=>{
  let p='shifttop='+(+tv.value||0)+'&shiftbottom='+(+bv.value||0)+'&shifty='+(+yv.value||0)+'&degrees='+rot+'&bands=6&exposure=1&smartseam=1';
  if(cropOn && cropW>0) p+='&cropx='+Math.round(cropX)+'&cropy='+Math.round(cropY)+'&cropw='+Math.round(cropW)+'&croph='+Math.round(cropH);
  return p;
};
// Show the equivalent CLI command as soon as an output is chosen, and keep it in
// step with every change - so a command can be copied without starting a stitch.
const outEl=document.getElementById('outpath'), cmdWrap=document.getElementById('cmdwrap'), cmdBox=document.getElementById('cmdbox');
let cmdTimer=null;
function showCmd(){
  if(!loaded || !outEl.value){ if(!polling) cmdWrap.style.display='none'; return; }
  clearTimeout(cmdTimer);
  cmdTimer=setTimeout(async()=>{
    try{ const d=await (await fetch('/command?'+params())).json();
         if(d.cmd){ cmdBox.value=d.cmd; cmdWrap.style.display='block'; } }catch(e){}
  },250);
}
function changed(){ render(); showCmd(); }
async function chooseOutput(){
  st('Choose where to save the output…');
  let out='';
  try{ out=(await (await fetch('/chooseoutput')).json()).path||''; }
  catch(e){ st('Could not open save dialog: '+e); return ''; }
  if(!out){ st('Save cancelled.'); return ''; }
  outEl.value=out; st('Output: '+out); showCmd();
  return out;
}
document.getElementById('chooseout').onclick=()=>{ chooseOutput(); };
stitchBtn.onclick=async()=>{
  if(polling || !loaded) return;
  // use the chosen output, or pop the "save as" dialog now if none was chosen yet
  const out=outEl.value || await chooseOutput();
  if(!out) return;
  stitchBtn.disabled=true;
  document.getElementById('prog').style.display='block';
  document.getElementById('finish').style.display='none';
  pb.value=0; pct.textContent='0%';
  st('Stitching all frames → '+out+' …');
  try{ const r=await fetch('/stitch?'+params());
       const t=await r.text();
       if(t==='busy'){ st('Already stitching…'); return; }
       if(t==='notloaded'){ st('Import a source first.'); stitchBtn.disabled=false; return; } }
  catch(e){ st('Error starting: '+e); stitchBtn.disabled=false; return; }
  polling=setInterval(async()=>{
    try{
      const p=await (await fetch('/progress')).json();
      pb.value=p.percent; pct.textContent=p.percent+'%';
      if(p.cmd){ document.getElementById('cmdwrap').style.display='block';
                 document.getElementById('cmdbox').value=p.cmd; }
      if(p.done){
        clearInterval(polling); polling=null;
        stitchBtn.disabled=false;
        pb.value=100; pct.textContent='100%';
        st('✅ Done — saved to '+p.result);
        document.getElementById('finish').style.display='inline-block';
      }
    }catch(e){}
  },400);
};
document.getElementById('finish').onclick=async()=>{
  try{await fetch('/quit');}catch(e){}
  st('Finished — server stopped. You can close this tab.'); try{window.close();}catch(e){}
};
document.getElementById('quit').onclick=async()=>{ try{await fetch('/quit');}catch(e){} st('Stopped. You can close this tab.'); };
</script></body></html>)HTML";
    return h.str();
}

// Value of `key` in a query string, "" if absent. Matches WHOLE keys only: a
// plain find("seam=") also hits "smartseam=1" and would read the seam as 1.
static string qparam(const string &query, const string &key)
{
    string k = key + "=";
    for (size_t p = query.find(k); p != string::npos; p = query.find(k, p + 1))
    {
        if (p != 0 && query[p - 1] != '&') continue;
        size_t s = p + k.size(), e = query.find('&', s);
        return query.substr(s, e == string::npos ? string::npos : e - s);
    }
    return "";
}

// Run a command and capture its stdout (trimmed). Used to drive the OS's
// native file dialogs so the binary is self-contained (no wrapper script).
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

// Native "open file" dialog. Returns the chosen path, or "" if cancelled/unavailable.
static string pickInputFile()
{
#ifdef _WIN32
    // A TopMost owner form forces the dialog to the foreground (otherwise it
    // opens behind the browser and you have to Alt+Tab to find it).
    return runCapture("powershell -NoProfile -Command \"Add-Type -AssemblyName System.Windows.Forms;"
                      "$o=New-Object System.Windows.Forms.Form -Property @{TopMost=$true};"
                      "$d=New-Object System.Windows.Forms.OpenFileDialog;"
                      "$d.Title='Select the input video or image';"
                      "if($d.ShowDialog($o) -eq 'OK'){$d.FileName}\" 2>NUL");
#elif __APPLE__
    return runCapture("osascript -e 'POSIX path of (choose file with prompt "
                      "\"Select the input video or image\")' 2>/dev/null");
#else
    return runCapture("zenity --file-selection --title=\"Select the input video or image\" 2>/dev/null");
#endif
}

// Native "save file" dialog. Returns the chosen path, or "" if cancelled/unavailable.
static string pickSaveFile(const string &defName)
{
#ifdef _WIN32
    return runCapture("powershell -NoProfile -Command \"Add-Type -AssemblyName System.Windows.Forms;"
                      "$o=New-Object System.Windows.Forms.Form -Property @{TopMost=$true};"
                      "$d=New-Object System.Windows.Forms.SaveFileDialog;"
                      "$d.Title='Save the stitched output as'; $d.FileName='" + defName + "';"
                      "if($d.ShowDialog($o) -eq 'OK'){$d.FileName}\" 2>NUL");
#elif __APPLE__
    return runCapture("osascript -e 'POSIX path of (choose file name with prompt "
                      "\"Save the stitched output as\" default name \"" + defName + "\")' 2>/dev/null");
#else
    return runCapture("zenity --file-selection --save --confirm-overwrite --filename=\"" + defName + "\" 2>/dev/null");
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

static string exePath();   // forward decl (defined below, near main)

// Build the exact, copy-pasteable CLI command that reproduces a stitch with these
// settings. Shown in the tuner UI + console so a tuned render (shifts, crop, etc.)
// can be re-run by hand. Only emits non-default flags to keep it readable.
static string buildCliCommand(const string &source, const string &calibDir,
                              double degrees, int seamArg, const Align &a,
                              const string &cropArg, int startFrame, int endFrame,
                              const string &outFile)
{
    auto q = [](const string &s) { return "\"" + s + "\""; };
    string exe = exePath(); if (exe.empty()) exe = "StitchPipeline";
    string c = q(exe) + " --source " + q(source);
    c += " --pair-offset " + to_string(g_pairOffset);   // resolved value, not "auto"
    if (g_scale != 1.0) c += " --scale " + to_string(g_scale);
    if (!calibDir.empty())    c += " --calib-dir " + q(calibDir);
    if (degrees != 0.0)       c += " --degrees " + to_string(degrees);
    if (seamArg >= 0)         c += " --seam " + to_string(seamArg);
    c += " --shift-top " + to_string(a.shiftTop) + " --shift-bottom " + to_string(a.shiftBottom);
    if (a.shiftY != 0.0)      c += " --shift-y " + to_string(a.shiftY);
    c += " --bands " + to_string(a.bands);
    if (!a.exposure)          c += " --no-exposure";
    if (!a.smartSeam)         c += " --no-smart-seam";
    if (!cropArg.empty())     c += " --crop " + q(cropArg);
    if (startFrame > 0)       c += " --start " + to_string(startFrame);
    if (endFrame >= 0)        c += " --end " + to_string(endFrame);
    c += " --out-file " + q(outFile);
    return c;
}

static void runTuneServer(const Mat &KL, const vector<double> &DL,
                          const Mat &KR, const vector<double> &DR, const Mat &R,
                          double degrees, int startFrame, int endFrame,
                          const string &outDir, const string &initSource,
                          const string &initOutFile, int port,
                          const string &calibDir)
{
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    string html = tunerHtml();

    // Mutable server state: a source can be loaded (or replaced) at any time
    // via the browser's Import button, so none of this is fixed up front.
    string source = initSource, outFile = initOutFile;
    bool video = false, loaded = false;
    int totalFrames = 1;
    StitchMaps m;
    string curLeft, curRight;            // first-frame preview (data: URIs)
    PairCapture frameCap;                // persistent for /frame scrubbing
    int frameCapPos = -1;

    // Open a file: build the stitch maps and the first-frame preview. Returns
    // "" on success or an error message.
    auto loadSource = [&](const string &path) -> string {
        bool isVid = isVideoFile(path);
        Mat fL, fR; int tf = 1;
        if (!isVid) { readPairedImage(path, fL, fR); }
        else
        {
            PairCapture cap(path);
            if (!cap.isOpened()) return "cannot open video";
            tf = (int)cap.get(CAP_PROP_FRAME_COUNT);
            if (tf < 1 || tf > 100000000) { double fps = cap.get(CAP_PROP_FPS); tf = probeFrames(path, fps > 0 ? fps : 30.0); }
            cap.read(fL, fR); cap.release();
        }
        if (fL.empty() || fR.empty()) return "cannot read source";
        StitchMaps mm = buildStitchMaps(KL, DL, KR, DR, R, fL.cols, fL.rows, -1);
        Mat mL, mR; warpPreview(fL, fR, mm, mL, mR);
        vector<uchar> bL, bR; vector<int> q = {IMWRITE_JPEG_QUALITY, 85};
        imencode(".jpg", mL, bL, q); imencode(".jpg", mR, bR, q);
        m = mm; video = isVid; totalFrames = tf; source = path; loaded = true;
        curLeft = "data:image/jpeg;base64," + base64(bL);
        curRight = "data:image/jpeg;base64," + base64(bR);
        frameCap.release(); frameCapPos = -1;
        return "";
    };

    auto stateJson = [&]() -> string {
        if (!loaded) return "{\"loaded\":false}";
        ostringstream j;
        j << "{\"loaded\":true,\"ow\":" << m.OW << ",\"oh\":" << m.OH << ",\"seam\":" << m.seam
          << ",\"ox0\":" << m.ox0 << ",\"ox1\":" << m.ox1
          << ",\"total\":" << totalFrames << ",\"video\":" << (video ? "true" : "false")
          << ",\"source\":\"" << jsonEscape(source) << "\",\"output\":\"" << jsonEscape(outFile) << "\""
          << ",\"left\":\"" << curLeft << "\",\"right\":\"" << curRight << "\"}";
        return j.str();
    };

    // Stitch settings from a /stitch or /command query. One parser for both, so the
    // command shown before a stitch is exactly the one the stitch runs.
    auto parseStitch = [&](const string &query, Align &a, StitchMaps &mm, ViewBox &vb,
                           string &cropStr, int &seamVal, double &dg) {
        a.shiftTop = !qparam(query, "shifttop").empty() ? stod(qparam(query, "shifttop")) : 0;
        a.shiftBottom = !qparam(query, "shiftbottom").empty() ? stod(qparam(query, "shiftbottom")) : 0;
        a.shiftY = !qparam(query, "shifty").empty() ? stod(qparam(query, "shifty")) : 0;
        a.bands = !qparam(query, "bands").empty() ? stoi(qparam(query, "bands")) : 0;
        a.exposure = qparam(query, "exposure") == "1";
        a.smartSeam = qparam(query, "smartseam") == "1";
        string ss = qparam(query, "seam");
        mm = m;
        if (!ss.empty()) mm.seam = stoi(ss);
        seamVal = ss.empty() ? -1 : stoi(ss);
        // Optional crop (full-canvas coords): restrict all work to this region.
        int cw = !qparam(query, "cropw").empty() ? stoi(qparam(query, "cropw")) : 0;
        int chh = !qparam(query, "croph").empty() ? stoi(qparam(query, "croph")) : 0;
        int cx = !qparam(query, "cropx").empty() ? stoi(qparam(query, "cropx")) : 0;
        int cy = !qparam(query, "cropy").empty() ? stoi(qparam(query, "cropy")) : 0;
        // Crop rect as a --crop string (parallel children re-apply it themselves).
        cropStr = (cw > 0 && chh > 0)
            ? (to_string(cx) + "," + to_string(cy) + "," + to_string(cw) + "," + to_string(chh)) : "";
        // Rotation of the finished panorama (tuner's Rotate control -> --degrees);
        // falls back to whatever was passed on the command line when the param is absent.
        dg = !qparam(query, "degrees").empty() ? stod(qparam(query, "degrees")) : degrees;
        vb = clampBox(mm, ViewBox{cx, cy, cw, chh, dg});   // rotate, then crop
    };

    // Preload a source passed on the command line (`--source x --tune`).
    if (!source.empty()) { string err = loadSource(source); if (!err.empty()) cerr << "preload: " << err << "\n"; }

    socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) { cerr << "socket() failed\n"; return; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int bound = -1;
    for (int p = port; p < port + 10; ++p)
    {
        addr.sin_port = htons((unsigned short)p);
        if (::bind(srv, (sockaddr *)&addr, sizeof(addr)) == 0) { bound = p; break; }
    }
    if (bound < 0 || listen(srv, 8) != 0) { cerr << "Could not bind a port\n"; CLOSESOCK(srv); return; }

    string url = "http://127.0.0.1:" + to_string(bound) + "/";
    cout << "\nTuner running at " << url << "  (opening browser; Ctrl+C or Quit to stop)\n";
    openBrowser(url);

    bool running = true;
    while (running)
    {
        socket_t cl = accept(srv, nullptr, nullptr);
        if (cl == INVALID_SOCKET) continue;

        string req; char buf[4096];
        for (;;)
        {
            int n = recv(cl, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, n);
            if (req.find("\r\n\r\n") != string::npos) break;
        }
        size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
        string target = (sp1 != string::npos && sp2 != string::npos) ? req.substr(sp1 + 1, sp2 - sp1 - 1) : "/";
        string path = target, query;
        size_t qm = target.find('?');
        if (qm != string::npos) { path = target.substr(0, qm); query = target.substr(qm + 1); }

        string status = "200 OK", ctype = "text/plain", body;
        if (path == "/")
        {
            ctype = "text/html; charset=utf-8";
            body = html;
        }
        else if (path == "/state")
        {
            ctype = "application/json";
            body = stateJson();
        }
        else if (path == "/import")
        {
            ctype = "application/json";
            string p = pickInputFile();
            if (p.empty()) body = "{\"cancelled\":true}";
            else { string err = loadSource(p); body = err.empty() ? stateJson() : ("{\"error\":\"" + jsonEscape(err) + "\"}"); }
        }
        else if (path == "/chooseoutput")
        {
            ctype = "application/json";
            string def = video ? "stitched.mp4" : "stitched.jpg";
            string p = pickSaveFile(def);
            if (!p.empty()) outFile = p;
            body = "{\"path\":\"" + jsonEscape(p) + "\"}";
        }
        else if (path == "/stitch")
        {
            if (!loaded) { body = "notloaded"; }
            else if (g_busy) { body = "busy"; }
            else
            {
                Align a; StitchMaps mm; ViewBox vb; string cropStr; int seamVal; double dg;
                parseStitch(query, a, mm, vb, cropStr, seamVal, dg);
                if (!cropStr.empty()) cout << "[stitch] crop " << cropStr << "\n";
                g_busy = true; g_done = false; g_percent = 0;
                { lock_guard<mutex> lk(g_mu); g_result.clear(); }
                cout << "[stitch] top=" << a.shiftTop << " bottom=" << a.shiftBottom
                     << " y=" << a.shiftY << " seam=" << mm.seam << " -> " << outFile << " ...\n";
                int tf = totalFrames;
                string src = source, of = outFile; bool vid = video;
                int endRes = endFrame >= 0 ? endFrame : (tf > 0 ? tf - 1 : -1);
                string calib = calibDir;
                // Build + record the exact equivalent CLI command (shown in UI + console).
                {
                    string fo = of.empty() ? (outDir + "/stitched_video.mp4") : of;
                    string cmd = buildCliCommand(src, calib, dg, seamVal, a, cropStr,
                                                 startFrame, (vid ? endRes : -1), fo);
                    { lock_guard<mutex> lk(g_mu); g_cmd = cmd; }
                    cout << "[stitch] equivalent CLI command:\n  " << cmd << "\n";
                }
                std::thread([mm, vb, a, src, vid, startFrame, endFrame, tf, outDir, of]() {
                    RenderMaps rm = buildRenderMaps(mm, vb, a);
                    string res = vid
                        ? stitchVideoFile(src, rm, a, startFrame, endFrame, tf, outDir, of, &g_percent)
                        : stitchImageFile(src, rm, a, outDir, of);
                    { lock_guard<mutex> lk(g_mu); g_result = res; }
                    g_percent = 100; g_done = true; g_busy = false;
                    cout << "[stitch] done -> " << res << "\n";
                }).detach();
                body = "started";
            }
        }
        else if (path == "/command")
        {
            // The CLI command for the current settings WITHOUT stitching - shown as
            // soon as an output is chosen. Empty until a source and an output exist.
            ctype = "application/json";
            string cmd;
            if (loaded && !outFile.empty())
            {
                Align a; StitchMaps mm; ViewBox vb; string cropStr; int seamVal; double dg;
                parseStitch(query, a, mm, vb, cropStr, seamVal, dg);
                int endRes = endFrame >= 0 ? endFrame : (totalFrames > 0 ? totalFrames - 1 : -1);
                cmd = buildCliCommand(source, calibDir, dg, seamVal, a, cropStr, startFrame,
                                      (video ? endRes : -1), outFile);
            }
            body = "{\"cmd\":\"" + jsonEscape(cmd) + "\"}";
        }
        else if (path == "/frame")
        {
            ctype = "application/json";
            if (!loaded) { body = "{\"error\":\"no source loaded\"}"; }
            else
            {
                int n = 0; string ns = qparam(query, "n");
                if (!ns.empty()) n = stoi(ns);
                if (n < 0) n = 0;
                Mat fL, fR;
                // sequential positioning (seeking is unreliable). Grab forward from the
                // current position; only re-open when scrubbing backward.
                if (!frameCap.isOpened() || n < frameCapPos)
                { frameCap.release(); frameCap.open(source); frameCapPos = -1; }
                while (frameCapPos < n) { if (!frameCap.grab()) break; frameCapPos++; }
                if (frameCapPos == n) frameCap.retrieve(fL, fR);
                if (fL.empty() || fR.empty()) { body = "{\"error\":\"cannot read frame\"}"; }
                else
                {
                    Mat mL, mR; warpPreview(fL, fR, m, mL, mR);
                    vector<uchar> bL, bR; vector<int> q = {IMWRITE_JPEG_QUALITY, 85};
                    imencode(".jpg", mL, bL, q);
                    imencode(".jpg", mR, bR, q);
                    ostringstream j;
                    j << "{\"left\":\"data:image/jpeg;base64," << base64(bL)
                      << "\",\"right\":\"data:image/jpeg;base64," << base64(bR) << "\"}";
                    body = j.str();
                }
            }
        }
        else if (path == "/progress")
        {
            ctype = "application/json";
            string res, cmd; { lock_guard<mutex> lk(g_mu); res = g_result; cmd = g_cmd; }
            ostringstream j;
            j << "{\"busy\":" << (g_busy ? "true" : "false")
              << ",\"done\":" << (g_done ? "true" : "false")
              << ",\"percent\":" << g_percent.load()
              << ",\"cmd\":\"" << jsonEscape(cmd) << "\""
              << ",\"result\":\"" << jsonEscape(res) << "\"}";
            body = j.str();
        }
        else if (path == "/quit")
        {
            body = "bye";
            running = false;
        }
        else { status = "404 Not Found"; body = "not found"; }

        string resp = "HTTP/1.1 " + status + "\r\nContent-Type: " + ctype +
                      "\r\nContent-Length: " + to_string(body.size()) +
                      "\r\nConnection: close\r\n\r\n" + body;
        send(cl, resp.data(), (int)resp.size(), 0);
        CLOSESOCK(cl);
    }
    frameCap.release();
    CLOSESOCK(srv);
#ifdef _WIN32
    WSACleanup();
#endif
    cout << "Tuner stopped.\n";
}

static string argVal(int argc, char **argv, const string &key, const string &def)
{
    for (int i = 1; i < argc - 1; i++)
        if (key == argv[i]) return argv[i + 1];
    return def;
}

static bool hasArg(int argc, char **argv, const string &key)
{
    for (int i = 1; i < argc; i++)
        if (key == argv[i]) return true;
    return false;
}

static bool isVideoFile(const string &path)
{
    string ext = fs::path(path).extension().string();
    transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    static const vector<string> vids = {".mp4", ".mkv", ".avi", ".mov", ".m4v", ".webm"};
    return find(vids.begin(), vids.end(), ext) != vids.end();
}

// Robust frame count when OpenCV's CAP_PROP_FRAME_COUNT is bogus.
// Prefer counting demuxed video packets via ffprobe: fast (no decode) and exact for
// intra-only streams (1 packet = 1 frame). Fall back to duration x fps.
static string runCmd(const string &cmd)
{
    FILE *p = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return "";
    char buf[128]; string out;
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    return out;
}

static int probeFrames(const string &source, double fps)
{
    string packets = runCmd("ffprobe -v error -count_packets -select_streams v:0 "
                            "-show_entries stream=nb_read_packets "
                            "-of default=nokey=1:noprint_wrappers=1 \"" + source + "\"");
    try { int n = stoi(packets); if (n > 0) return n; } catch (...) {}
    string dur = runCmd("ffprobe -v error -show_entries format=duration "
                        "-of default=nokey=1:noprint_wrappers=1 \"" + source + "\"");
    try { return (int)llround(stod(dur) * fps); } catch (...) { return 0; }
}

// Full path to the running executable (used to locate calibration/ regardless
// of the current working directory, so double-clicking the binary works).
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

// A directory counts as a calibration if it holds cam0_intrinsics.json. The
// upward search below means a WRONG hit here is not an error, it is a silently
// wrong panorama built from another rig's camera model - so the test must match
// only what this rig actually writes. (A looser test did exactly that once,
// found 2026-09-19.)
static bool hasCalib(const fs::path &dir)
{
    std::error_code ec;
    return fs::exists(dir / "cam0_intrinsics.json", ec);
}

// Resolve the calibration directory. Priority:
//   1. the requested path if it already has the JSONs (explicit --calib-dir,
//      or the default when run from the stitching/ folder);
//   2. a calibration/ folder found by walking UP from the executable's location
//      (depth-independent: handles Mac's build/ and Windows' build/Release/);
//   3. the same upward search from the current working directory;
//   4. the requested path unchanged (caller then reports the missing files).
static string resolveCalibDir(const string &requested)
{
    if (hasCalib(requested)) return requested;
    std::error_code ec;
    for (fs::path d = fs::path(exePath()).parent_path(); !d.empty(); d = d.parent_path())
    {
        if (hasCalib(d / "calibration")) return (d / "calibration").string();
        if (d == d.root_path()) break;
    }
    for (fs::path d = fs::current_path(ec); !d.empty(); d = d.parent_path())
    {
        if (hasCalib(d / "calibration")) return (d / "calibration").string();
        if (d == d.root_path()) break;
    }
    return requested;
}

// Run a shell command, blocking until it exits. On Windows a command that begins
// with a quoted path needs the WHOLE string wrapped again or cmd.exe mis-parses it.
static int runShell(string cmd)
{
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    return std::system(cmd.c_str());
}

int main(int argc, char **argv)
{
#ifndef _WIN32
    // If the ffmpeg encoder pipe dies, we want fwrite to fail (handled) rather than a
    // SIGPIPE killing us silently. (Windows has no SIGPIPE.)
    signal(SIGPIPE, SIG_IGN);
#endif
    string source = argVal(argc, argv, "--source", argVal(argc, argv, "--image", ""));
    string calibDir = resolveCalibDir(argVal(argc, argv, "--calib-dir", "../calibration"));
    string outDir = argVal(argc, argv, "--out", "pipeline_out");
    string outFile = argVal(argc, argv, "--out-file", "");   // full path incl. filename (overrides --out)
    double degrees = stod(argVal(argc, argv, "--degrees", "0"));
    int seamArg = stoi(argVal(argc, argv, "--seam", "-1"));
    int startFrame = stoi(argVal(argc, argv, "--start", "0"));
    int endFrame = stoi(argVal(argc, argv, "--end", "-1"));
    string sx = argVal(argc, argv, "--shift-x", "0");   // convenience: sets top=bottom
    Align a;
    a.shiftTop = stod(argVal(argc, argv, "--shift-top", sx));
    a.shiftBottom = stod(argVal(argc, argv, "--shift-bottom", sx));
    a.shiftY = stod(argVal(argc, argv, "--shift-y", "0"));
    a.bands = stoi(argVal(argc, argv, "--bands", "6"));   // 0 = hard seam
    a.exposure = !hasArg(argc, argv, "--no-exposure");     // on by default
    a.smartSeam = !hasArg(argc, argv, "--no-smart-seam");  // on by default
    int port = stoi(argVal(argc, argv, "--port", "8090"));
    bool tune = hasArg(argc, argv, "--tune");
    string cropArg = argVal(argc, argv, "--crop", "");
    {   // --pair-offset N pins the offset; "auto" (the default) estimates it
        string po = argVal(argc, argv, "--pair-offset", "auto");
        if (po != "auto") { g_pairOffset = stoi(po); g_pairAuto = false; g_pairResolved = true; }
    }
    g_scale = stod(argVal(argc, argv, "--scale", "1.0"));
    if (g_scale <= 0.05 || g_scale > 1.0) { cerr << "--scale must be in (0.05, 1]\n"; return 1; }

    // Video-encoder selection (globals consumed by chooseVideoEncoder / stitchVideoFile).
    // --cpu / --no-hwenc force libx264; --venc names a specific encoder; --bitrate sets -b:v.
    g_forceCpu   = hasArg(argc, argv, "--cpu") || hasArg(argc, argv, "--no-hwenc");
    g_vencExplicit = argVal(argc, argv, "--venc", "");
    g_vbitrate   = argVal(argc, argv, "--bitrate", "auto");

    // Ensure the output destination exists (batch, or a preset --out-file).
    if (!outFile.empty()) { fs::path p(outFile); if (p.has_parent_path()) fs::create_directories(p.parent_path()); }
    else fs::create_directories(outDir);

    // Stitching/warp ALWAYS runs on the CPU: the OpenCL/GPU warp was measured slower
    // (a memory-bound remap sandwiched between CPU decode and CPU encode pays a
    // per-frame CPU<->GPU copy that outweighs the GPU speedup). The video ENCODER
    // still uses the GPU (hardware H.264, with a libx264 fallback - chooseVideoEncoder).
    ocl::setUseOpenCL(false);
    cout << "OpenCL available: " << ocl::haveOpenCL() << ", using GPU (warp): " << ocl::useOpenCL() << "\n";

    cout << "Calibration: " << calibDir << "\n";
    Mat KL, KR, R;
    vector<double> DL, DR;
    loadIntrinsics(calibDir + "/cam0_intrinsics.json", KL, DL);
    loadIntrinsics(calibDir + "/cam1_intrinsics.json", KR, DR);
    cout << "calibration model: fisheye (equidistant)\n";
    R = loadRotation(calibDir + "/stereo_extrinsics.json");
    // cam1 left of cam0 (negative yaw) -> run with cam1 as the left image. R maps
    // cam0-frame directions into cam1's frame; the reverse mapping is its transpose.
    if (atan2(R.at<double>(2, 0), R.at<double>(2, 2)) < 0)
    {
        g_swapLR = true;
        std::swap(KL, KR);
        std::swap(DL, DR);
        R = R.t();
        cout << "orientation: cam1 is the LEFT camera (from the extrinsics) - "
             << "stitching cam1|cam0\n";
    }

    // Interactive tuner — the default when no --source is given, and whenever
    // --tune is passed. The browser's Import button loads the source on demand,
    // so `source` may be empty here (empty page until the user imports).
    if (source.empty() || tune)
    {
        runTuneServer(KL, DL, KR, DR, R, degrees, startFrame, endFrame, outDir, source, outFile, port, calibDir);
        return 0;
    }

    // Headless batch stitch of a source given on the command line.
    bool video = isVideoFile(source);
    Mat fL, fR;
    int totalFrames = 1;
    if (!video) readPairedImage(source, fL, fR);
    else
    {
        PairCapture cap(source);
        if (cap.isOpened())
        {
            totalFrames = (int)cap.get(CAP_PROP_FRAME_COUNT);
            // some containers (e.g. MJPEG-in-MKV) don't report a valid count;
            // fall back to ffprobe (duration x fps), or 0 -> typeable frame box.
            if (totalFrames < 1 || totalFrames > 100000000)
            {
                double fps = cap.get(CAP_PROP_FPS);
                totalFrames = probeFrames(source, fps > 0 ? fps : 30.0);
            }
            if (startFrame > 0) seekFrame(cap, startFrame);
            cap.read(fL, fR);
            cap.release();
        }
    }
    if (fL.empty() || fR.empty()) { cerr << "Cannot read source: " << source << endl; return 1; }

    StitchMaps m = buildStitchMaps(KL, DL, KR, DR, R, fL.cols, fL.rows, seamArg);

    // Optional --crop "x,y,w,h" (full-canvas coords) and --degrees: rotate the whole
    // panorama, then crop - folded with cam1's shear into one table per camera
    // (see buildRenderMaps).
    ViewBox vb;
    vb.degrees = degrees;
    if (!cropArg.empty()
        && sscanf(cropArg.c_str(), "%d,%d,%d,%d", &vb.x, &vb.y, &vb.w, &vb.h) == 4 && vb.w > 0 && vb.h > 0)
        cout << "crop " << vb.w << "x" << vb.h << " @ (" << vb.x << "," << vb.y << ")\n";
    RenderMaps rm = buildRenderMaps(m, vb, a);
    if (degrees != 0.0)
        cout << "rotate " << degrees << " deg (clockwise), then crop -> " << rm.OW << "x" << rm.OH << "\n";

    string result = video ? stitchVideoFile(source, rm, a, startFrame, endFrame, totalFrames, outDir, outFile, nullptr)
                          : stitchImageFile(source, rm, a, outDir, outFile);
    cout << (video ? "video -> " : "image -> ") << result << "\n";
    return 0;
}
