// kp_gen: a synthetic analog FPV transmitter as raw IQ, for testing without an adapter. From Play RX
// (github.com/scerstt/PlayRX, tools/gen_testsignal.cpp, GPL-2.0).
//
//   kp_gen OUT [--mhz 5769] [--centre MHZ (default --mhz)] [--frames 30] [--dev 8] [--cnr 25] [--pal] [--label R4]
//
// Writes OUT.cs16 (int16 I,Q at 40 MS/s, 12-bit values << 4, like wsdr_probe) and OUT.json (sample_rate,
// centre_hz), which kp_replay and the simulated receiver (kp_sim, KP_SIM_FILE) read.
//
// Picture: 75 % colour bars (top two thirds), a grey ramp, and a white box that moves across the bottom band
// once per loop, so a frozen picture is obvious; --label puts big text over the bars. Composite levels in IRE
// (sync -40, blank 0, black 7.5 for NTSC, white 100), FM-modulated so that sync tip -> -dev/2, white -> +dev/2.
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

struct Std {
  double H_us;        // line period
  int half_lines;     // per frame (525 / 625 lines * 2)
  int eq;             // equalizing half-lines before and after the broad pulses
  int broad;          // broad-pulse half-lines
  int first_active;   // first active half-line in a field (from the field start)
  int active;         // active lines per field
  double a0_us, aw_us;  // active video start after hsync leading edge, width
  double fsc;         // colour subcarrier (Hz)
  double setup;       // black level (IRE)
  bool pal;
};
// NTSC: field = 262.5 lines. Broad pulses start at half-line 6 (line 3); kepyur's skip of 18 lines puts
// the first active line at line 21 (half-line 42). PAL: broad pulses at half-line 0, first active line 22+.
const Std kNtsc{1e6 / (4.5e6 / 286), 1050, 6, 6, 42, 240, 9.4, 52.6, 315e6 / 88, 7.5, false};
const Std kPal{64.0, 1250, 5, 5, 44, 288, 10.5, 52.0, 4433618.75, 0, true};

// RGB (0..1) -> luma and colour-difference
struct Yuv {
  double y, u, v;
};
Yuv yuv(double r, double g, double b) {
  const double y = 0.299 * r + 0.587 * g + 0.114 * b;
  return {y, 0.492 * (b - y), 0.877 * (r - y)};
}

// 5x7 glyphs for channel labels ("R4", "F2", "L8", ...); bit 4 is the left column.
const uint8_t *glyph(char c) {
  static const uint8_t digits[10][7] = {
      {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},     {14, 17, 1, 2, 4, 8, 31},
      {30, 1, 1, 14, 1, 1, 30},     {2, 6, 10, 18, 31, 2, 2},    {31, 16, 30, 1, 1, 17, 14},
      {6, 8, 16, 30, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},      {14, 17, 17, 14, 17, 17, 14},
      {14, 17, 17, 15, 1, 2, 12}};
  static const uint8_t A[7] = {14, 17, 17, 31, 17, 17, 17}, B[7] = {30, 17, 17, 30, 17, 17, 30},
                       E[7] = {31, 16, 16, 30, 16, 16, 31}, F[7] = {31, 16, 16, 30, 16, 16, 16},
                       L[7] = {16, 16, 16, 16, 16, 16, 31}, R[7] = {30, 17, 17, 30, 20, 18, 17},
                       dash[7] = {0, 0, 0, 31, 0, 0, 0}, blank[7] = {};
  if (c >= '0' && c <= '9') return digits[c - '0'];
  switch (c) {
  case 'A': return A;
  case 'B': return B;
  case 'E': return E;
  case 'F': return F;
  case 'L': return L;
  case 'R': return R;
  case '-': return dash;
  }
  return blank;
}

