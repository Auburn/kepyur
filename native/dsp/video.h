// kepyur: analog FPV video decoder (FM composite -> 8-bit grey frames). Shared by the app and the desktop
// tools. Grown from vtx-spike/viewer/vtxview.cpp.
//
//   FrontEnd fe(fs_in, shift_hz, on_frame);   // fs_in 40e6 (halfband to 20 MS/s) or 20e6
//   for (each IQ sample) fe.feed(x);
//
// Single-threaded: feed() and every callback run on the caller's thread.
#pragma once
#include <complex>
#include <cstdint>
#include <functional>
#include <vector>

namespace kp {

using cf = std::complex<float>;

struct VideoStd {
  double T_us;   // line period
  double a0_us;  // active video start after the hsync leading edge
  double aw_us;  // active video width
  int active;    // active lines per field
  int skip;      // lines from the field's first broad pulse to the first active line
  const char *name;
};
extern const VideoStd kNtsc, kPal;

// A finished picture, 4 bytes per pixel (R, G, B, 0xff). Valid only during the callback.
struct Frame {
  const uint8_t *data;
  int width, height;  // 720 x 480 (NTSC) / 720 x 576 (PAL)
  const VideoStd *std;
};
using FrameSink = std::function<void(const Frame &)>;

struct DecoderStats {
  bool acquiring = true;
  float sign = 1, tip = 0, blank = 0;     // levels in MHz of discriminator output
  uint64_t lines = 0, lines_locked = 0;   // since the last acquisition
  uint64_t fields = 0, frames = 0;        // since construction
  uint64_t acquisitions = 0, losses = 0;  // successful acquisitions, lock losses
  uint64_t rephases = 0;                  // line grid moved onto a jumped signal without re-acquiring
  uint64_t clicks_repaired = 0;           // sparkle repair: FM clicks patched over ...
  uint64_t lines_mended = 0;              // ... and badly hit lines redrawn from the lines above and below
  double line_us = 0;                     // tracked line period
  float lock_recent = 0;                  // locked fraction over the last ~2 fields
  bool colour = false;                    // colour is being decoded (burst present and steady)
  float burst_err = 0;                    // mean burst phase error (rad)
  const char *std_name = "?";
};

struct DecoderOpts {
  int width = 720;
  bool fields = true;          // true: every field, line-doubled (60/50 Hz, no combing); false: woven frames
  int std_force = 0;           // 0 auto, 1 NTSC, 2 PAL
  bool force_invert = false;   // only try sync at the high-frequency end
  bool colour = true;          // decode colour (falls back to grey when there is no usable burst)
  bool comb = false;           // NTSC: adaptive 3-line comb for chroma (fewer rainbows on fine detail and
                               // OSD text; ~+15 % decode CPU)
  // NTSC: how colour is kept out of the picture's brightness (PAL always uses the notch).
  //  0 = notch: 3 MHz 4th-order low-pass + a notch at the subcarrier (rings ~4 px after every edge)
  //  1 = comb: a gentle 4.8 MHz low-pass, colour cancelled with the line before (no ringing, sharper)
  // -1 = whatever set_luma_comb() says, followed live
  int luma_filter = -1;
};

class Decoder;

// ---- picture switches, for every decoder in the process. Each takes effect within a line of video, without
// rebuilding the decoder, and none of them holds the picture back (no added delay). All off by default. From
// Play RX (github.com/scerstt/PlayRX, GPL-2.0), where they were tuned on RTL8812BU adapters.

// Luma comb (NTSC): see DecoderOpts::luma_filter.
void set_luma_comb(bool on);
bool luma_comb_on();

// Adaptive comb (NTSC, with the luma comb on). The comb separates colour from brightness by comparing each line
// with the one before; where that line is clearly different (OSD text, horizons) it leaves a halo and false
// colour. There the brightness falls back to the notch filter and the colour is faded out. With the 3-line
// chroma comb (DecoderOpts::comb) the colour is only faded where that comb finds no matching line below either.
void set_adaptive_comb(bool on);
bool adaptive_comb_on();

// Sparkle repair. The white / black sparkles of a weak signal are FM "clicks": the carrier's envelope collapses
// for a moment and its phase slips a turn. The front end marks those instants (a longer dip of the envelope, as
// an echo makes at one brightness, is not one); the decoder, which draws each line about a line after it
// arrives, patches the picture over each mark (a straight line between the good points either side; sync is
// left alone) and redraws a badly hit line from the lines above and below it.
void set_repair(bool on);
bool repair_on();

// Smooth lines (DecoderOpts::fields). A field is every other line of the picture; shown with each line twice it
// has stair-stepped edges, and the two fields sit half a line apart, so the picture shimmers up and down. On:
// each field's lines go in their own place and the lines between are interpolated along edges (edge-based line
// averaging), as each field ends.
void set_smooth_lines(bool on);
bool smooth_lines_on();

// IQ (at fs_in) -> DC removal -> NCO -> [halfband /2 when fs_in >= 40e6] -> FM discriminator -> Decoder.
class FrontEnd {
public:
  FrontEnd(double fs_in, double shift_hz, FrameSink sink, const DecoderOpts &o = {});
  ~FrontEnd();
  void set_shift(double shift_hz);  // carrier offset from the stream centre (Hz) to bring to 0
  double shift() const { return _shift; }
  void feed(const cf *x, size_t n);  // demod + decode
  // The two halves, for running them on two threads: demod() -> discriminator output (MHz at fs_demod),
  // decode() -> frames. Each half is single-threaded.
  // bad (optional, sparkle repair): per output sample, 1 = the carrier collapsed there (a click). Left empty
  // while sparkle repair is off.
  void demod(const cf *x, size_t n, std::vector<float> &out, std::vector<uint8_t> *bad = nullptr);
  void decode(const float *d, size_t n, const uint8_t *bad = nullptr);
  DecoderStats stats() const;
  double fs_demod() const { return _fs_out; }

private:
  double _fs_in, _fs_out, _shift = 0;
  bool _decimate;
  static constexpr int kHb = 23;   // halfband taps
  static constexpr int kTab = 4096;  // mixer table / chunk length
  float _h[kHb];
  std::vector<float> _tr, _ti;     // step^i, i < kTab
  cf _stepN{1, 0}, _rot{1, 0}, _dc{}, _prev{1, 0};
  std::vector<float> _wr, _wi;     // mixed samples with kHb-1 of history in front
  std::vector<float> _yr, _yi;     // decimated
  int _phase = 0;                  // start of the next halfband window in the next chunk (0 or 1)
  std::vector<float> _dbuf;
  std::vector<uint8_t> _badbuf;
  Decoder *_dec;
  // sparkle repair, on demod()'s thread: each output sample's envelope (power), with the end of the last call's
  // kept in front; the discriminator output of those kept samples; the envelope's recent level
  std::vector<float> _env, _dtail;
  float _pref = 0;
};

}  // namespace kp
