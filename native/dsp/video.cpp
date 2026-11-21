// kepyur video decoder. See video.h. The algorithm follows vtx-spike/viewer/vtxview.cpp (measured on a
// real R4 NTSC VTX: 93-96 % of lines locked); changes are marked "kepyur:".
#include "video.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace kp {

const VideoStd kNtsc{63.5556, 9.4, 52.6, 240, 18, "NTSC"};
const VideoStd kPal{64.0, 10.5, 52.0, 288, 22, "PAL"};

// ---- picture switches (every decoder in the process; see video.h) ----
namespace {
std::atomic<bool> g_luma_comb{false}, g_acomb{false}, g_repair{false}, g_smooth{false};
}
void set_luma_comb(bool on) { g_luma_comb = on; }
bool luma_comb_on() { return g_luma_comb.load(std::memory_order_relaxed); }
void set_adaptive_comb(bool on) { g_acomb = on; }
bool adaptive_comb_on() { return g_acomb.load(std::memory_order_relaxed); }
void set_repair(bool on) { g_repair = on; }
bool repair_on() { return g_repair.load(std::memory_order_relaxed); }
void set_smooth_lines(bool on) { g_smooth = on; }
bool smooth_lines_on() { return g_smooth.load(std::memory_order_relaxed); }

namespace {

struct Biquad {  // RBJ low-pass
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void design(double fc, double fs, double q = 0.7071) {
    const double w = 2 * M_PI * fc / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
    const double a0 = 1 + al;
    b0 = static_cast<float>((1 - c) / 2 / a0);
    b1 = static_cast<float>((1 - c) / a0);
    b2 = b0;
    a1 = static_cast<float>(-2 * c / a0);
    a2 = static_cast<float>((1 - al) / a0);
  }
  void bandpass(double fc, double fs, double q) {  // RBJ band-pass, 0 dB at fc
    const double w = 2 * M_PI * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    const double a0 = 1 + al;
    b0 = static_cast<float>(al / a0);
    b1 = 0;
    b2 = static_cast<float>(-al / a0);
    a1 = static_cast<float>(-2 * c / a0);
    a2 = static_cast<float>((1 - al) / a0);
  }
  void notch(double fc, double fs, double q) {
    const double w = 2 * M_PI * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    const double a0 = 1 + al;
    b0 = static_cast<float>(1 / a0);
    b1 = static_cast<float>(-2 * c / a0);
    b2 = b0;
    a1 = b1;
    a2 = static_cast<float>((1 - al) / a0);
  }
  inline float run(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  // For a filter that is being fed zeros: once its state has died away to nothing (far below anything that could
  // reach a pixel), make it exactly zero, before it gets down to denormal numbers.
  void settle() {
    if (std::fabs(z1) < 1e-25f && std::fabs(z2) < 1e-25f) z1 = z2 = 0;
  }
};

// Biquads side by side, one per lane of a vector of 4 (or 2) floats (NEON on a phone, SSE on a PC): each lane
// goes through exactly Biquad::run's arithmetic, all of them in one step. For filters that run on every sample,
// where a step of each on its own was most of the cost.
typedef float v4f __attribute__((vector_size(16)));
typedef float v2f __attribute__((vector_size(8)));
template <typename V> struct BiquadN {
  V b0{}, b1{}, b2{}, a1{}, a2{}, z1{}, z2{};
  void load(int i, const Biquad &q) {
    b0[i] = q.b0;
    b1[i] = q.b1;
    b2[i] = q.b2;
    a1[i] = q.a1;
    a2[i] = q.a2;
    z1[i] = q.z1;
    z2[i] = q.z2;
  }
  void store(int i, Biquad &q) const {
    q.z1 = z1[i];
    q.z2 = z2[i];
  }
  inline V run(V x) {
    const V y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void settle(int i) {  // Biquad::settle, for one lane
    if (std::fabs(z1[i]) < 1e-25f && std::fabs(z2[i]) < 1e-25f) z1[i] = z2[i] = 0;
  }
};
using Biquad4 = BiquadN<v4f>;
using Biquad2 = BiquadN<v2f>;

// Four pixels as RGBX words (bytes R, G, B, X): draw_line's per-pixel conversion, each of R, G and B (0..1)
// to clamp(z * 255, 0, 255) truncated to a byte.
typedef uint32_t v4u __attribute__((vector_size(16)));
inline v4u byte_of(v4f z) {
  const v4f lo = {0.f, 0.f, 0.f, 0.f}, hi = {255.f, 255.f, 255.f, 255.f};
  z = z * 255.f;
  z = z < lo ? lo : z;
  z = hi < z ? hi : z;
  return __builtin_convertvector(z, v4u);
}
inline v4u rgb_px(v4f r, v4f g, v4f b) { return 0xff000000u | byte_of(b) << 16 | byte_of(g) << 8 | byte_of(r); }
inline v4u grey_px(v4f y) {
  const v4u q = byte_of(y);
  return 0xff000000u | q << 16 | q << 8 | q;
}

}  // namespace

// ---- the decoder (streaming, one sample of discriminator output at a time, MHz) ----------------------
class Decoder {
public:
  Decoder(double fs, FrameSink sink, const DecoderOpts &o) : fs(fs), sink(std::move(sink)), o(o) {
    luma_ring.assign(8192, 0);
    chroma_ring.assign(8192, cf{});
    ring_mask = 8191;
    // chroma: mixed down by the subcarrier, 4th-order Butterworth low-pass at 1.2 MHz (U/V bandwidth)
    for (Biquad *b : {&cl_r1, &cl_i1}) b->design(1.2e6, fs, 0.54);
    for (Biquad *b : {&cl_r2, &cl_i2}) b->design(1.2e6, fs, 1.31);
    lp_luma.design(3.0e6, fs, 0.54);
    lp_luma2.design(3.0e6, fs, 1.31);
    lp_sync.design(1.0e6, fs);
    // luma comb: one gentle section (Q 0.6: no peaking, ~1 % overshoot), -3 dB near 4 MHz
    lp_wide.design(4.8e6, fs, 0.6);
    comb_ring.assign(8192, CombTap{});
    lp_vert.design(1.0e6, fs);  // adaptive comb: brightness below the colour band, line against line
    fade_ring.assign(8192, 0);
    bad_ring.assign(8192, 0);
    // sparkle repair: how long after a click each filter still carries it. The 1.2 MHz chroma low-pass peaks
    // 0.4 us after it, swings back and has settled by 1.2 us; the adaptive comb's 1 MHz band by 0.6 us; the
    // comb's wide luma within 0.15 us.
    rep_fwd_c = std::max(1, static_cast<int>(std::lround(1.2e-6 * fs)));
    rep_fwd_low = std::max(1, static_cast<int>(std::lround(0.6e-6 * fs)));
    rep_fwd_wide = std::max(1, static_cast<int>(std::lround(0.15e-6 * fs)));
    // ... and where in that stretch a click is big in chroma: the low-pass's main lobe, 0.3 to 0.75 us after
    // the run's first mark; the swing back after it is small (patch_line, two clicks close together). 0.3, not
    // 0.25: with the lobe taken one sample wider at 20 MS/s, patching such a pair as one was no gain in most
    // modes (more pixels visibly wrong, measured)
    rep_lobe0 = static_cast<int>(std::lround(0.3e-6 * fs));
    rep_lobe1 = std::min(rep_fwd_c, static_cast<int>(std::lround(0.75e-6 * fs)));
    // adaptive fade behind the 3-line comb (draw_line): the chroma low-pass's main lobe, as three points
    fade_back = std::max(1, static_cast<int>(std::lround(0.4e-6 * fs)));
    fade_step = std::max(1, static_cast<int>(std::lround(0.2e-6 * fs)));
    // colour / brightness registration: the notch chain was aligned by hand (0.21 us); the comb's luma filter
    // is faster, so colour must come that much later relative to it
    {
      const auto delay = [](Biquad a, Biquad *b) {  // group delay at low frequency: the impulse's centroid
        double s = 0, sw = 0;
        for (int i = 0; i < 2048; i++) {
          float x = i == 0 ? 1.f : 0.f;
          x = a.run(x);
          if (b) x = b->run(x);
          s += x;
          sw += x * static_cast<double>(i);
        }
        return s != 0 ? sw / s : 0;
      };
      Biquad l2 = lp_luma2;
      cdel_comb = 0.21e-6 * fs + (delay(lp_luma, &l2) - delay(lp_wide, nullptr));
    }
    acq.resize(static_cast<size_t>(fs * 0.06));  // 60 ms
    hist.assign(800, 0);
    set_std(o.std_force == 2 ? kPal : kNtsc);
  }

  DecoderStats stats() const {
    DecoderStats s;
    s.acquiring = acquiring;
    s.sign = sign;
    s.tip = tip;
    s.blank = blank;
    s.lines = lines;
    s.lines_locked = lines_locked;
    s.fields = fields_seen;
    s.frames = frames_out;
    s.acquisitions = acquisitions;
    s.losses = losses;
    s.line_us = T / fs * 1e6;
    s.lock_recent = recent_n ? static_cast<float>(recent_locked) / recent_n : 0.f;
    s.std_name = std_->name;
    s.rephases = rephases;
    s.colour = colour_on;
    s.burst_err = c_err;
    s.clicks_repaired = clicks_repaired;
    s.lines_mended = lines_mended;
    return s;
  }

