// kepyur radio: one RTL8812BU (wifi-sdr Receiver) + a DSP thread. Modes:
//   scan   - sweep the analog FPV bands at 40 MS/s, list pilots (humps that pass a sync check)
//   watch  - tune to one pilot and decode its video (frames to the FrameSink)
//   test   - stream at a given rate and only count banks (USB/controller soak, no DSP)
// The Receiver's sink only copies banks into a queue (wifi-sdr rule); everything else runs on the DSP thread.
// Two-antenna combining, off-centre tuning and the sparkle repair plumbing come from Play RX
// (github.com/scerstt/PlayRX, GPL-2.0), a Windows receiver built on kepyur.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "combine.h"
#include "spectrum.h"
#include "video.h"

namespace wsdr {
class Receiver;
struct RxStatus;
}

namespace kp {

struct Pilot {
  double mhz = 0;        // measured carrier
  std::string name;      // nearest standard channel ("R4"), "" if none
  std::string std_name;  // NTSC / PAL
  float snr_db = 0;
  float lock = 0;
  int64_t seen_ms = 0;   // steady-clock ms of the last scan that saw it
};

class Radio {
public:
  explicit Radio(FrameSink frames);
  ~Radio();
  Radio(const Radio &) = delete;
  Radio &operator=(const Radio &) = delete;

  // fd >= 0: the adapter behind this usbfs fd (Android); fd < 0: the first 8812BU on the bus (desktop).
  // Blocking (~5 s chip bring-up). Throws std::exception with a user-presentable message.
  void open(int fd);
  void close();
  bool is_open() const;
  bool superspeed() const;
  // The DSP thread has stopped because the receiver did (the adapter was lost, or its stream kept failing): the
  // Radio still reads as open, but nothing more will come until open() again (which clears this).
  bool dead() const;

  // Then watch prefer_hz if found (or, with nothing found, anyway), else the previous pilot again, else the
  // strongest found. prefer_hz 0: the pilot being watched.
  void scan(double prefer_hz = 0);
  void watch(double hz);
  void stream_test(int rate_sel, double seconds);
  void idle();
  // Debug: the rate watch uses (1 = 40 MS/s, the default; 2 = 20 MS/s); takes effect on the next watch().
  // Also clears a 20 MS/s fallback.
  void set_watch_rate(int rate_sel);
  // Debug: gain for the next tune: < 0 = the chip's AGC, else fixed gain in dB (CCA off, IGI pinned).
  void set_gain(double db);
  // NTSC chroma comb (DecoderOpts::comb), taken at the next watch(): fewer rainbows on fine detail and OSD
  // text, ~15 % more decode CPU. Off by default. Android: Native.setComb (the "comb" debug extra).
  void set_comb(bool on);
  // Off-centre tuning (watch only): the hardware centre on the 5 MHz grid ~6 MHz to one side of the pilot at
  // 40 MS/s, so the video never sits on the chip's DC point: its DC filter eats whatever brightness lands there
  // and rings when the picture moves off it (stripes at the left edge, ghost outlines). At 20 MS/s it changes
  // nothing: the stream is only +-10 MHz wide, the video has to stay in its middle, and the centre stays the
  // grid point nearest the pilot (at most 2.5 MHz from it). Off by default; taken at the next watch(). True when
  // it changed (call watch() again to retune now).
  bool set_offset_tuning(bool on);
  // The hardware centre off-centre tuning picks for a pilot at hz, streaming at fs.
  static double offset_centre(double hz, double fs);
  // Two-antenna video: true = paths A and B combined before demodulation (combine.h), false = A only. Takes
  // effect at once (no retune).
  void set_combine(bool on);
  struct ComboStatus {
    bool on = false;
    Combiner::Stats s;  // the last second; the time offset between the paths as of its end
  };
  ComboStatus combine_status();
  // Debug: save the next `seconds` of watched IQ as PREFIX.cs16 + PREFIX.json (kp_replay reads them), and path B
  // as PREFIX_b.cs16.
  void record(double seconds, const std::string &prefix);