// Is (x, y) (0..1 inside the label box) on a lit pixel of `text`?
bool label_lit(const std::string &text, double x, double y) {
  const int n = static_cast<int>(text.size());
  const double cw = 1.0 / n;  // each char cell: 5 columns + 1 gap
  const int ci = std::min(n - 1, static_cast<int>(x / cw));
  const double cx = (x - ci * cw) / cw * 6;  // 0..6
  const int col = static_cast<int>(cx) - 0, row = static_cast<int>(y * 9) - 1;  // 1 row margin top/bottom
  if (col < 0 || col > 4 || row < 0 || row > 6) return false;
  return (glyph(text[ci])[row] >> (4 - col)) & 1;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: kp_gen OUT [--mhz F] [--centre F] [--frames N] [--dev MHZ] [--cnr DB] [--pal] [--label TEXT]\n");
    return 2;
  }
  const std::string out = argv[1];
  double vtx_mhz = 5769, centre_mhz = NAN, dev_mhz = 8, cnr_db = 25;
  int frames = 30;
  bool pal = false;
  std::string label;  // big text over the bars (a channel name), so feeds are easy to tell apart
  for (int i = 2; i < argc; i++) {
    const std::string k = argv[i];
    auto v = [&] { return i + 1 < argc ? std::atof(argv[++i]) : (std::fprintf(stderr, "missing value\n"), std::exit(2), 0.0); };
    if (k == "--mhz") vtx_mhz = v();
    else if (k == "--centre") centre_mhz = v();
    else if (k == "--frames") frames = static_cast<int>(v());
    else if (k == "--dev") dev_mhz = v();
    else if (k == "--cnr") cnr_db = v();
    else if (k == "--pal") pal = true;
    else if (k == "--label" && i + 1 < argc) label = argv[++i];
    else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
  }
  if (std::isnan(centre_mhz)) centre_mhz = vtx_mhz;  // the recording centred on its transmitter
  const Std &S = pal ? kPal : kNtsc;
  const double fs = 40e6;
  const double H = S.H_us * 1e-6, half = H / 2;
  const double frame_s = H * S.half_lines / 2;
  const size_t n = static_cast<size_t>(std::llround(frames * frame_s * fs));
  const double offset_hz = (vtx_mhz - centre_mhz) * 1e6;

  // 75 % bars: white, yellow, cyan, green, magenta, red, blue, black
  const double bars[8][3] = {{.75, .75, .75}, {.75, .75, 0}, {0, .75, .75}, {0, .75, 0},
                             {.75, 0, .75}, {.75, 0, 0}, {0, 0, .75}, {0, 0, 0}};

  FILE *f = std::fopen((out + ".cs16").c_str(), "wb");
  if (!f) { std::fprintf(stderr, "cannot write %s.cs16\n", out.c_str()); return 1; }

  const double amp = 1400;  // 12-bit full scale is 2047
  // CNR in an 18 MHz channel: noise power over the whole 40 MHz is scaled up by 40/18
  const double noise_pow = amp * amp / std::pow(10, cnr_db / 10) * (fs / 18e6);
  std::mt19937 rng(1);
  std::normal_distribution<double> nd(0, std::sqrt(noise_pow / 2));

  std::vector<int16_t> buf;
  buf.reserve(1 << 20);
  double phase = 0;
  const double w_sc = 2 * M_PI * S.fsc;
  for (size_t i = 0; i < n; i++) {
    const double t = i / fs;
    const double tf = std::fmod(t, frame_s);
    const int frame_no = static_cast<int>(t / frame_s);
    const int h = static_cast<int>(tf / half);            // half-line in frame
    const double th = (tf - h * half) * 1e6;               // us into this half-line
    const int field = h < S.half_lines / 2 ? 0 : 1;
    // half-line within the field, counted from the field's first equalizing pulse
    const int k = h - field * (S.half_lines / 2);
    // PAL fields have no leading equalizing in this simple model's numbering: shift so broad starts at 0
    const int kb = pal ? k + S.eq : k;  // NTSC: eq 0..5, broad 6..11, eq 12..17

    double ire = 0;
    const bool is_line_start = ((h % 2) == 0);
    if (kb < S.eq || (kb >= S.eq + S.broad && kb < 2 * S.eq + S.broad)) {
      ire = th < 2.3 ? -40 : 0;  // equalizing
    } else if (kb < S.eq + S.broad) {
      ire = th < S.H_us / 2 - 4.7 ? -40 : 0;  // broad (serrated)
    } else {
      // a normal line: hsync at each full-line boundary
      const double tl = is_line_start ? th : th + S.H_us / 2;  // us since the line's hsync
      const int line_h = is_line_start ? k : k - 1;           // half-line where this line started
      if (tl < 4.7) {
        ire = -40;
      } else if (tl >= 5.3 && tl < 5.3 + 2.25) {
        // colour burst, 20 IRE along -U (NTSC); PAL swings +-135 deg line by line
        const double ph = pal ? ((line_h / 2) % 2 ? 0.75 * M_PI : 1.25 * M_PI) : M_PI;
        ire = 20 * std::sin(w_sc * t + ph);
      } else if (tl >= S.a0_us && tl < S.a0_us + S.aw_us && line_h >= S.first_active &&
                 line_h < S.first_active + 2 * S.active) {
        const int row = ((line_h - S.first_active) / 2) * 2 + field;  // 0 .. 2*active-1
        const double x = (tl - S.a0_us) / S.aw_us;                    // 0..1 across
        const double rows = 2.0 * S.active;
        double r, g, b;
        // label box: x 0.3..0.7, rows 0.12..0.52 of the picture
        const double ly = (row / rows - 0.12) / 0.40, lx = (x - 0.3) / 0.4;
        if (!label.empty() && lx >= 0 && lx < 1 && ly >= 0 && ly < 1) {
          r = g = b = label_lit(label, lx, ly) ? 1.0 : 0.0;
        } else if (row < rows * 0.66) {
          const int bi = std::min(7, static_cast<int>(x * 7));
          r = bars[bi][0], g = bars[bi][1], b = bars[bi][2];
        } else if (row < rows * 0.80) {
          r = g = b = x;  // grey ramp
        } else {
          const double bx = std::fmod(frame_no, frames) / static_cast<double>(frames);
          const bool box = x >= bx * 0.85 && x < bx * 0.85 + 0.15;
          r = g = b = box ? 1.0 : 0.1;
        }
        const Yuv c = yuv(r, g, b);
        const double sat = 100 - S.setup;
        double chroma;
        if (pal) {
          // V switch: the lines whose burst is at 135 degrees (-U + V, above) carry V as it is, the others
          // (225 degrees) carry it inverted
          const double vs = ((line_h / 2) % 2) ? 1 : -1;
          chroma = c.u * std::sin(w_sc * t) + vs * c.v * std::cos(w_sc * t);
        } else {
          chroma = c.u * std::sin(w_sc * t) + c.v * std::cos(w_sc * t);
        }
        ire = S.setup + sat * c.y + sat * chroma;
      } else {
        ire = 0;
      }
    }
    // FM: -40 IRE (sync tip) -> -dev/2, 100 IRE (white) -> +dev/2
    const double fi = offset_hz + dev_mhz * 1e6 * ((ire + 40) / 140.0 - 0.5);
    phase += 2 * M_PI * fi / fs;
    if (phase > M_PI) phase -= 2 * M_PI * std::floor((phase + M_PI) / (2 * M_PI));
    const double re = amp * std::cos(phase) + nd(rng), im = amp * std::sin(phase) + nd(rng);
    auto q = [](double v) { return static_cast<int16_t>(std::lround(std::max(-2047.0, std::min(2047.0, v))) * 16); };
    buf.push_back(q(re));
    buf.push_back(q(im));
    if (buf.size() >= (1 << 20)) {
      std::fwrite(buf.data(), 2, buf.size(), f);
      buf.clear();
    }
  }
  std::fwrite(buf.data(), 2, buf.size(), f);
  std::fclose(f);
  if (FILE *j = std::fopen((out + ".json").c_str(), "w")) {
    std::fprintf(j, "{\"sample_rate\": %.0f, \"centre_hz\": %.0f, \"vtx_hz\": %.0f, \"std\": \"%s\", \"synthetic\": true}\n",
                 fs, centre_mhz * 1e6, vtx_mhz * 1e6, pal ? "PAL" : "NTSC");
    std::fclose(j);
  }
  std::fprintf(stderr, "wrote %s.cs16: %zu samples (%.3f s, %d %s frames), VTX %.1f MHz, centre %.1f MHz, CNR %.0f dB\n",
               out.c_str(), n, n / fs, frames, pal ? "PAL" : "NTSC", vtx_mhz, centre_mhz, cnr_db);
  return 0;
}