  // A block of discriminator output, MHz; bad: the front end's click marks for it (sparkle repair), or null.
  // Each piece of up to kPiece samples goes through in two passes. First the sync path (a 1 MHz low-pass and a
  // 3-point median), whose filter never changes, over the whole piece. Then the pulse tracking, a sample at a
  // time (track), with the luma and colour paths following behind (filter_to): they are brought up to the
  // sample in hand before anything that reads their rings or changes their switches (an hsync, a line that
  // ends, acquisition), and at the end of the piece. So their filters run in tight loops with the state in
  // registers (sample by sample, through everything, it went through memory for every filter and every
  // sample: a third of the decode thread on a Pixel 8 Pro), and every sample still gets the same arithmetic,
  // in the same order, with the switches as they were when it came.
  void push_block(const float *d, const uint8_t *bad, size_t cnt) {
    float mb[kPiece];
    while (cnt > 0) {
      const size_t pn = std::min(cnt, kPiece);
      piece_d = d;
      piece_bad = bad;
      piece_n0 = n;
      {
        Biquad ls = lp_sync;
        float a = med_a, b = med_b;
        for (size_t k = 0; k < pn; k++) {
          const float s = ls.run(d[k]);
          mb[k] = std::max(std::min(a, b), std::min(std::max(a, b), s));
          b = a;
          a = s;
        }
        lp_sync = ls;
        med_a = a;
        med_b = b;
      }
      for (size_t k = 0; k < pn;) {
        k += quiet_run(mb + k, pn - k);
        if (k < pn) track(mb[k++]);
      }
      filter_to(n);
      d += pn;
      if (bad) bad += pn;
      cnt -= pn;
    }
  }

private:
  double fs;
  FrameSink sink;
  DecoderOpts o;
  const VideoStd *std_ = &kNtsc;
  float sign = +1, tip = -1, blank = 0, thr = -0.5f;
  uint64_t n = 0;  // samples taken (the pulse tracking's count)
  // push_block: the piece in hand (piece_n0: n at its first sample), and how far the luma and colour paths have
  // got (nf <= n: filter_to)
  static constexpr size_t kPiece = 512;
  const float *piece_d = nullptr;
  const uint8_t *piece_bad = nullptr;
  uint64_t piece_n0 = 0, nf = 0;
  // overdue_at: the line_start and T it was worked out for
  bool od_valid = false;
  double od_ls = 0, od_T = 0;
  uint64_t od_at = UINT64_MAX;
  std::vector<float> luma_ring;
  std::vector<cf> chroma_ring;  // chroma at baseband (U/V before the burst rotation)
  // kepyur: NTSC chroma comb (DecoderOpts::comb). The subcarrier is 227.5 x the line rate, so at the same
  // place one line away chroma comes back with the subcarrier's sign flipped while luma detail does not:
  // combing cancels the luma near the subcarrier that the 1.2 MHz chroma filter lets through as rainbows.
  // It works on the baseband chroma already in chroma_ring, inside render_line: each line is drawn one line
  // late, once the line below is in, and combed with both neighbours (centred, so chroma stays on luma's
  // line). Adaptive: per pixel a neighbour only counts as far as its luma there matches this line's, so the
  // top and bottom edges of OSD glyphs fall back to the plain filter instead of keeping half the rainbow.
  // Luma keeps the notch. PAL (2H subcarrier pattern) is unchanged.
  struct CombLine {
    double ls = -1;
    int64_t k = -1;
    int field = -1;
    bool col = false;  // drawn in colour
    bool ok = false;   // usable as a neighbour: locked, in colour, burst on the PLL's predicted phase
    bool hit = false;  // sparkle repair: still badly hit after patching (mend_rows)
    bool fade = false;  // adaptive comb: fade_ring holds this line's fade
    uint64_t epoch = 0;
    double phase = 0;  // c_phase and c_mag after this line's burst: it is drawn after the next line's
    float mag = 0;
  };
  static constexpr float kCombFull = 0.08f, kCombNone = 0.25f;  // luma mismatch, of black-to-white
  static constexpr float kCombMaxErr = 0.35f;  // rad: a burst further off means the stream jumped
  bool comb = false;
  double comb_w = 0;           // the mixing oscillator's frequency, rad per sample
  uint64_t comb_epoch = 0;     // bumped where the sample stream may have jumped: lines across it don't mix
  CombLine comb_up, comb_mid;  // the two lines before the current one
  float burst_e = 0;           // |phase error| of the last burst against the PLL's prediction
  std::vector<float> comb_m;   // draw_line: the comb on every other pixel (see there)
  std::vector<cf> comb_o;
  size_t ring_mask;
  // kepyur: colour. A free-running oscillator at the nominal subcarrier mixes chroma to baseband; a
  // per-line PLL on the burst tracks the transmitter's subcarrier phase (and its small frequency error);
  // PAL: the burst's +-45 degree swing gives the V switch, and adjacent lines are averaged (PAL-D).
  Biquad luma_notch, cl_r1, cl_r2, cl_i1, cl_i2;
  cf lo{1, 0}, lo_step{1, 0};
  double c_phase = 0, c_dphi = 0;  // burst reference phase (rad) and its drift per line
  int64_t c_last_line = -1;
  float c_mag = 0, c_err = 1;      // burst amplitude (MHz), mean |phase error| (rad)
  bool colour_on = false;
  int c_good = 0;
  std::vector<float> prev_u, prev_v;  // PAL-D: the previous line's U/V
  std::vector<float> px_cm, px_cr, px_ci;  // draw_colour: the comb for each pixel (chroma * px_cm - px_c{r,i})
  std::vector<double> px_off;      // draw_line: pixel x is px_off[x] samples into the line's picture (set_std)
  float med_a = 0, med_b = 0;
  Biquad lp_luma, lp_luma2, lp_sync;
  // Luma comb (DecoderOpts::luma_filter, set_luma_comb) and adaptive comb (set_adaptive_comb): see push().
  // The switches are read once per line (refresh_switches), not per sample.
  Biquad lp_wide, bp_chroma, lp_vert, luma_notch_w;
  struct CombTap {
    float wide, low, d;  // wide luma, its low band (adaptive comb only), the raw discriminator output
  };
  std::vector<CombTap> comb_ring;  // like luma_ring; one array, so a look at the line before costs one miss
  std::vector<float> fade_ring;    // adaptive comb with the 3-line chroma comb: each sample's fade, for draw_line
  int fade_back = 8, fade_step = 4;  // where draw_line reads it, in samples before the chroma (see there)
  bool luma_comb = false, adaptive = false, repair = false;
  int adaptive_lines = 0;  // lines since the adaptive comb came on: its filters and ring need one to fill
  uint64_t prev_back = 1;  // the line before: ring sample n - prev_back, and prev_fr of the way to the next one
  float prev_fr = 0, inv_depth = 1;  // inv_depth: 1 / sync depth
  float vdiff = 0;
  int chroma_quiet = 0;  // samples in a row with the colour faded out completely
  double cdel_comb = 0;
  // Sparkle repair (set_repair): the front end's click marks alongside luma_ring; see patch_line / mend_rows.
  std::vector<uint8_t> bad_ring;
  int rep_fwd_c = 24, rep_fwd_low = 12, rep_fwd_wide = 3;  // samples a click lasts in chroma / the comb's taps
  int rep_lobe0 = 6, rep_lobe1 = 15;  // chroma: where the click's main lobe is, in samples after a run's first mark
  int64_t bad_from = -1;  // first of the badly hit rows waiting for a good row below
  uint64_t mend_field = ~0ull;
  int mend_cur = -1;
  uint64_t clicks_repaired = 0, lines_mended = 0;
  std::vector<uint16_t> lum_rows;  // smooth lines: each field row's brightness ...
  std::vector<int8_t> best_dir;    // ... and one row's interpolation directions
  bool low = false, longrun = false;
  uint64_t low_start = 0;
  double T = 0, line_start = -1;
  uint64_t lines = 0, lines_locked = 0, line_idx = 0, acquisitions = 0, losses = 0;
  uint64_t line_low = 0;
  bool line_has_longrun = false;
  uint64_t line_longrun_start = 0;
  bool prev_line_vsync = false;
  std::vector<float> acq;
  size_t acq_n = 0;
  bool acquiring = true;
  std::vector<uint8_t> fieldA, fieldB, frame;
  bool haveA = false, haveB = false;
  int cur_field = -1;
  int64_t field_line0 = 0;
  uint64_t frames_out = 0, fields_seen = 0;
  std::vector<uint32_t> hist;
  double hlo = -8, hhi = 8;
  int hdec = 0;
  uint64_t hist_count = 0;
  // kepyur: sync-gated levels: sync tip from the middle of each hsync pulse, blanking from the back porch,
  // measured on locked active lines and smoothed over ~64 lines. Picture content cannot move them (the
  // spike's histogram peaks could, and the threshold wandered with the scene).
  uint64_t gated_lines = 0;
  // kepyur: re-phase instead of re-acquire. After a jump in the signal (stream restart, a dropped bank)
  // the hsyncs keep coming, one line apart, but outside the PLL's window. A run of them on a steady grid
  // while the PLL freewheels moves the line grid onto them within a few lines.
  int miss_run = 0, stray_run = 0;
  double last_stray = -1;
  uint64_t rephases = 0;
  // kepyur: lock quality over a sliding window of lines (the spike counted since acquisition)
  static constexpr int kRecent = 1024;
  uint8_t recent[kRecent] = {};
  int recent_pos = 0, recent_n = 0, recent_locked = 0;

  // The pulse tracking for the next sample (m: its sync-path value). The luma and colour paths are behind it
  // (see push_block): filter_to(n) brings them up to and including this sample before anything that uses them.
  inline void track(float m) {
    const uint64_t now = n++;
    if (acquiring) {
      if (acq_n < acq.size()) {
        acq[acq_n++] = m;
      } else {
        filter_to(n);
        acquire();
      }
      return;
    }
    if (gated_lines < 64) track_levels(m);  // until the sync-gated measurement has settled
    const float w = sign * m;
    if (!low && w < thr) {
      low = true;
      low_start = now;
      longrun = false;
    } else if (low) {
      if (!longrun && now - low_start > 0.3 * T) {
        longrun = true;
        if (!line_has_longrun) {
          line_has_longrun = true;
          line_longrun_start = low_start;
        }
      }
      if (w >= thr) {
        low = false;
        const double wid = (now - low_start) / fs * 1e6;
        if (wid > 3.0 && wid < 6.5) {
          filter_to(n);
          on_hsync(static_cast<double>(low_start));
        }
      }
      line_low++;
    }
    // freewheel: line overdue by > 8 % with no edge
    if (now > overdue_at()) {
      filter_to(n);
      const double ls = line_start;
      miss_run++;
      count_line(false);
      render_line(ls, false);
      close_line(ls + T);
      check_lost();
    }
  }

  // How many of the next samples (sync-path values m[0..cnt)) track() would do nothing with: locked on (not
  // acquiring, the levels settled), between pulses, still above the threshold, and the line not yet overdue.
  // They are passed over here, n moved on past them; the first that would do something is left to track().
  // ~90 % of the samples, looked at eight at a time (a vector compare), where track() took each on its own.
  size_t quiet_run(const float *m, size_t cnt) {
    if (acquiring || gated_lines < 64 || low) return 0;
    const uint64_t od = overdue_at();
    if (od < n) return 0;  // the next sample is overdue
    if (od - n < cnt) cnt = static_cast<size_t>(od - n) + 1;  // samples up to and including od are not
    const float s = sign, t = thr;
    size_t j = 0;
    for (; j + 8 <= cnt; j += 8) {
      bool hit = false;
      for (int q = 0; q < 8; q++) hit |= s * m[j + q] < t;
      if (hit) break;
    }
    while (j < cnt && !(s * m[j] < t)) j++;
    n += j;
    return j;
  }

  // The first sample past which the line is overdue (line_start + 8 % of T; none while there is no line), as
  // an integer, worked out again only when line_start or T has changed: for a whole number now, now > x exactly
  // when now > floor(x). Per sample in doubles it cost ~10 % of the decode thread on a phone.
  uint64_t overdue_at() {
    if (!od_valid || line_start != od_ls || T != od_T) {
      od_valid = true;
      od_ls = line_start;
      od_T = T;
      const double x = line_start + T * 1.08;
      od_at = line_start < 0 || !(x < 1.8e19) ? UINT64_MAX : static_cast<uint64_t>(std::floor(x));
    }
    return od_at;
  }

  // The luma and colour paths (and the click marks' ring) up to sample `to`, not included (see push_block).
  void filter_to(uint64_t to) {
    if (nf >= to) return;
    const size_t k0 = static_cast<size_t>(nf - piece_n0), cnt = static_cast<size_t>(to - nf);
    const uint8_t *bd = piece_bad ? piece_bad + k0 : nullptr;
    if (luma_comb) filter_comb(piece_d + k0, bd, cnt);
    else filter_notch(piece_d + k0, bd, cnt);
    nf = to;
  }