  std::string status_json();   // for the UI (polled)
  std::vector<Pilot> pilots();

private:
  enum class Mode { Idle, Scan, Watch, Test };
  struct Bank {
    std::vector<uint8_t> data;
    uint64_t seq;
    uint32_t gen;
    double fs, centre_hz, shift_hz;
  };
  void dsp_loop();
  void decode_loop();
  void on_bank(const uint8_t *b, size_t n, uint64_t seq, uint32_t gen, double fs, double centre, double shift);
  // watching: off-centre tuning applies (the hardware centre recomputed for this rate)
  void tune(double hz, int rate_sel, bool watching = false);
  void handle_watch(Bank &b);
  bool watch_fault(double hz);
  bool watch_drops(double hz);
  bool no_fallback() const;
  void fall_back(double hz);
  void measure_snr(const std::vector<cf> &iq, double fs, double shift_hz);
  void record_bank(const Bank &b);
  void handle_scan(Bank &b);
  void handle_test(Bank &b);
  void scan_step_done();
  void unpack(const Bank &b, std::vector<cf> &out);
  void unpack_b(const Bank &b, std::vector<cf> &out);  // path B
  void note_faults(const wsdr::RxStatus &st);  // log each stream restart the receiver has made since the last call

  FrameSink _frames;
  std::unique_ptr<wsdr::Receiver> _rx;
  std::mutex _rxm;  // _rx itself (open and close replace it; status_json reads it from another thread)
  mutable std::mutex _m;  // mode, pilots, status fields below
  Mode _mode = Mode::Idle;
  uint64_t _mode_epoch = 0;  // bumped on every mode change; the DSP thread restarts its state
  double _watch_hz = 0, _req_hz = 0;
  int _req_rate = 1;
  int _watch_rate = 1;
  bool _fallback = false;  // 40 MS/s kept faulting during a watch: watch at 20 MS/s until open() / set_watch_rate()
  // The stream wedged while watching at 20 MS/s after a fallback: on this host 20 MS/s is worse than a 40 MS/s that
  // restarts, so no more falling back while this Radio lives (it is one for the process; open() keeps it)
  bool _no_fallback = false;
  double _gain_db = -1;
  bool _comb = false;
  bool _watch_comb = false;  // _comb as of the current watch (DSP thread only)
  bool _offset = false;      // off-centre tuning
  bool _dead = false;        // the DSP thread exited because the receiver stopped (cleared by open())
  bool _wedged = false;      // ... because the chip's C2H path wedged: only open() again recovers (RxStatus::wedged)
  // recording (DSP thread writes; the request is under _m)
  double _rec_req_s = 0;
  std::string _rec_req_prefix;
  FILE *_rec = nullptr, *_rec_b = nullptr;  // path A (PREFIX.cs16) and B (PREFIX_b.cs16)
  std::string _rec_prefix;
  uint64_t _rec_left = 0, _rec_samples = 0;
  double _rec_fs = 0, _rec_centre = 0, _rec_shift = 0;
  std::vector<Pilot> _pilots;
  bool _superspeed = false, _open = false;
  std::string _chip;

  // bank queue
  std::mutex _qm;
  std::condition_variable _qcv;
  std::deque<Bank> _q;
  std::vector<std::vector<uint8_t>> _pool;
  size_t _qcap = 512;  // 100 ms at 40 MS/s
  std::atomic<uint64_t> _banks{0}, _dropped{0};

  std::thread _dsp;
  std::atomic<bool> _stop{false};
  std::atomic<bool> _started{false};  // open() has the receiver started: only then can it be found stopped