  // Luma through the 3 MHz low-pass (two sections) and the subcarrier notch. Colour (here and in filter_comb):
  // mixed down by the oscillator, whose phase belongs to the absolute sample index (burst and picture share
  // it), then a 4th-order low-pass for each of U and V. The two sections of luma, U and V go side by side
  // (Biquad4: lanes 0-2, the first sections in s1 and the second in s2, so s1's output is s2's input lane for
  // lane), the notch on its own. All in locals for the loop, so that their state stays in registers.
  void filter_notch(const float *d, const uint8_t *bd, size_t cnt) {
    Biquad4 s1, s2;
    s1.load(0, lp_luma);
    s1.load(1, cl_r1);
    s1.load(2, cl_i1);
    s2.load(0, lp_luma2);
    s2.load(1, cl_r2);
    s2.load(2, cl_i2);
    Biquad nt = luma_notch;
    cf osc = lo;
    const cf step = lo_step;
    const bool col = o.colour;
    const size_t mask = ring_mask;
    float *lr = luma_ring.data();
    cf *cr = chroma_ring.data();
    uint8_t *br = bad_ring.data();
    uint64_t idx = nf;
    for (size_t k = 0; k < cnt; k++, idx++) {
      const size_t at = idx & mask;
      const float x = d[k];
      br[at] = bd ? bd[k] : 0;
      cf c{};
      if (col) {
        c = x * std::conj(osc);
        osc *= step;
        if ((idx & 4095) == 0) osc /= std::abs(osc);
      }
      const v4f y = s2.run(s1.run(v4f{x, c.real(), c.imag(), 0.f}));
      lr[at] = nt.run(y[0]);
      if (col) cr[at] = cf(y[1], y[2]);
    }
    s1.store(0, lp_luma);
    s2.store(0, lp_luma2);
    luma_notch = nt;
    if (col) {  // without colour its filters stay as they were (only ever fed zeros here)
      s1.store(1, cl_r1);
      s1.store(2, cl_i1);
      s2.store(1, cl_r2);
      s2.store(2, cl_i2);
      lo = osc;
    }
  }

  // The NTSC luma comb, and the adaptive comb on top of it (refresh_switches: the switches are read once per
  // line, so they hold for a whole call). Locals as in filter_notch, and side by side the same way: after the
  // wide low-pass, its three filters (the colour band of the line difference, and the adaptive comb's low band
  // and notch) in s2's lanes 0-2, then U and V's two sections in c1 and c2.
  void filter_comb(const float *d, const uint8_t *bd, size_t cnt) {
    Biquad wide = lp_wide;
    Biquad4 s2;
    s2.load(0, bp_chroma);
    s2.load(1, lp_vert);
    s2.load(2, luma_notch_w);
    Biquad2 c1, c2;
    c1.load(0, cl_r1);
    c1.load(1, cl_i1);
    c2.load(0, cl_r2);
    c2.load(1, cl_i2);
    cf osc = lo;
    const cf step = lo_step;
    const bool col = o.colour, adapt = adaptive, adapt_full = adaptive_lines > 1, comb3 = comb;
    const uint64_t back = prev_back;
    const float fr = prev_fr, idepth = inv_depth;
    float vd = vdiff;
    int quiet = chroma_quiet;
    const size_t mask = ring_mask;
    float *lr = luma_ring.data(), *fdr = fade_ring.data();
    cf *cr = chroma_ring.data();
    uint8_t *br = bad_ring.data();
    CombTap *ring = comb_ring.data();
    uint64_t idx = nf;
    for (size_t k = 0; k < cnt; k++, idx++) {
      const size_t at = idx & mask;
      const float x = d[k];
      br[at] = bd ? bd[k] : 0;
      float csrc = x;  // what the colour is demodulated from
      // NTSC colour flips phase from one line to the next. Half the difference with the same point one line
      // earlier (T samples, tracked) is colour plus vertical detail; band-passing it at the subcarrier leaves
      // the colour, and the brightness is the wide signal minus that. No notch, so no ringing.
      const float w = wide.run(x);
      CombTap &now_tap = ring[at];
      now_tap.wide = w;
      now_tap.d = x;
      // the same point on the line before: T samples back, between ring samples q0 and q0 + 1 (prev_back and
      // prev_fr are worked out once per line, see refresh_switches)
      const bool have_prev = idx > back;
      const CombTap &p0 = ring[(idx - back) & mask], &p1 = ring[(idx - back + 1) & mask];
      const v4f y2 = s2.run(v4f{0.5f * (w - (have_prev ? p0.wide + (p1.wide - p0.wide) * fr : w)), w, w, 0.f});
      float luma = w - y2[0];
      if (adapt) {
        // Adaptive comb: how different the line before is, below the colour band (so colour itself, which
        // flips from line to line, doesn't count), in sync depths; past ~0.25 fall back to the notch, fully by
        // ~0.6.
        const float lf = y2[1];
        now_tap.low = lf;
        const float ln = y2[2];
        if (have_prev && adapt_full) {
          const float dv = std::fabs(lf - (p0.low + (p1.low - p0.low) * fr));
          vd += (dv - vd) * 0.35f;  // ~3 samples: a whole edge, not one noisy sample
          const float fade = std::clamp((vd * idepth - 0.25f) * (1.f / 0.35f), 0.f, 1.f);
          luma += (ln - luma) * fade;
          // And the colour combed too: half the difference with the line before keeps the colour (it flips
          // from line to line) and cancels brightness detail that continues down the picture (the strokes of
          // OSD characters); what is left at a sharp edge is faded out. With the 3-line chroma comb on, that
          // comb does the cancelling (centred on this line), and the fade is only noted here: draw_line
          // applies it where the comb found no line to cancel with. Fading here took the colour off the
          // line under every horizontal edge, which the comb draws well from the line below, and off part of
          // the next line, which is combed with that one.
          if (comb3) {
            fdr[at] = fade;
          } else {
            csrc = 0.5f * (x - (p0.d + (p1.d - p0.d) * fr));
            csrc *= 1.f - fade;
            // While the fade is complete (a whole line under a horizontal edge; the first picture line of every
            // field too: top_line() works that line's colour out again afterwards, with filters of its own,
            // but this one still runs through the line here) the chroma low-pass is fed exact zeros, and
            // within a few hundred samples its state decays into denormal numbers, which x86 computes some
            // hundred times slower: 9 % of the decode time on the bench's test picture. Looked at every 64
            // samples, it never gets that far.
            if (fade < 1.f) {
              quiet = 0;
            } else if ((++quiet & 63) == 0) {
              c1.settle(0);
              c1.settle(1);
              c2.settle(0);
              c2.settle(1);
            }
          }
        }
      }
      lr[at] = luma;
      if (col) {
        const cf c = csrc * std::conj(osc);
        osc *= step;
        if ((idx & 4095) == 0) osc /= std::abs(osc);
        const v2f yc = c2.run(c1.run(v2f{c.real(), c.imag()}));
        cr[at] = cf(yc[0], yc[1]);
      }
    }
    lp_wide = wide;
    s2.store(0, bp_chroma);
    if (adapt) {  // without the adaptive comb its filters stay as they were
      s2.store(1, lp_vert);
      s2.store(2, luma_notch_w);
    }
    if (col) {
      c1.store(0, cl_r1);
      c1.store(1, cl_i1);
      c2.store(0, cl_r2);
      c2.store(1, cl_i2);
      lo = osc;
    }
    vdiff = vd;
    chroma_quiet = quiet;
  }

  void set_std(const VideoStd &s) {
    std_ = &s;
    T = s.T_us * 1e-6 * fs;
    fieldA.assign(static_cast<size_t>(o.width) * s.active * 4, 0);
    fieldB = fieldA;
    frame.assign(static_cast<size_t>(o.width) * s.active * 2 * 4, 0);
    const double fsc = &s == &kPal ? 4.43361875e6 : 315e6 / 88;
    const double a = 2 * M_PI * fsc / fs;
    lo_step = cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    luma_notch.notch(fsc, fs, 1.5);  // the subcarrier out of the picture (also the B/W herringbone)
    luma_notch_w.notch(fsc, fs, 1.5);  // adaptive comb: the same, on the comb's wide signal
    bp_chroma.bandpass(fsc, fs, 1.0);  // luma comb: the colour band of the line difference
    refresh_switches();
    comb = o.comb && &s != &kPal;
    comb_w = a;
    comb_epoch++;
    prev_u.assign(o.width, 0.f);
    prev_v.assign(o.width, 0.f);
    px_off.resize(o.width);  // draw_line: each pixel's place on the line, in samples after its picture starts
    for (int x = 0; x < o.width; x++) px_off[x] = s.aw_us * 1e-6 * fs * x / o.width;
    colour_on = false;
    c_good = 0;
    c_last_line = -1;
  }

  // The process-wide picture switches, as of now. PAL's colour repeats over two lines, not one: it keeps the
  // notch.
  void refresh_switches() {
    luma_comb = (o.luma_filter >= 0 ? o.luma_filter == 1 : luma_comb_on()) && std_ != &kPal;
    adaptive = luma_comb && adaptive_comb_on();
    adaptive_lines = adaptive ? std::min(adaptive_lines + 1, 2) : 0;
    repair = repair_on();
    // n - T = (n - ceil(T)) + (ceil(T) - T)
    const double up = std::ceil(T);
    prev_back = static_cast<uint64_t>(std::max(up, 1.0));
    prev_fr = static_cast<float>(up - T);
    inv_depth = 1.f / std::max(blank - tip, 0.2f);
  }

  void count_line(bool locked) {
    lines++;
    if (locked) lines_locked++;
    if (recent_n == kRecent) recent_locked -= recent[recent_pos];
    else recent_n++;
    recent[recent_pos] = locked;
    recent_locked += locked;
    recent_pos = (recent_pos + 1) % kRecent;
  }

  // kepyur: lost when fewer than 40 % of the last ~2 fields' lines found their hsync (a healthy picture
  // locks ~90 %: only the vertical interval freewheels). The spike used the ratio since acquisition.
  void check_lost() {
    const int window = std::min(kRecent, 2 * (std_ == &kPal ? 312 : 262));
    if (recent_n < window) return;
    if (recent_locked * 10 < recent_n * 4) {
      losses++;
      KP_LOGI("video: lost lock (%d/%d recent lines), re-acquiring", recent_locked, recent_n);
      restart_acquisition();
    }
  }

  void restart_acquisition() {
    acquiring = true;
    acq_n = 0;
    lines = lines_locked = 0;
    cur_field = -1;
    haveA = haveB = false;
    recent_n = recent_locked = recent_pos = 0;
    line_start = -1;
    gated_lines = 0;
    miss_run = stray_run = 0;
    last_stray = -1;
    comb_epoch++;
  }

  void track_levels(float m) {
    // level tracking histogram (every 8th sample), refreshed every ~40 ms
    if ((++hdec & 7) != 0) return;
    const float v = sign * m;
    const int nb = static_cast<int>(hist.size());
    const int b = static_cast<int>((v - hlo) / (hhi - hlo) * nb);
    if (b >= 0 && b < nb) {
      hist[b]++;
      hist_count++;
    }
    if (hist_count >= static_cast<uint64_t>(fs * 0.04 / 8)) {
      float t, bl;
      if (levels_from_hist(static_cast<uint32_t>(hist_count), t, bl)) {
        tip = 0.8f * tip + 0.2f * t;
        blank = 0.8f * blank + 0.2f * bl;
        thr = 0.5f * (tip + blank);
      }
      std::fill(hist.begin(), hist.end(), 0);
      hist_count = 0;
    }
  }

  bool levels_from(const std::vector<float> &buf, size_t cnt, float sgn, float &tip_o, float &blank_o) {
    std::fill(hist.begin(), hist.end(), 0);
    const int nb = static_cast<int>(hist.size());
    uint32_t total = 0;
    for (size_t i = 0; i < cnt; i += 4) {
      const float v = sgn * buf[i];
      const int b = static_cast<int>((v - hlo) / (hhi - hlo) * nb);
      if (b < 0 || b >= nb) continue;
      hist[b]++;
      total++;
    }
    const bool ok = levels_from_hist(total, tip_o, blank_o);
    std::fill(hist.begin(), hist.end(), 0);
    return ok;
  }

  bool levels_from_hist(uint32_t total, float &tip_o, float &blank_o) {
    const int nb = static_cast<int>(hist.size());
    if (total < 1000) return false;
    uint32_t c = 0;
    int p0 = 0, p1 = nb - 1;
    for (int i = 0; i < nb; i++) {
      c += hist[i];
      if (c >= total * 0.005) { p0 = i; break; }
    }
    c = 0;
    for (int i = nb - 1; i >= 0; i--) {
      c += hist[i];
      if (c >= total * 0.005) { p1 = i; break; }
    }
    if (p1 - p0 < 20) return false;
    std::vector<float> sm(nb, 0);
    for (int i = 0; i < nb; i++) {
      float s = 0;
      for (int k = -4; k <= 4; k++) {
        const int j = i + k;
        if (j >= 0 && j < nb) s += hist[j];
      }
      sm[i] = s / 9;
    }
    float mx = 0;
    for (int i = p0; i <= p1; i++) mx = std::max(mx, sm[i]);
    std::vector<int> peaks;
    const int dist = std::max(3, (p1 - p0) / 40);
    for (int i = p0; i <= p1; i++) {
      if (sm[i] < mx * 0.02) continue;
      bool ok = true;
      for (int k = -dist; k <= dist && ok; k++) {
        const int j = i + k;
        if (k != 0 && j >= 0 && j < nb && sm[j] > sm[i]) ok = false;
      }
      if (ok && (peaks.empty() || i - peaks.back() > dist)) peaks.push_back(i);
    }
    if (peaks.size() < 2) return false;
    const float bw = static_cast<float>((hhi - hlo) / nb);
    const float tipv = static_cast<float>(hlo) + (peaks[0] + 0.5f) * bw;
    const float span = (p1 - p0) * bw;
    for (size_t k = 1; k < peaks.size(); k++) {
      const float v = static_cast<float>(hlo) + (peaks[k] + 0.5f) * bw;
      if (v - tipv > 0.12f * span) {
        tip_o = tipv;
        blank_o = v;
        return true;
      }
    }
    return false;
  }

  // acquisition: pick polarity + levels + line period from a 60 ms buffer
  void acquire() {
    float best = 0, bt = 0, bb = 0, bs = 0;
    for (float sgn : {+1.f, -1.f}) {
      if (o.force_invert && sgn > 0) continue;
      float t, b;
      if (!levels_from(acq, acq_n, sgn, t, b)) continue;
      const float th = 0.5f * (t + b);
      std::vector<double> edges;
      bool lo = false;
      size_t ls = 0;
      for (size_t i = 0; i < acq_n; i++) {
        const float v = sgn * acq[i];
        if (!lo && v < th) {
          lo = true;
          ls = i;
        } else if (lo && v >= th) {
          lo = false;
          const double w = (i - ls) / fs * 1e6;
          if (w > 3.0 && w < 6.5) edges.push_back(static_cast<double>(ls));
        }
      }
      if (edges.size() < 30) continue;
      std::vector<double> good;
      for (size_t k = 1; k < edges.size(); k++) {
        const double iv = (edges[k] - edges[k - 1]) / fs * 1e6;
        if (iv > 62.5 && iv < 65.0) good.push_back(iv);
      }
      const float score = static_cast<float>(good.size()) / (edges.size() - 1);
      KP_LOGD("video acq: sign %+.0f tip %.2f blank %.2f, %zu pulses, score %.2f", sgn, t, b, edges.size(), score);
      if (score > best && !good.empty()) {
        best = score;
        bs = sgn;
        bt = t;
        bb = b;
        std::sort(good.begin(), good.end());
        const double Tus = good[good.size() / 2];
        if (!o.std_force) {
          const VideoStd &s = std::fabs(Tus - kNtsc.T_us) < std::fabs(Tus - kPal.T_us) ? kNtsc : kPal;
          if (&s != std_) set_std(s);
        }
        T = Tus * 1e-6 * fs;
      }
    }
    if (best > 0.3f) {
      sign = bs;
      tip = bt;
      blank = bb;
      thr = 0.5f * (tip + blank);
      acquiring = false;
      line_start = -1;
      acquisitions++;
      KP_LOGI("video: acquired, sync %s end, tip %.2f blank %.2f MHz, line %.3f us -> %s, score %.2f",
              sign > 0 ? "low" : "high", tip, blank, T / fs * 1e6, std_->name, best);
    }
    acq_n = 0;
  }

  // Burst: chroma over the burst window (NTSC 5.3-7.8 us, PAL 5.6-7.85 us after the hsync edge; the
  // chroma filter delays it ~0.35 us). Updates the reference phase; returns false when there is none.
  bool burst(double ls, bool &pal_v) {
    const double us = 1e-6 * fs;
    const int64_t i0 = static_cast<int64_t>(ls + 6.0 * us), i1 = static_cast<int64_t>(ls + 7.8 * us);
    if (i1 >= static_cast<int64_t>(n) || static_cast<int64_t>(n) - i0 > static_cast<int64_t>(ring_mask)) return false;
    cf b{};
    for (int64_t i = i0; i < i1; i++) b += chroma_ring[i & ring_mask];
    b /= static_cast<float>(i1 - i0);
    const float mag = std::abs(b);
    const int64_t li = static_cast<int64_t>(line_idx);
    const double pred = c_last_line < 0 ? std::arg(b) : c_phase + c_dphi * (li - c_last_line);
    double e = std::arg(b * std::polar(1.f, static_cast<float>(-pred)));
    pal_v = false;
    if (std_ == &kPal) {
      // The burst sits 45 degrees either side of the reference: at baseband a normal line's burst (135 deg
      // in U/V) lands at -45, a V-switched line's (225 deg) at +45.
      pal_v = e > 0;
      e += pal_v ? -M_PI / 4 : M_PI / 4;
    }
    burst_e = static_cast<float>(std::fabs(e));
    if (c_last_line < 0) {
      c_phase = pred + e;
      c_dphi = 0;
    } else {
      c_phase = pred + 0.2 * e;
      c_dphi += 0.01 * e / std::max<int64_t>(1, li - c_last_line);
    }
    c_phase = std::remainder(c_phase, 2 * M_PI);
    c_last_line = li;
    c_mag += 0.05f * (mag - c_mag);
    c_err += 0.05f * (static_cast<float>(std::fabs(e)) - c_err);
    // colour killer, with hysteresis: a burst near the expected size (~1/4 of the sync depth) and steady
    const bool ok = c_mag > 0.08f * (blank - tip) && c_err < 0.5f;
    c_good = ok ? std::min(c_good + 1, 60) : std::max(c_good - 2, 0);
    if (!colour_on && c_good >= 40) colour_on = true;
    if (colour_on && c_good == 0) colour_on = false;
    return colour_on;
  }

  void render_line(double ls, bool locked_line) {
    if (cur_field < 0) return;
    const int64_t k = static_cast<int64_t>(line_idx) - field_line0 - std_->skip;
    if (comb) {
      comb_line(ls, k, locked_line);
      return;
    }
    if (k < 0 || k >= std_->active) return;
    if (locked_line) measure_levels(ls);
    if (luma_comb) top_line(ls, k);
    const bool hit = repair && patch_line(ls);
    bool pal_v = false;
    const bool col = o.colour && locked_line && burst(ls, pal_v);
    draw_line(ls, k, col, pal_v, false, false, c_phase, c_mag, false);
    if (repair) mend_rows(k, hit);
  }

  // NTSC comb: line k is measured now (levels, burst) and the previous one, k - 1, is drawn, combed with
  // k - 2 and k. A neighbour is used only if both lines are locked and in colour, in the same field and on
  // the same side of any jump in the stream (comb_epoch, and a burst on its predicted phase).
  void comb_line(double ls, int64_t k, bool locked_line) {
    CombLine cur;
    cur.ls = ls;
    cur.k = k;
    cur.field = cur_field;
    cur.epoch = comb_epoch;
    if (k >= 0 && k < std_->active) {
      if (locked_line) measure_levels(ls);
      if (luma_comb) top_line(ls, k);
      cur.hit = repair && patch_line(ls);  // before this line is anyone's neighbour
      bool pal_v = false;
      cur.col = o.colour && locked_line && burst(ls, pal_v);
      cur.ok = cur.col && burst_e < kCombMaxErr;
      cur.phase = c_phase;
      cur.mag = c_mag;
      cur.fade = adaptive && adaptive_lines > 1;  // as push() had them over this line's picture
    }
    const CombLine &m = comb_mid;
    if (m.field == cur_field && m.k == k - 1 && m.k >= 0 && m.k < std_->active) {
      const auto pair = [&](const CombLine &x, int64_t dk) {
        return m.ok && x.ok && x.field == m.field && x.k == m.k + dk && x.epoch == m.epoch;
      };
      draw_line(m.ls, m.k, m.col, false, pair(comb_up, -1), pair(cur, 1), m.phase, m.mag, m.fade);
      if (repair) mend_rows(m.k, m.hit);
    }
    comb_up = comb_mid;
    comb_mid = cur;
  }

  // draw_line without colour: each pixel the same arithmetic as there (the luma between the two samples either
  // side, then to bytes), the interpolation and the conversion four pixels at a time as vectors; the samples are
  // fetched one by one (there is no gather). Pixels whose samples are not in the ring (past the stream's end, or
  // too far back) are black, as there: a run at either end of the line, found once per line, not per pixel.
  void draw_grey(uint32_t *row, double base, float yscale) {
    const int W = o.width;
    const double *xo = px_off.data();
    const float *lr = luma_ring.data();
    const size_t mask = ring_mask;
    const int64_t nn = static_cast<int64_t>(n), far = static_cast<int64_t>(ring_mask) - 8;
    const auto first = [&](int x) { return static_cast<int64_t>(base + xo[x]); };  // pixel x's first sample
    int x0 = 0, x1 = W;  // [x0, x1): in the ring (first() only grows with x)
    while (x0 < W && nn - first(x0) > far) row[x0++] = 0xff000000u;
    while (x1 > x0 && first(x1 - 1) + 8 >= nn) row[--x1] = 0xff000000u;
    const float sg = sign, sb = sign * blank;
    int x = x0;
    for (; x + 4 <= x1; x += 4) {
      v4f a, b, f;
      for (int q = 0; q < 4; q++) {
        const double p = base + xo[x + q];
        const int64_t i0 = static_cast<int64_t>(p);
        f[q] = static_cast<float>(p - i0);
        a[q] = lr[i0 & mask];
        b[q] = lr[(i0 + 1) & mask];
      }
      const v4u px = grey_px(sg * (a + (b - a) * f - sb) * yscale);
      std::memcpy(row + x, &px, sizeof px);
    }
    for (; x < x1; x++) {
      const double p = base + xo[x];
      const int64_t i0 = static_cast<int64_t>(p);
      const float f = static_cast<float>(p - i0);
      const float v0 = lr[i0 & mask], v1 = lr[(i0 + 1) & mask];
      const v4u px = grey_px(v4f{sg * (v0 + (v1 - v0) * f - sb) * yscale, 0.f, 0.f, 0.f});
      row[x] = px[0];
    }
  }