  // decode thread: watch mode runs demod on the DSP thread and the video decoder here (two cores)
  struct DBlock {
    std::shared_ptr<FrontEnd> fe;
    std::vector<float> d;
    std::vector<uint8_t> bad;  // sparkle repair marks (empty while it is off)
    bool gap = false;          // the stream lost samples before this block (a restart, a dropped bank)
  };
  // Torn frames are not shown: after a gap in the stream the decoder's next kConcealFrames frames go nowhere, and
  // the screen keeps the last whole one (KP_CONCEAL=0: shown as before). Set on the decode thread, read by _gate.
  // At most once every kConcealEveryMs (so at most 4 frames a second hidden).
  static constexpr int kConcealFrames = 2;
  static constexpr int64_t kConcealEveryMs = 500;
  int64_t _conceal_t_ms = 0;  // decode thread only
  std::atomic<int> _conceal{0};
  std::atomic<uint64_t> _concealed{0};
  std::atomic<bool> _conceal_on{true};  // KP_CONCEAL, read at every watch
  FrameSink _gate;  // what the decoders are given: _frames, minus the frames being concealed
  std::thread _decoder;
  std::mutex _dqm;
  std::condition_variable _dqcv;
  std::deque<DBlock> _dq;
  std::vector<std::vector<float>> _dpool;
  std::vector<std::vector<uint8_t>> _bpool;  // DBlock::bad, recycled like _dpool
  std::atomic<uint64_t> _ddropped{0};
  double _dload = 0;

  // DSP-thread state
  uint64_t _epoch_seen = ~0ull;
  uint64_t _faults_seen = 0;  // the receiver's restart count as last logged
  std::shared_ptr<FrontEnd> _fe;
  uint32_t _fe_gen = ~0u;
  std::deque<int64_t> _watch_faults;  // ms of the stream restarts seen in this watch at 40 MS/s
  // the receiver's count of banks its stream dropped mid-run, read every kDropPollMs of this watch at 40 MS/s
  std::deque<std::pair<int64_t, uint64_t>> _drop_marks;  // (ms, count), the last kFallbackWindowMs of them
  int64_t _drop_poll_ms = 0;
  int64_t _watch_t0_ms = 0;  // when this watch began (its first kFallbackGraceMs do not count towards a fallback)
  uint64_t _watch_seq = 0;  // the last bank this watch took (with _fe_gen: does the next one follow on from it?)
  // live SNR of the watched carrier: a spectrum of kPsdSpan of IQ every kSnrEveryMs
  std::unique_ptr<Psd> _snr_psd;
  size_t _snr_have = 0, _snr_need = 0;
  int64_t _snr_t_ms = 0;
  float _snr_db = -1;  // < 0: none yet (under _m)
  double _fe_fs = 0, _fe_shift = 0;
  std::vector<cf> _iq, _iqb;
  bool _iqb_fresh = false;  // _iqb holds this bank's path B (unpacked only when combining or recording needs it)
  // two-antenna video
  std::atomic<bool> _combine{false};
  bool _combine_was = false;  // what the last bank did (the combiner restarts on a change)
  Combiner _combiner;
  int64_t _comb_t0_ms = 0;
  ComboStatus _comb_status;  // under _m
  // scan
  std::vector<double> _plan;
  size_t _at = 0;  // the step the scan is on
  // The scan's progress as status_json() shows it (_step: _at, _plan_n: _plan.size()). Atomic: read there while
  // the DSP thread moves it on, and set back to the start by scan() for the status to show until that thread
  // takes the new scan up.
  std::atomic<size_t> _step{0}, _plan_n{0};
  int _settle = 0;
  uint64_t _cap_last_seq = 0;
  uint32_t _cap_gen = 0;
  int _cap_restarts = 0;  // of this step's capture, after a gap
  std::vector<std::complex<int16_t>> _cap;
  size_t _cap_need = 0;
  double _cap_centre = 0;
  std::vector<Hump> _humps;  // this step's candidates (from the first kPsdSpan of the capture)  // hardware centre of the capture (humps are measured from it)
  double _resume_hz = 0;
  // test
  uint64_t _test_banks = 0, _test_gaps = 0, _test_last_seq = 0, _test_runs = 0;
  uint32_t _test_gen = 0;
  int64_t _test_t0_ms = 0;
  std::atomic<int64_t> _test_end_ms{0};  // set by stream_test() on the caller's thread
  double _test_fs = 0;
  std::string _test_result;
  // load
  double _busy_s = 0, _load = 0;
  int64_t _load_t0_ms = 0;
  uint64_t _status_ticks = 0;
  // video stats for status
  DecoderStats _vstats;
  uint64_t _frames_at_last = 0;
  int64_t _fps_t0_ms = 0;
  float _fps = 0;
};

int64_t now_ms();

}  // namespace kp