  // draw_line in colour: as draw_grey, four pixels at a time, each through the same arithmetic as one at a time
  // (written out once more in `one`, for the pixels left over at the end). Chroma is interpolated like luma,
  // the comb taken out (comb_on: the comb's terms per pixel, worked out for the line first), turned onto the
  // burst's phase and scaled, PAL-D averaged with the line before, and converted to RGB.
  void draw_colour(uint32_t *row, double base, float yscale, double cdel, cf rot, float k_uv, bool comb_on, bool pal,
                   bool pal_v) {
    const int W = o.width;
    const double *xo = px_off.data();
    const float *lr = luma_ring.data();
    const cf *chr = chroma_ring.data();
    const size_t mask = ring_mask;
    const int64_t nn = static_cast<int64_t>(n), far = static_cast<int64_t>(ring_mask) - 8;
    const auto first = [&](int x) { return static_cast<int64_t>(base + xo[x]); };
    int x0 = 0, x1 = W;  // [x0, x1): in the ring, as in draw_grey
    while (x0 < W && nn - first(x0) > far) row[x0++] = 0xff000000u;
    while (x1 > x0 && first(x1 - 1) + 8 >= nn) row[--x1] = 0xff000000u;
    if (comb_on) {
      // the comb on every other pixel (comb_m, comb_o: see draw_line), in between the mean of the two either side
      px_cm.resize(W);
      px_cr.resize(W);
      px_ci.resize(W);
      for (int x = x0; x < x1; x++) {
        const int h = x >> 1;
        if (x & 1) {
          px_cm[x] = 1 - 0.5f * (comb_m[h] + comb_m[h + 1]);
          px_cr[x] = 0.5f * (comb_o[h].real() + comb_o[h + 1].real());
          px_ci[x] = 0.5f * (comb_o[h].imag() + comb_o[h + 1].imag());
        } else {
          px_cm[x] = 1 - comb_m[h];
          px_cr[x] = comb_o[h].real();
          px_ci[x] = comb_o[h].imag();
        }
      }
    }
    float *pu = prev_u.data(), *pv = prev_v.data();
    const float sg = sign, sb = sign * blank, rr = rot.real(), ri = rot.imag();
    int x = x0;
    for (; x + 4 <= x1; x += 4) {
      v4f a, b, f, c0r, c0i, c1r, c1i, fc;
      for (int q = 0; q < 4; q++) {
        const double p = base + xo[x + q];
        const int64_t i0 = static_cast<int64_t>(p);
        f[q] = static_cast<float>(p - i0);
        a[q] = lr[i0 & mask];
        b[q] = lr[(i0 + 1) & mask];
        const double pc = p + cdel;
        const int64_t j0 = static_cast<int64_t>(pc);
        fc[q] = static_cast<float>(pc - j0);
        const cf c0 = chr[j0 & mask], c1 = chr[(j0 + 1) & mask];
        c0r[q] = c0.real();
        c0i[q] = c0.imag();
        c1r[q] = c1.real();
        c1i[q] = c1.imag();
      }
      const v4f y = sg * (a + (b - a) * f - sb) * yscale;
      v4f cre = c0r + (c1r - c0r) * fc, cim = c0i + (c1i - c0i) * fc;
      if (comb_on) {
        v4f cm, cr, ci;
        std::memcpy(&cm, &px_cm[x], sizeof cm);
        std::memcpy(&cr, &px_cr[x], sizeof cr);
        std::memcpy(&ci, &px_ci[x], sizeof ci);
        cre = cre * cm - cr;
        cim = cim * cm - ci;
      }
      v4f u = -((cre * rr - cim * ri) * k_uv), v = -((cre * ri + cim * rr) * k_uv);
      if (pal) {
        if (pal_v) v = -v;
        v4f lu, lv;  // the line before
        std::memcpy(&lu, pu + x, sizeof lu);
        std::memcpy(&lv, pv + x, sizeof lv);
        std::memcpy(pu + x, &u, sizeof u);
        std::memcpy(pv + x, &v, sizeof v);
        u = 0.5f * (u + lu);
        v = 0.5f * (v + lv);
      }
      const v4u px = rgb_px(y + 1.140f * v, y - 0.395f * u - 0.581f * v, y + 2.032f * u);
      std::memcpy(row + x, &px, sizeof px);
    }
    for (; x < x1; x++) {  // one pixel at a time: the same, in scalars
      const double p = base + xo[x];
      const int64_t i0 = static_cast<int64_t>(p);
      const float f = static_cast<float>(p - i0);
      const float v0 = lr[i0 & mask], v1 = lr[(i0 + 1) & mask];
      const float y = sg * (v0 + (v1 - v0) * f - sb) * yscale;
      const double pc = p + cdel;
      const int64_t j0 = static_cast<int64_t>(pc);
      const float fc = static_cast<float>(pc - j0);
      const cf c0 = chr[j0 & mask], c1 = chr[(j0 + 1) & mask];
      float cre = c0.real() + (c1.real() - c0.real()) * fc, cim = c0.imag() + (c1.imag() - c0.imag()) * fc;
      if (comb_on) {
        cre = cre * px_cm[x] - px_cr[x];
        cim = cim * px_cm[x] - px_ci[x];
      }
      float u = -((cre * rr - cim * ri) * k_uv), v = -((cre * ri + cim * rr) * k_uv);
      if (pal) {
        if (pal_v) v = -v;
        const float lu = pu[x], lv = pv[x];
        pu[x] = u;
        pv[x] = v;
        u = 0.5f * (u + lu);
        v = 0.5f * (v + lv);
      }
      const v4u px = rgb_px(v4f{y + 1.140f * v, 0, 0, 0}, v4f{y - 0.395f * u - 0.581f * v, 0, 0, 0},
                            v4f{y + 2.032f * u, 0, 0, 0});
      row[x] = px[0];
    }
  }

  // up / dn: comb this line's chroma with the line above / below (see comb_line); fade: the adaptive comb's
  // fade for this line is in fade_ring
  void draw_line(double ls, int64_t k, bool col, bool pal_v, bool up, bool dn, double phase, float mag, bool fade) {
    uint32_t *row = reinterpret_cast<uint32_t *>((cur_field == 0 ? fieldA : fieldB).data()) + static_cast<size_t>(k) * o.width;
    const double a0 = std_->a0_us * 1e-6 * fs, aw = std_->aw_us * 1e-6 * fs;
    const float white = blank + 2.5f * (blank - tip);
    const float yscale = 1.f / (white - blank);
    // chroma -> U/V in luma units: the burst is 20/92.5 (NTSC) or 0.15/0.7 (PAL) of black-to-white, and
    // demodulated chroma comes out as -(U + jV)/2 against a burst of +1/2 (see fpv_sim.py's composite())
    const float k_uv = 0.215f / std::max(mag, 1e-6f);
    const cf rot = std::polar(1.f, static_cast<float>(-phase));
    const double cdel = luma_comb ? cdel_comb : 0.21e-6 * fs;  // chroma filter delay minus luma filter delay
    const bool pal = std_ == &kPal;
    // the comb's neighbours sit one line period T (Ti whole samples and Tf more) away, turned onto this
    // line's subcarrier phase: the oscillator runs on the absolute sample index, so what is mixed down one
    // line away comes out turned by w*T
    if (dn && ls + a0 + aw + cdel + T + 2 >= static_cast<double>(n)) dn = false;
    if (up && static_cast<double>(n) - (ls + a0 - T) > static_cast<double>(ring_mask) - 8) up = false;
    // The comb, on every other pixel (chroma is 1.2 MHz wide; per pixel cost twice as much for nothing
    // visible) and interpolated in between below: chroma becomes c * (1 - comb_m) - comb_o.
    const bool comb_on = col && (up || dn || fade);
    if (comb_on) {
      const int64_t Ti = static_cast<int64_t>(T);
      const float Tf = static_cast<float>(T - Ti);
      const cf turn = std::polar(1.f, static_cast<float>(-comb_w * T));  // the line above; below: conj
      const float wk = std::fabs(yscale) / (kCombNone - kCombFull), w1 = kCombNone / (kCombNone - kCombFull);
      const int nc = (o.width + 1) / 2;
      comb_m.resize(nc + 1);
      comb_o.resize(nc + 1);
      const double step = 2 * aw / o.width;
      for (int h = 0; h < nc; h++) {
        float m = 0, orr = 0, oi = 0;
        const double p = ls + a0 + step * h, pc = p + cdel;
        const int64_t i0 = static_cast<int64_t>(p + 0.5), j0 = static_cast<int64_t>(pc);
        if (j0 + Ti + 3 < static_cast<int64_t>(n) &&
            static_cast<int64_t>(n) - (i0 - Ti) <= static_cast<int64_t>(ring_mask) - 8) {
          // a neighbour's weight: 1 while its luma here is within kCombFull of this line's, 0 past kCombNone
          const float y0 = luma_ring[i0 & ring_mask];
          const float wu = up ? std::clamp(w1 - std::fabs(luma_ring[(i0 - Ti) & ring_mask] - y0) * wk, 0.f, 1.f) : 0.f;
          const float wd = dn ? std::clamp(w1 - std::fabs(luma_ring[(i0 + Ti) & ring_mask] - y0) * wk, 0.f, 1.f) : 0.f;
          if (wu + wd > 0) {
            // A neighbour turned onto this line has chroma -c and the same luma detail, so c + the neighbours'
            // mean is twice the detail: take out half of it per unit of the best weight (all of it at 1).
            m = 0.5f * std::max(wu, wd);
            const float sc = m / (wu + wd);
            const float fc = static_cast<float>(pc - j0);
            // chroma_ring at j + t (0 <= t < 2), in plain floats: branch-free, and std::complex cost more here
            const auto take = [&](int64_t j, float t, float rr, float ri) {
              const int iw = static_cast<int>(t);
              j += iw;
              t -= static_cast<float>(iw);
              const cf a = chroma_ring[j & ring_mask], b = chroma_ring[(j + 1) & ring_mask];
              const float xr = a.real() + (b.real() - a.real()) * t, xi = a.imag() + (b.imag() - a.imag()) * t;
              orr += xr * rr - xi * ri;
              oi += xr * ri + xi * rr;
            };
            if (wu > 0) take(j0 - Ti - 1, fc + 1 - Tf, turn.real() * sc * wu, turn.imag() * sc * wu);
            if (wd > 0) take(j0 + Ti, fc + Tf, turn.real() * sc * wd, -turn.imag() * sc * wd);
          }
        }
        if (fade) {
          // Adaptive comb: where this line differs from the one before, colour is faded out, because there
          // the detail that passes for colour cannot be cancelled against that line. The comb above can use
          // the line below as well, so only what it left in (all of it at m = 0: no line matches, a stroke one
          // line tall; none at m = 0.5) is faded. The fade was worked out ahead of the chroma low-pass: seen
          // through that filter (its main lobe, as three points) it is what fading ahead of it would have done.
          const int64_t jf = j0 - fade_back;
          const float f = 0.25f * (fade_ring[(jf - fade_step) & ring_mask] + fade_ring[(jf + fade_step) & ring_mask]) +
                          0.5f * fade_ring[jf & ring_mask];
          const float keep = 1.f - f * (1.f - 2.f * m);
          m = 1.f - (1.f - m) * keep;
          orr *= keep;
          oi *= keep;
        }
        comb_m[h] = m;
        comb_o[h] = cf(orr, oi);
      }
      comb_m[nc] = comb_m[nc - 1];
      comb_o[nc] = comb_o[nc - 1];
    }
    if (!col) {
      draw_grey(row, ls + a0, yscale);
      return;
    }
    draw_colour(row, ls + a0, yscale, cdel, rot, k_uv, comb_on, pal, pal_v);
  }

  // The top of the picture with the luma comb. The first picture line of a field has blanking above it, not
  // picture, and the luma comb works each line out against the line before. Against blanking it leaves half of
  // the colour in the brightness as dots, and the adaptive comb, which sees a line that differs all the way
  // along, fades the colour out as it does under any horizontal edge: the top rows of every frame came out grey.
  // There is nothing above that line to keep rainbows off, so its picture is worked out again here, from what
  // the comb's ring holds of it, the way the decoder does without the luma comb: brightness through the notch
  // (which the adaptive comb falls back to there anyway), colour straight from the signal, not faded. Only the
  // picture's own stretch of that one line is replaced and nothing in push() changes: levels, burst and every
  // other line come out exactly as they did.
  // Which line that is depends on the camera and on where the field was found to start: the first drawn row
  // (k = 0) or one of the next few, with blank rows above it. So it is looked for: the first of rows 0..kTopRows
  // that has picture (it averages kTopPicture sync depths above blanking or more: the level from which the
  // adaptive comb starts to see a difference) while every row above it, and the line before row 0, averages
  // within kTopBlank sync depths of blanking. The second limit is the tighter one on purpose: black picture
  // sits 7.5 IRE (0.19 sync depths) above blanking, and a row under black rows of picture is a line inside the
  // picture, which keeps the comb's fade. (A camera that sends black at the blanking level cannot be told
  // apart this way: under up to kTopRows black rows at the top of its picture, the first brighter row is taken
  // for the top line and drawn as without the luma comb.) A field that begins with
  // the standard's half line has it just above row 0 here: then row 0 has blanking above its left half only,
  // and only that half is done (to the middle, where the half line's picture has begun and the comb is right
  // again). A line further down that follows a dark line is not touched: there the fade is Play RX's trade.
  static constexpr int kTopRows = 4;
  static constexpr float kTopPicture = 0.25f, kTopBlank = 0.10f;
  void top_line(double ls, int64_t k) {
    if (k > kTopRows) return;
    const double a0 = std_->a0_us * 1e-6 * fs, aw = std_->aw_us * 1e-6 * fs;
    // no picture over the part [x0, x1) of the width of the line that starts at l?
    const auto blanking = [&](double l, double x0, double x1, float limit = kTopBlank) {
      float m;
      return ring_mean(l + a0 + aw * x0, l + a0 + aw * x1, m) && (m - blank) * inv_depth < limit;
    };
    if (blanking(ls, 0, 1, kTopPicture)) return;
    for (int64_t up = 2; up <= k + 1; up++)  // the rows above the line before, and the line before row 0
      if (!blanking(ls - static_cast<double>(up) * T, 0, 1)) return;
    const bool whole = blanking(ls - T, 0, 1);
    if (!whole && (k > 0 || !blanking(ls - T, 0, 0.4))) return;
    // The whole line: from the first good point patch_line may take (8 samples ahead of the picture) to its
    // last one in luma (20 after it; those of its chroma patches further on lie in blanking, where there is no
    // colour either way). The left half: to the middle, the last kBlend samples blended into what push() made.
    // The filters start kWarm samples ahead, in the sync pulse, and have settled long before the picture.
    constexpr int kWarm = 128, kBlend = 32;
    const int64_t s0 = static_cast<int64_t>(ls + a0) - 8;
    const int64_t s1 = whole ? static_cast<int64_t>(ls + a0 + aw) + 21 : static_cast<int64_t>(ls + a0 + 0.5 * aw);
    const int64_t back = static_cast<int64_t>(n) - (s0 - kWarm);
    if (s1 >= static_cast<int64_t>(n) || back > static_cast<int64_t>(ring_mask) - 32) return;
    const auto fresh = [](Biquad b) {
      b.z1 = b.z2 = 0;
      return b;
    };
    Biquad nt = fresh(luma_notch_w), r1 = fresh(cl_r1), r2 = fresh(cl_r2), i1 = fresh(cl_i1), i2 = fresh(cl_i2);
    // Colour: only the adaptive comb on its own takes it from the line difference and fades it (push). Otherwise
    // chroma_ring already has it straight from the signal; with the 3-line comb the fade is only noted, and
    // draw_line uses it where that comb finds no matching line below either, which is left as it is. The
    // oscillator turns by comb_w per sample: back to where it was at the first sample.
    const bool chroma = o.colour && adaptive && !comb;
    const std::complex<double> was = std::polar(1.0, -comb_w * static_cast<double>(back));
    cf osc = lo * cf(static_cast<float>(was.real()), static_cast<float>(was.imag()));
    // push()'s line difference reads the line before between two samples, which takes 3 % off the subcarrier
    // and turns it by half a degree. The burst went through it and the picture's colour is measured against
    // the burst, so this line's colour gets the same, or it would come out that much more saturated than the
    // rest: half of (1 - the subcarrier as read prev_back samples back, prev_fr of the way to the next sample).
    const std::complex<double> prev = std::polar(1.0, -comb_w * static_cast<double>(prev_back)) *
                                      ((1.0 - prev_fr) + static_cast<double>(prev_fr) * std::polar(1.0, comb_w));
    const cf gain(static_cast<float>(0.5 * (1.0 - prev.real())), static_cast<float>(-0.5 * prev.imag()));
    for (int64_t i = s0 - kWarm; i < s1; i++) {
      const CombTap &t = comb_ring[i & ring_mask];
      const float y = nt.run(t.wide);
      const float mix = whole ? 1.f : std::min(1.f, static_cast<float>(s1 - i) * (1.f / kBlend));
      if (i >= s0) luma_ring[i & ring_mask] += (y - luma_ring[i & ring_mask]) * mix;
      if (!chroma) continue;
      const cf c = t.d * std::conj(osc);
      osc *= lo_step;
      const cf uv(r2.run(r1.run(c.real())), i2.run(i1.run(c.imag())));
      if (i >= s0) chroma_ring[i & ring_mask] += (uv * gain - chroma_ring[i & ring_mask]) * mix;
    }
  }

  // Sparkle repair: up to 3 badly hit rows in a row are redrawn from the good rows above and below them in the
  // same field (blended by distance), once the row below has come in. Nothing older is used, so there is no
  // tearing in motion; a longer run is left as it is.
  void mend_rows(int64_t k, bool hit) {
    if (mend_field != fields_seen || mend_cur != cur_field) {
      mend_field = fields_seen;
      mend_cur = cur_field;
      bad_from = -1;
    }
    if (hit) {
      if (bad_from < 0) bad_from = k;
      return;
    }
    if (bad_from >= 1 && k - bad_from <= 3) {
      uint8_t *buf = (cur_field == 0 ? fieldA : fieldB).data();
      const size_t rowb = static_cast<size_t>(o.width) * 4;
      const uint8_t *top = buf + static_cast<size_t>(bad_from - 1) * rowb, *bot = buf + static_cast<size_t>(k) * rowb;
      const int span = static_cast<int>(k - (bad_from - 1));
      for (int64_t r = bad_from; r < k; r++) {
        const int wb = static_cast<int>(r - (bad_from - 1)), wt = span - wb;
        uint8_t *dst = buf + static_cast<size_t>(r) * rowb;
        for (size_t x = 0; x < rowb; x++) dst[x] = static_cast<uint8_t>((top[x] * wt + bot[x] * wb + span / 2) / span);
        lines_mended++;
      }
    }
    bad_from = -1;
  }

  // Sparkle repair. Patches the picture of the line starting at ls: each run of marked samples (widened by the
  // filters' spread) becomes a straight line between the points either side, in the filtered luma and chroma.
  // Chroma is patched further on than luma: its low-pass is narrower, and a patch that ended inside the click's
  // own response there drew a ramp to a wrong colour and left the rest as a coloured streak. Returns true when
  // the line is still badly hit (over 8 % of it lost, or a run too long to patch): mend_rows() then redraws it
  // from its neighbours.
  bool patch_line(double ls) {
    const double a0 = std_->a0_us * 1e-6 * fs, aw = std_->aw_us * 1e-6 * fs;
    const int64_t s0 = static_cast<int64_t>(ls + a0) - 4, s1 = static_cast<int64_t>(ls + a0 + aw) + 4;
    constexpr int kBack = 3, kFwd = 8, kMaxRun = 64;  // luma: filter spread (samples) either side; longest patch
    // a run may end kFwd past s1, and the furthest patch ends on the sample its own spread after that
    const int reach = kFwd + std::max({kFwd, rep_fwd_c, rep_fwd_low, rep_fwd_wide});
    if (s1 + reach >= static_cast<int64_t>(n) || static_cast<int64_t>(n) - s0 > static_cast<int64_t>(ring_mask) - 32)
      return false;
    // [a, b) of a ring becomes a straight line between the samples either side of it, a - 1 and b
    const auto ramp = [&](auto &ring, int64_t a, int64_t b) {
      const auto va = ring[(a - 1) & ring_mask], vb = ring[b & ring_mask];
      const float inv = 1.f / static_cast<float>(b - a + 1);
      for (int64_t q = a; q < b; q++) ring[q & ring_mask] = va + (vb - va) * (static_cast<float>(q - a + 1) * inv);
    };
    // [a, b) of one of the comb's taps from the same place two lines earlier (2 T back, between two samples)
    const double up2 = std::ceil(2 * T);
    const int64_t back2 = static_cast<int64_t>(up2);
    const float fr2 = static_cast<float>(up2 - 2 * T);
    const auto two_up = [&](float CombTap::*v, int64_t a, int64_t b) {
      for (int64_t q = a; q < b; q++) {
        const float x0 = comb_ring[(q - back2) & ring_mask].*v, x1 = comb_ring[(q - back2 + 1) & ring_mask].*v;
        comb_ring[q & ring_mask].*v = x0 + (x1 - x0) * fr2;
      }
    };
    const bool taps = luma_comb && static_cast<int64_t>(n) - (s0 - kBack - back2) <= static_cast<int64_t>(ring_mask);
    // Two clicks close together. A patch ends on a point taken to be good: kFwd samples after its marked run in
    // luma, rep_fwd_c in chroma. It is not good when another click shows there: the patch then ramped to a wrong
    // value and the next one started from that ramp, a streak as bright as the click it was meant to remove.
    // Such runs are patched as one, from before the first to after the last. A later click shows at the good
    // point
    //  - in luma, when its run begins up to kFwd - rise samples after this one's end: a click shows `rise`
    //    samples after its first mark (which is 2 ahead of it): at once through the comb's wide filter, a
    //    sample later through the notch's low-pass;
    //  - in chroma, when the good point falls in the main lobe of the low-pass's answer to it, rep_lobe0 to
    //    rep_lobe1 after its first mark. A click that began earlier than that has swung back by then: little is
    //    left of it, and one longer patch across both did no better. Except with the adaptive comb on its own:
    //    there a click also fades the colour around it for as long as its low band lasts, and any click that
    //    began in the stretch before the lobe counts as well.
    // chain() gives the end of the last marked run that belongs with the run ending at e: a later run that begins
    // from..upto samples after it, and on from that one's end; e itself for a click on its own, which is patched
    // exactly as before. Luma and chroma chain separately, so that no more of either is drawn over than has to
    // be. The comb's taps have no good points to spoil.
    const int rise = luma_comb ? 2 : 3;
    const int c_from = adaptive && !comb ? 0 : rep_fwd_c - rep_lobe1, c_upto = rep_fwd_c - rep_lobe0;
    const auto chain = [&](int64_t e, int from, int upto) {
      for (;;) {
        const int64_t last = std::min<int64_t>(e + upto, s1 - 1);  // a run begins before s1, as below
        int64_t q = e + from;
        while (q <= last && !(bad_ring[q & ring_mask] && !bad_ring[(q - 1) & ring_mask])) q++;
        if (q > last) return e;
        while (q < s1 + kFwd && bad_ring[q & ring_mask]) q++;
        e = q;
      }
    };
    // ..._end: the end of the chain the last patch covers; ..._solo: the end of a chain too long for one patch
    int64_t lost = 0, luma_end = s0, chroma_end = s0, luma_solo = s0, chroma_solo = s0;
    bool too_long = false, luma_fits = true, chroma_fits = true;
    for (int64_t i = s0; i < s1;) {
      if (!bad_ring[i & ring_mask]) {
        i++;
        continue;
      }
      int64_t j = i;
      while (j < s1 + kFwd && bad_ring[j & ring_mask]) j++;
      lost += j - i;
      // the limit is on the luma patch, as it always was; chroma's longer spread does not count against it. A
      // chain too long for one patch: in that ring (luma or chroma; the other may still find a chain that fits
      // among the same runs) every run of it is patched on its own, exactly as each was before chains were
      // looked for (..._solo: up to where). Not left alone: the comb's taps would stay as they are too, and
      // the click would show in the line below. Not a shorter chain from each run in turn: that looked for one
      // again from every run, slow on a line full of marks. And the row does not count as badly hit for it.
      // That is a trade, measured with marked clicks: redrawing the row from its neighbours hides a dense
      // burst (eight clicks 8 samples apart) better than these patches do, but for three or four clicks 20
      // to 24 apart it costs a row of detail where the short patches do less harm. Noise down to a CNR of
      // 5 dB produced neither.
      const auto fits = [&](int64_t e) { return e - i + kBack + kFwd <= kMaxRun; };
      if (i >= luma_end) {
        luma_end = j;
        if (i >= luma_solo) {
          const int64_t e = chain(j, 1, kFwd - rise);
          (fits(e) ? luma_end : luma_solo) = e;
        }
        luma_fits = fits(luma_end);
        if (luma_fits) ramp(luma_ring, i - kBack, luma_end + kFwd);
      }
      if (i >= chroma_end) {
        chroma_end = j;
        if (i >= chroma_solo) {
          const int64_t e = chain(j, c_from, c_upto);
          (fits(e) ? chroma_end : chroma_solo) = e;
        }
        chroma_fits = fits(chroma_end);
        if (chroma_fits) ramp(chroma_ring, i - kBack, chroma_end + rep_fwd_c);
      }
      if (luma_fits && chroma_fits) {
        if (taps) {
          // The comb's look at the line before: left as it is, the next line would take half the click from
          // here (its luma through the comb, its colour through the adaptive comb's line difference) and
          // show it again one line down, where nothing is marked. That line reads these samples a line
          // period after they came in; this runs well before, as soon as the next line's sync has passed.
          // The stand-in is the line two up, whose colour has the same phase (a straight line would have
          // no colour, and the line below would lose half of its own there); where that line looks
          // different, the adaptive comb sees it in the low band like any other difference.
          two_up(&CombTap::d, i - 1, j + rep_fwd_wide);
          two_up(&CombTap::wide, i - 1, j + rep_fwd_wide);
          two_up(&CombTap::low, i - kBack, j + rep_fwd_low);
        }
        clicks_repaired++;
      } else {
        too_long = true;
      }
      i = j;
    }
    return too_long || lost > 0.08 * aw;
  }

  // The mean of luma_ring over [from, to) into out; false when that stretch is not in the ring. (Not a NaN
  // return: this file is built with -ffast-math, under which a test for NaN may be compiled away.)
  bool ring_mean(double from, double to, float &out) {
    const int64_t i0 = static_cast<int64_t>(from), i1 = static_cast<int64_t>(to);
    if (i1 >= static_cast<int64_t>(n) || static_cast<int64_t>(n) - i0 > static_cast<int64_t>(ring_mask)) return false;
    float s = 0;
    for (int64_t i = i0; i < i1; i++) s += luma_ring[i & ring_mask];
    out = sign * s / static_cast<float>(i1 - i0);
    return true;
  }

  void measure_levels(double ls) {
    const double us = 1e-6 * fs;
    float t, b;
    if (!ring_mean(ls + 1.2 * us, ls + 3.6 * us, t)) return;  // hsync is 4.7 us
    if (!ring_mean(ls + 5.8 * us, ls + 8.8 * us, b)) return;  // back porch (burst averages out)
    if (b - t < 0.05f) return;
    const float a = gated_lines < 64 ? 0.1f : 1.f / 64;
    tip += a * (t - tip);
    blank += a * (b - blank);
    thr = 0.5f * (tip + blank);
    gated_lines++;
  }

  // Smooth lines: field f (the upper field when upper) into the frame at its own lines, the lines between from
  // the field lines above (p) and below (q), averaged along whichever of 5 directions matches best (so a
  // diagonal edge stays a clean diagonal instead of a staircase).
  void emit_field(const std::vector<uint8_t> &f, bool upper) {
    const int W = o.width, A = std_->active;
    const size_t rowb = static_cast<size_t>(W) * 4;
    uint8_t *out = frame.data();
    for (int r = 0; r < A; r++) std::memcpy(out + (2 * r + (upper ? 0 : 1)) * rowb, f.data() + r * rowb, rowb);
    // every row's brightness once: the search below compares 5 pairs of them per pixel
    lum_rows.resize(static_cast<size_t>(W) * A);
    for (size_t i = 0, e = lum_rows.size(); i < e; i++) {
      const uint8_t *px = f.data() + i * 4;
      lum_rows[i] = static_cast<uint16_t>(px[0] + 2 * px[1] + px[2]);
    }
    best_dir.resize(W);
    const auto blend = [](const uint8_t *a, const uint8_t *b, uint8_t *dst) {  // per byte (a + b + 1) / 2
      uint32_t x, y;
      std::memcpy(&x, a, 4);
      std::memcpy(&y, b, 4);
      const uint32_t z = (x | y) - (((x ^ y) >> 1) & 0x7f7f7f7fu);
      std::memcpy(dst, &z, 4);
    };
    for (int r = 0; r < A; r++) {
      const int line = upper ? 2 * r + 1 : 2 * r;
      const int rp = upper ? r : std::max(r - 1, 0), rq = upper ? std::min(r + 1, A - 1) : r;
      const uint8_t *p = f.data() + rp * rowb, *q = f.data() + rq * rowb;
      const uint16_t *lp = lum_rows.data() + static_cast<size_t>(rp) * W, *lq = lum_rows.data() + static_cast<size_t>(rq) * W;
      uint8_t *dst = out + line * rowb;
      int8_t *best = best_dir.data();
      // The direction d (p at x + d against q at x - d) with the lowest cost, straight down preferred; ties go
      // to the first of -2, -1, 0, 1, 2. Away from the row's ends all five exist: no branches, so it vectorises.
      const auto edge = [&](int x) {
        int b = 0, bc = 1 << 30;
        for (int d = std::max(-2, std::max(-x, x - (W - 1))), de = std::min(2, std::min(x, W - 1 - x)); d <= de; d++) {
          const int c = std::abs(lp[x + d] - lq[x - d]) + (d ? 24 + 8 * std::abs(d) : 0);
          if (c < bc) bc = c, b = d;
        }
        best[x] = static_cast<int8_t>(b);
      };
      for (int x = 0; x < std::min(2, W); x++) edge(x);
      for (int x = 2; x < W - 2; x++) {
        int bc = std::abs(lp[x - 2] - lq[x + 2]) + 40, b = -2;
        const int c1 = std::abs(lp[x - 1] - lq[x + 1]) + 32, c2 = std::abs(lp[x] - lq[x]);
        const int c3 = std::abs(lp[x + 1] - lq[x - 1]) + 32, c4 = std::abs(lp[x + 2] - lq[x - 2]) + 40;
        b = c1 < bc ? -1 : b, bc = std::min(bc, c1);
        b = c2 < bc ? 0 : b, bc = std::min(bc, c2);
        b = c3 < bc ? 1 : b, bc = std::min(bc, c3);
        b = c4 < bc ? 2 : b;
        best[x] = static_cast<int8_t>(b);
      }
      for (int x = std::max(2, W - 2); x < W; x++) edge(x);
      for (int x = 0; x < W; x++) blend(p + (x + best[x]) * 4, q + (x - best[x]) * 4, dst + x * 4);
    }
    frames_out++;
    if (sink) sink(Frame{frame.data(), o.width, 2 * std_->active, std_});
  }

  void emit(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    const size_t rowb = static_cast<size_t>(o.width) * 4;
    for (int r = 0; r < std_->active; r++) {
      std::memcpy(frame.data() + (2 * r) * rowb, a.data() + r * rowb, rowb);
      std::memcpy(frame.data() + (2 * r + 1) * rowb, b.data() + r * rowb, rowb);
    }
    frames_out++;
    if (sink) sink(Frame{frame.data(), o.width, 2 * std_->active, std_});
  }

  void close_line(double next_start) {
    const bool vs = line_low > 0.5 * T;
    if (vs && !prev_line_vsync && line_has_longrun) {
      // field start: phase of the broad-pulse start inside this line
      double ph = (static_cast<double>(line_longrun_start) - line_start) / T;
      ph -= std::floor(ph + 0.25);  // -> [-0.25, 0.75)
      const bool odd = std::fabs(ph) < 0.25;
      // smooth lines: A is the lower field, B the upper one (B's line k is drawn above A's line k)
      const bool smooth = smooth_lines_on();
      if (cur_field == 0) {
        haveA = true;
        if (o.fields) smooth ? emit_field(fieldA, false) : emit(fieldA, fieldA);
      }
      if (cur_field == 1) {
        haveB = true;
        if (o.fields) smooth ? emit_field(fieldB, true) : emit(fieldB, fieldB);
      }
      if (!o.fields && haveA && haveB) {
        emit(fieldA, fieldB);
        haveA = haveB = false;
      }
      cur_field = odd ? 0 : 1;
      field_line0 = static_cast<int64_t>(line_idx);
      fields_seen++;
      if (odd) haveB = false;  // a frame is A then B
    }
    prev_line_vsync = vs;
    line_idx++;
    line_low = 0;
    line_has_longrun = false;
    line_start = next_start;
    refresh_switches();
  }

  void on_hsync(double edge) {
    if (line_start < 0) {
      line_start = edge;
      return;
    }
    const double pred = line_start + T;
    if (std::fabs(edge - pred) <= 0.08 * T) {
      const double ns = 0.6 * edge + 0.4 * pred;
      T = 0.995 * T + 0.005 * (ns - line_start);
      miss_run = 0;
      stray_run = 0;
      count_line(true);
      render_line(line_start, true);
      close_line(ns);
      check_lost();
      return;
    }
    // an hsync-width pulse off the grid: noise, an equalising pulse, or the picture has jumped
    if (last_stray >= 0 && std::fabs(edge - last_stray - T) < 0.03 * T) stray_run++;
    else stray_run = 1;
    last_stray = edge;
    if (stray_run >= 6 && miss_run >= 4) {
      rephases++;
      comb_epoch++;
      KP_LOGD("video: re-phased the line grid by %.1f us", (edge - line_start) / fs * 1e6);
      line_start = edge;
      line_low = 0;
      line_has_longrun = false;
      stray_run = 0;
      miss_run = 0;
    }
  }
};

// ---- front end ----------------------------------------------------------------------------------------
// kepyur: block-wise and branch-free so the compiler vectorises it (the spike's per-sample loop used a whole
// phone core at 40 MS/s). Per chunk of <= kTab samples: DC removal (block mean, smoothed), mix by a
// precomputed rotation table, halfband /2 over a work array with 22 samples of history, discriminator.
FrontEnd::FrontEnd(double fs_in, double shift_hz, FrameSink sink, const DecoderOpts &o)
    : _fs_in(fs_in), _fs_out(fs_in >= 40e6 ? fs_in / 2 : fs_in), _decimate(fs_in >= 40e6) {
  for (int i = 0; i < kHb; i++) {
    const int k = i - kHb / 2;
    const double w = 0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / kHb);
    _h[i] = k == 0 ? 0.5f : static_cast<float>(std::sin(M_PI * k / 2) / (M_PI * k) * w);
  }
  _wr.assign(kHb - 1 + kTab, 0.f);
  _wi.assign(kHb - 1 + kTab, 0.f);
  _dec = new Decoder(_fs_out, std::move(sink), o);
  set_shift(shift_hz);
}

FrontEnd::~FrontEnd() { delete _dec; }

void FrontEnd::set_shift(double s) {
  _shift = s;
  _tr.resize(kTab);
  _ti.resize(kTab);
  for (int i = 0; i < kTab; i++) {
    const double a = -2 * M_PI * s / _fs_in * i;
    _tr[i] = static_cast<float>(std::cos(a));
    _ti[i] = static_cast<float>(std::sin(a));
  }
  const double a = -2 * M_PI * s / _fs_in * kTab;
  _stepN = cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
  _rot = cf(1, 0);
}

DecoderStats FrontEnd::stats() const { return _dec->stats(); }

namespace {
// branch-free atan2 (a polynomial, max error ~1e-4 rad) so the discriminator loop vectorises
inline float atan2_bf(float y, float x) {
  const float ax = std::fabs(x), ay = std::fabs(y);
  const float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
  const float a = mn / (mx + 1e-20f);
  const float s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  r = ay > ax ? 1.57079637f - r : r;
  r = x < 0 ? 3.14159274f - r : r;
  return y < 0 ? -r : r;
}
}  // namespace

void FrontEnd::demod(const cf *xs, size_t n, std::vector<float> &out, std::vector<uint8_t> *bad) {
  out.clear();
  const bool rep = bad && repair_on();
  // sparkle repair keeps the end of the last call's envelope in front of this call's (_dtail: the discriminator
  // output of those samples), so that a dip at the start of a call is seen whole
  if (!rep) {
    _env.clear();
    _dtail.clear();
  }
  const size_t hist = _env.size();
  if (bad) bad->clear();
  const float k = static_cast<float>(_fs_out / (2 * M_PI) / 1e6);  // rad/sample -> MHz
  constexpr int H = kHb - 1;
  while (n > 0) {
    const int m = static_cast<int>(std::min<size_t>(n, kTab));
    // DC: block mean, smoothed across blocks (the adapter's DC line)
    float sr = 0, si = 0;
    for (int i = 0; i < m; i++) {
      sr += xs[i].real();
      si += xs[i].imag();
    }
    _dc += (cf(sr / m, si / m) - _dc) * 0.3f;
    const float dr = _dc.real(), di = _dc.imag();
    const float pr = _rot.real(), pi = _rot.imag();
    float *wr = _wr.data() + H, *wi = _wi.data() + H;
    const float *tr = _tr.data(), *ti = _ti.data();
    for (int i = 0; i < m; i++) {
      const float xr = xs[i].real() - dr, xi = xs[i].imag() - di;
      const float cr = pr * tr[i] - pi * ti[i], ci = pr * ti[i] + pi * tr[i];  // rot * step^i
      wr[i] = xr * cr - xi * ci;
      wi[i] = xr * ci + xi * cr;
    }
    _rot *= _stepN;
    if (m < kTab) {  // a short chunk: the next one starts at step^m, not step^kTab
      _rot = cf(pr, pi) * cf(tr[m], ti[m]);
    }
    _rot /= std::abs(_rot);
    // halfband /2 (or pass through), then the discriminator
    const size_t o0 = out.size();
    if (_decimate) {
      const float *r = _wr.data(), *q = _wi.data();
      int j0 = _phase;
      const int nout = m - 1 - j0 >= 0 ? (m - 1 - j0) / 2 + 1 : 0;  // windows that end inside this chunk
      _yr.resize(nout);
      _yi.resize(nout);
      for (int j = 0; j < nout; j++) {
        const int s0 = j0 + 2 * j;
        float a = 0.5f * r[s0 + kHb / 2], b = 0.5f * q[s0 + kHb / 2];
        for (int t = 0; t < kHb; t += 2) {
          a += _h[t] * r[s0 + t];
          b += _h[t] * q[s0 + t];
        }
        _yr[j] = a;
        _yi[j] = b;
      }
      _phase = j0 + 2 * nout - m;  // where the next window starts, relative to the next chunk
      std::memmove(_wr.data(), _wr.data() + m, H * sizeof(float));
      std::memmove(_wi.data(), _wi.data() + m, H * sizeof(float));
    } else {
      _yr.assign(wr, wr + m);
      _yi.assign(wi, wi + m);
    }
    const int ny = static_cast<int>(_yr.size());
    out.resize(o0 + ny);
    float *d = out.data() + o0;
    float lr = _prev.real(), li = _prev.imag();
    if (ny > 0) {
      d[0] = atan2_bf(_yi[0] * lr - _yr[0] * li, _yr[0] * lr + _yi[0] * li) * k;
      for (int j = 1; j < ny; j++) {
        const float re = _yr[j] * _yr[j - 1] + _yi[j] * _yi[j - 1];
        const float im = _yi[j] * _yr[j - 1] - _yr[j] * _yi[j - 1];
        d[j] = atan2_bf(im, re) * k;
      }
      _prev = cf(_yr[ny - 1], _yi[ny - 1]);
    }
    if (rep) {
      const size_t e0 = _env.size();
      _env.resize(e0 + ny);
      float *e = _env.data() + e0;
      for (int j = 0; j < ny; j++) e[j] = _yr[j] * _yr[j] + _yi[j] * _yi[j];
    }
    xs += m;
    n -= m;
  }
  // Sparkle repair: a click is where the envelope drops under kDip of its recent level (~2 ms) for a moment AND
  // the phase slips about one whole turn across that dip (the discriminator's area over it, against the level
  // either side), most of it at once. Indoors, echoes put dips in the envelope at certain brightness levels with
  // no slip: those are picture, not clicks, and are left alone. They last as long as the picture stays at that
  // brightness, and over a long dip the "area" is only how much brighter or darker that stretch is than its
  // surroundings, times its length: any amount. So a dip counts only if
  //  - it is short: noise clicks last 1-3 samples at 20 MS/s (the channel is ~18 MHz wide), a fade or a
  //    stretch of picture at an echo's notch lasts longer;
  //  - its area is near one turn, not merely above a part of one;
  //  - it is abrupt: some sample of it is kJump away from the level either side. A click's turn comes within
  //    a sample or two (8-14 MHz at 20 MS/s, measured on noise). A picture edge crossing a notch, or a thin
  //    white stroke of OSD text whose brightness sits on one, can pass the first two tests but stays under
  //    that: black to white is 5.3 MHz at 8 MHz deviation, and the echo's own phase swing adds up to 2.5 when
  //    it is 3 dB under the direct signal. (With a stronger echo, or over a strongly coloured background,
  //    such strokes can still be taken for clicks. 7 MHz would find 2 % more of the real clicks and take
  //    two to three times as many strokes; 9 MHz would lose a tenth of the clicks.)
  if (rep && _env.size() > hist) {
    constexpr float kDip = 0.12f;  // -9 dB
    constexpr size_t kWiden = 2;
    constexpr double kSlip = 0.7, kSlipMax = 1.5;  // turns
    constexpr float kJump = 8.f;  // MHz
    const size_t max_dip = std::max<size_t>(1, static_cast<size_t>(std::lround(0.4e-6 * _fs_out)));
    const size_t M = _env.size(), N = M - hist;  // N == out.size()
    double m = 0;
    for (size_t i = hist; i < M; i++) m += _env[i];
    m /= static_cast<double>(N);
    _pref = _pref <= 0 ? static_cast<float>(m) : _pref + 0.1f * (static_cast<float>(m) - _pref);
    const float thr = kDip * _pref;
    const double turns = 1e6 / _fs_out;  // MHz x one sample -> turns
    const auto dsc = [&](size_t q) { return q < hist ? _dtail[q] : out[q - hist]; };  // alongside _env
    bad->assign(out.size(), 0);
    size_t i = 0;
    while (i < M && _env[i] < thr) i++;  // under way where the kept samples begin: it is longer than a click
    while (i < M) {
      if (_env[i] >= thr) {
        i++;
        continue;
      }
      size_t j = i;
      while (j < M && _env[j] < thr) j++;
      const size_t a = i >= kWiden ? i - kWiden : 0, b = std::min(M, j + kWiden);
      // A dip cut off by the end of this call cannot be told yet and is left alone. The next call sees it whole
      // and marks what it still can: the samples that were this call's have been handed on by then.
      if (j - i <= max_dip && a >= 3 && b + 3 <= M) {
        const float base = (dsc(a - 1) + dsc(a - 2) + dsc(a - 3) + dsc(b) + dsc(b + 1) + dsc(b + 2)) / 6.f;
        double area = 0;
        float lo = 0, hi = 0;
        for (size_t q = a; q < b; q++) {
          const float v = dsc(q) - base;
          area += v;
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
        const double slip = std::fabs(area * turns);
        if (slip > kSlip && slip < kSlipMax && (area > 0 ? hi : -lo) > kJump)
          for (size_t q = std::max(a, hist); q < b; q++) (*bad)[q - hist] = 1;
      }
      i = j;
    }
    // the end of this call, for the next one: room for a whole dip, its widening and the level either side
    const size_t keep = std::min(M, max_dip + 2 * kWiden + 8);
    _dtail.resize(keep);
    for (size_t q = 0; q < keep; q++) _dtail[q] = dsc(M - keep + q);
    _env.erase(_env.begin(), _env.end() - static_cast<std::ptrdiff_t>(keep));
  }
}

void FrontEnd::decode(const float *d, size_t n, const uint8_t *bad) {
  _dec->push_block(d, bad, n);
}

void FrontEnd::feed(const cf *xs, size_t n) {
  demod(xs, n, _dbuf, &_badbuf);
  decode(_dbuf.data(), _dbuf.size(), _badbuf.size() == _dbuf.size() ? _badbuf.data() : nullptr);
}

}  // namespace kp
