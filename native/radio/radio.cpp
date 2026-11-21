#include "radio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "log.h"
#include "receiver.h"
#include "spectrum.h"
#include "stream88xx.h"

#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

namespace kp {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace {
constexpr double kFs = 40e6;
constexpr double kEdge = 16.5e6;     // usable +- span at 40 MS/s (BW40 DFIR is flat to ~17 MHz)
constexpr double kStep = 20e6;       // a hump up to ~13 MHz wide is fully inside some step's span
constexpr double kCapture = 0.09;    // s of IQ per scan step (60 ms acquisition + ~300 lines)
constexpr double kPsdSpan = 0.012;   // s of it for the spectrum
constexpr int kSettleBanks = 40;     // ~8 ms after a retune (chip AGC)
constexpr int kCapRestarts = 8;      // times a scan step's capture starts over after a gap; then gaps stay in
// A watch at 40 MS/s that restarts this often falls back to 20 MS/s (same picture: the BW20 RX filter is
// +-10 MHz either way). A link that cannot carry 40 MS/s restarts every 0.2-0.3 s (Chrome's WebUSB: ~14k
// transfers/s against the 24.4k needed); a healthy one runs minutes without one.
constexpr int kFallbackFaults = 3;
constexpr int64_t kFallbackWindowMs = 5000;
// So does one whose stream keeps dropping single banks without restarting (collected late or overrun: wifi-sdr
// RxStatus::mid_drops). At 40 MS/s a bank is 205 us, ~4900 of them a second, and each one lost takes 3 lines out
// of the picture. A host that stalls now and then loses one to three a minute (Play RX's figure for Windows), at
// most four at one go (a fifth close behind ends the run): left alone. kFallbackDrops in the window is two a
// second kept up; before single banks could be dropped each of them was a restart, and three of those in the
// window were enough.
constexpr int kFallbackDrops = 10;
constexpr int64_t kDropPollMs = 200;  // how often the receiver's count is read
// Neither counts in the first seconds of a watch. On some hosts a fresh stream starts badly for a while: a Pixel 8 Pro
// loses the run at bank 51-55 (where the first of the 256 posted reads come round again) for one to four seconds,
// a restart each time, before it keeps up for good. Counted, three of those used to drop the watch to 20 MS/s within
// a second, and on that phone 20 MS/s is far worse (its USB path wedges within ~0.2 s, where 40 MS/s runs for many
// seconds). A link that cannot carry 40 MS/s goes on restarting past the grace, and still falls back.
constexpr int64_t kFallbackGraceMs = 10000;
constexpr int64_t kSnrEveryMs = 500;  // live SNR while watching (~2 % of a core at 20 MS/s)
constexpr double kSnrSpan = 8e6;      // +- span the BW20 RX filter passes flat (-3 dB at ~9.2 MHz)
// Off-centre tuning at 40 MS/s: how far beside the pilot the hardware centre goes (Radio::offset_centre). The
// video swings about +-2.5 MHz around its carrier; this keeps that clear of the DC point and inside the flat band.
constexpr double kOffsetAim40 = 6e6;

// Raise this thread's priority (the USB stream thread must never miss a chip deadline; the DSP thread
// must keep up). Best effort: logs what the OS allowed.
void boost_thread(const char *what) {
  const int tid = static_cast<int>(gettid());
  const int rc = setpriority(PRIO_PROCESS, tid, -10);
  KP_LOGI("radio: %s thread %d nice -10: %s", what, tid, rc == 0 ? "ok" : "refused");
}

// The cores a processing thread is kept on: all but the slowest kind, on a chip with faster ones (each core's
// cpu_capacity, else its top clock), so long as at least two are left. Empty: no such split, or none readable.
std::vector<int> fast_cores() {
  const auto read = [](int c, const char *leaf) {
    char path[96];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/%s", c, leaf);
    long v = -1;
    if (FILE *f = std::fopen(path, "r")) {
      if (std::fscanf(f, "%ld", &v) != 1) v = -1;
      std::fclose(f);
    }
    return v;
  };
  const int n = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
  std::vector<long> cap(std::max(n, 0));
  for (const char *leaf : {"cpu_capacity", "cpufreq/cpuinfo_max_freq"}) {
    bool all = n > 0;
    for (int c = 0; c < n && all; c++) all = (cap[c] = read(c, leaf)) > 0;
    if (all) break;
    cap.assign(cap.size(), -1);
  }
  if (cap.empty() || cap[0] <= 0) return {};
  const long slowest = *std::min_element(cap.begin(), cap.end());
  std::vector<int> out;
  for (int c = 0; c < n; c++)
    if (cap[c] > slowest) out.push_back(c);
  if (out.size() < 2) out.clear();
  return out;
}

// Keep this processing thread off the slowest cores (fast_cores). Left to itself, Android now and then runs a
// thread that is busy half the time on a little core (a Pixel 8 Pro's Cortex-A510s, about a third the speed of
// its A715s): the decode thread was there 7 % of its time, falling behind each time, and its queue overflowed in
// bursts with the thread at half load. KP_FAST_CPUS=0 (the "env" debug extra): left to the OS, as before.
void keep_off_slow_cores(const char *what) {
#if defined(__linux__)
  if (const char *e = std::getenv("KP_FAST_CPUS"); e && *e == '0') {
    KP_LOGI("radio: %s thread left to the OS (KP_FAST_CPUS=0)", what);
    return;
  }
  static const std::vector<int> cores = fast_cores();
  if (cores.empty()) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  std::string list;
  for (int c : cores) {
    CPU_SET(c, &set);
    list += (list.empty() ? "" : ",") + std::to_string(c);
  }
  const int rc = sched_setaffinity(0, sizeof set, &set);
  KP_LOGI("radio: %s thread on cores %s: %s", what, list.c_str(), rc == 0 ? "ok" : std::strerror(errno));
#else
  (void)what;
#endif
}

std::vector<double> scan_plan() {
  std::vector<double> p;
  // the common analog bands first (A, B, E, F, R: 5645..5945), then the low band (L: 5362..5621)
  for (double c = 5652e6; c <= 5940e6; c += kStep) p.push_back(c);
  for (double c = 5370e6; c <= 5630e6; c += kStep) p.push_back(c);
  return p;
}
}  // namespace

Radio::Radio(FrameSink frames) : _frames(std::move(frames)) {
  _gate = [this](const Frame &f) {
    if (_conceal.load(std::memory_order_relaxed) > 0) {
      _conceal--;
      _concealed++;
      return;
    }
    _frames(f);
  };
}

Radio::~Radio() { close(); }

bool Radio::is_open() const {
  std::lock_guard<std::mutex> l(_m);
  return _open;
}

bool Radio::superspeed() const {
  std::lock_guard<std::mutex> l(_m);
  return _superspeed;
}

bool Radio::dead() const {
  std::lock_guard<std::mutex> l(_m);
  return _dead;
}

void Radio::open(int fd) {
  close();
  auto log = std::make_shared<Logger>();
  auto rx = std::make_unique<wsdr::Receiver>(log);
  wsdr::RxSettings s;
  s.freq = 5769e6;
  s.rate_sel = 1;  // 40 MS/s
  s.wide = true;
  s.agc = true;
  KP_LOGI("radio: opening adapter (%s)", fd >= 0 ? "Android fd" : "bus scan");
  rx->open(wsdr::kPid8812bu, s, -1, {}, fd);
  const auto caps = rx->caps();
  KP_LOGI("radio: %s, USB %s, fastest continuous rate %.1f MS/s", caps.chip.c_str(), caps.superspeed ? "3" : "2",
          wsdr::Receiver::rate_of(caps.min_rate_sel) / 1e6);
  if (!caps.superspeed) {
    // 40 MS/s does not stream on USB 2; open at a rate it can, so the stream still comes up for diagnostics
    s.rate_sel = caps.min_rate_sel;
    rx->set(s);
  }
  {
    std::lock_guard<std::mutex> l(_m);
    _superspeed = caps.superspeed;
    _chip = caps.chip;
    _open = true;
    _mode = Mode::Idle;
    _mode_epoch++;
    _req_hz = s.freq;
    _req_rate = s.rate_sel;
    _fallback = false;
    _dead = false;
    _wedged = false;
  }
  _banks = 0;
  _dropped = 0;
  _stop = false;
  {
    std::lock_guard<std::mutex> l(_rxm);
    _rx = std::move(rx);
  }
  _ddropped = 0;
  _conceal = 0;
  _concealed = 0;
  _faults_seen = 0;
  _started = false;
  _dsp = std::thread([this] { dsp_loop(); });
  _decoder = std::thread([this] { decode_loop(); });
  _rx->start([this](const uint8_t *b, size_t n, const wsdr::BankMeta &m) {
    on_bank(b, n, m.seq, m.gen, m.fs, m.centre_hz, m.shift_hz);
  });
  _started = true;
}

void Radio::close() {
  _stop = true;
  _qcv.notify_all();
  if (_rx) _rx->stop();
  if (_dsp.joinable()) _dsp.join();
  _dqcv.notify_all();
  if (_decoder.joinable()) _decoder.join();
  {
    std::lock_guard<std::mutex> l(_dqm);
    _dq.clear();
  }
  _fe.reset();
  // a recording cut short by the close: what was written stays (no .json: kp_replay then needs --fs)
  if (_rec) std::fclose(_rec), _rec = nullptr;
  if (_rec_b) std::fclose(_rec_b), _rec_b = nullptr;
  // out of status_json()'s sight first, then closed: closing can take a while, and a status poll (the app's
  // main thread) must not wait for it
  std::unique_ptr<wsdr::Receiver> rx;
  {
    std::lock_guard<std::mutex> l(_rxm);
    rx = std::move(_rx);
  }
  if (rx) {
    rx->close();
    rx.reset();
    KP_LOGI("radio: closed");
  }
  {
    std::lock_guard<std::mutex> l(_m);
    _open = false;
    _mode = Mode::Idle;
  }
  std::lock_guard<std::mutex> q(_qm);
  _q.clear();
}

// Receiver stream thread: copy only.
void Radio::on_bank(const uint8_t *b, size_t n, uint64_t seq, uint32_t gen, double fs, double centre, double shift) {
  thread_local bool boosted = false;
  if (!boosted) {
    boosted = true;
    boost_thread("usb stream");
  }
  std::vector<uint8_t> v;
  {
    std::lock_guard<std::mutex> l(_qm);
    if (!_pool.empty()) {
      v = std::move(_pool.back());
      _pool.pop_back();
    }
  }
  v.assign(b, b + n);
  {
    std::lock_guard<std::mutex> l(_qm);
    if (_q.size() >= _qcap) {
      _pool.push_back(std::move(_q.front().data));
      _q.pop_front();
      _dropped++;
    }
    _q.push_back(Bank{std::move(v), seq, gen, fs, centre, shift});
  }
  _banks++;
  _qcv.notify_one();
}

// The hardware centre on the 5 MHz grid (where the chip's 40 MHz channels put it). At 40 MS/s: the one whose
// distance from the pilot is nearest the aim, either side.
// At 20 MS/s: the one nearest the pilot, which is where the watch is without off-centre tuning. The stream ends
// 10 MHz from its centre, and a signal ~18 MHz wide loses one side as soon as it sits off the middle: up to 2 MHz
// out the picture is the centred one, at 2.5 to 3 MHz PAL already suffers, at 4 to 5 MHz bright colours wash out
// (measured with the --as20 filter of kp_replay). The grid offers the pilot one centre at most 2.5 MHz away and
// the next one at least 2.5 MHz away on the other side, so the near one it is; a pilot 2 MHz off the grid has the
// DC point at the edge of its video's swing that way, one on the grid has to live with it in the middle.
double Radio::offset_centre(double hz, double fs) {
  const double c0 = std::round(hz / 5e6) * 5e6;
  if (fs < 40e6) return c0;
  double best = c0, err = 1e18;
  for (int k = -3; k <= 3; k++) {
    const double c = c0 + k * 5e6, e = std::fabs(std::fabs(hz - c) - kOffsetAim40);
    if (e < err) err = e, best = c;
  }
  return best;
}

bool Radio::set_offset_tuning(bool on) {
  std::lock_guard<std::mutex> l(_m);
  if (_offset == on) return false;
  _offset = on;
  return true;
}

void Radio::tune(double hz, int rate_sel, bool watching) {
  wsdr::RxSettings s = _rx->settings();
  s.freq = hz;
  {
    std::lock_guard<std::mutex> l(_m);
    // recomputed on every watching tune, so a 20 MS/s fallback gets the centre for its own rate
    s.centre = watching && _offset ? offset_centre(hz, wsdr::Receiver::rate_of(rate_sel)) : 0;
  }
  if (s.centre > 0)
    KP_LOGI("radio: hardware centre %.0f MHz (pilot %+.1f MHz beside it)", s.centre / 1e6, (hz - s.centre) / 1e6);
  s.rate_sel = rate_sel;
  s.wide = true;
  {
    std::lock_guard<std::mutex> l(_m);
    s.agc = _gain_db < 0;
    if (_gain_db >= 0) s.gain_db = _gain_db;
  }
  _rx->set(s);
  std::lock_guard<std::mutex> l(_m);
  _req_hz = hz;
  _req_rate = rate_sel;
}

// The four mode calls do nothing on a radio that is not open, and that is decided under the lock open() and
// close() set _open under: a call that races a close can no longer leave the closed radio in its mode.
void Radio::scan(double prefer_hz) {
  std::lock_guard<std::mutex> l(_m);
  if (!_open) return;
  _resume_hz = prefer_hz > 0 ? prefer_hz : _mode == Mode::Watch ? _watch_hz : 0;
  _mode = Mode::Scan;
  _mode_epoch++;
  // the status says "scan" from here on: its progress with it, not what the scan before left (the DSP thread
  // sets both again when it takes the scan up, and moves the step on only for the scan it is working on)
  _step = 0;
  _plan_n = scan_plan().size();
}

void Radio::watch(double hz) {
  std::lock_guard<std::mutex> l(_m);
  if (!_open) return;
  _watch_hz = hz;
  _mode = Mode::Watch;
  _mode_epoch++;
}

void Radio::stream_test(int rate_sel, double seconds) {
  std::lock_guard<std::mutex> l(_m);
  if (!_open) return;
  _mode = Mode::Test;
  _mode_epoch++;
  _req_rate = rate_sel;
  _test_end_ms = static_cast<int64_t>(seconds * 1000);
}

void Radio::idle() {
  std::lock_guard<std::mutex> l(_m);
  if (!_open) return;
  _mode = Mode::Idle;
  _mode_epoch++;
}

void Radio::set_watch_rate(int rate_sel) {
  std::lock_guard<std::mutex> l(_m);
  _watch_rate = rate_sel;
  _fallback = false;
}

void Radio::set_gain(double db) {
  std::lock_guard<std::mutex> l(_m);
  _gain_db = db;
}

void Radio::set_comb(bool on) {
  std::lock_guard<std::mutex> l(_m);
  _comb = on;
}

void Radio::set_combine(bool on) { _combine = on; }

Radio::ComboStatus Radio::combine_status() {
  std::lock_guard<std::mutex> l(_m);
  return _comb_status;
}

void Radio::record(double seconds, const std::string &prefix) {
  std::lock_guard<std::mutex> l(_m);
  _rec_req_s = seconds;
  _rec_req_prefix = prefix;
}

std::vector<Pilot> Radio::pilots() {
  std::lock_guard<std::mutex> l(_m);
  return _pilots;
}

void Radio::unpack(const Bank &b, std::vector<cf> &out) {
  const size_t ns = b.data.size() / 8;
  out.resize(ns);
  const uint8_t *p = b.data.data();
  for (size_t i = 0; i < ns; i++) {
    uint64_t w;
    std::memcpy(&w, p + 8 * i, 8);
    out[i] = cf(static_cast<float>(wsdr::la_i(w)), static_cast<float>(wsdr::la_q(w)));
  }
}

void Radio::unpack_b(const Bank &b, std::vector<cf> &out) {
  const size_t ns = b.data.size() / 8;
  out.resize(ns);
  const uint8_t *p = b.data.data();
  for (size_t i = 0; i < ns; i++) {
    uint64_t w;
    std::memcpy(&w, p + 8 * i, 8);
    // path B: I [35:24], Q [47:36] (kDmaBothPaths8822b), sign-extended like la_i / la_q
    out[i] = cf(static_cast<float>(wsdr::la_i(w >> 24)), static_cast<float>(wsdr::la_q(w >> 24)));
  }
  _iqb_fresh = true;
}

void Radio::dsp_loop() {
  boost_thread("dsp");
  keep_off_slow_cores("dsp");
  Bank b;
  _load_t0_ms = now_ms();
  while (!_stop) {
    bool none;
    {
      std::unique_lock<std::mutex> l(_qm);
      _qcv.wait_for(l, std::chrono::milliseconds(200), [&] { return !_q.empty() || _stop; });
      if (_stop) break;  // close(): stops the receiver too, which is not a death
      none = _q.empty();
      if (!none) {
        b = std::move(_q.front());
        _q.pop_front();
      }
    }
    if (none) {
      // Outside the queue's lock: _m is never taken while holding it. And only once open() has the receiver
      // started: this thread runs before that call, and a receiver not yet running is not one that stopped.
      if (_started && _rx) {
        const wsdr::RxStatus st = _rx->status();
        if (_stop) break;  // close() got in between: it stopped the receiver itself
        note_faults(st);
        if (!st.running) {
          KP_LOGE("radio: receiver stopped%s: %s", st.wedged ? " (C2H wedged)" : "", st.error.c_str());
          // nothing restarts it from here: say so, so the app can reopen the adapter (at once, if it wedged)
          std::lock_guard<std::mutex> m(_m);
          _dead = true;
          _wedged = st.wedged;
          if (st.wedged && _fallback && _mode == Mode::Watch && !_no_fallback) {
            _no_fallback = true;
            KP_LOGW("radio: wedged at 20 MS/s after falling back from 40: no more falling back this session");
          }
          break;
        }
      }
      continue;
    }
    const auto t0 = std::chrono::steady_clock::now();
    Mode mode;
    uint64_t epoch;
    {
      std::lock_guard<std::mutex> l(_m);
      mode = _mode;
      epoch = _mode_epoch;
    }
    const bool fresh = epoch != _epoch_seen;
    _epoch_seen = epoch;
    if (fresh) {
      // a mode change: set the radio up for it (a new watch builds a new decoder)
      _fe.reset();
      _fe_gen = ~0u;
      if (mode == Mode::Watch) {
        double hz;
        {
          std::lock_guard<std::mutex> l(_m);
          hz = _watch_hz;
        }
        // Watch at 40 MS/s with the chip's AGC, or 20 MS/s after a fallback (watch_fault). Both on a 40 MHz
        // channel: at 20 MHz width the OFDM CCA/AGC froze the LA in 120-sample runs (wifi-sdr RxSettings::wide).
        int rate;
        {
          std::lock_guard<std::mutex> l(_m);
          rate = _fallback && _watch_rate == 1 ? 2 : _watch_rate;
          _watch_comb = _comb;  // a decoder rebuilt later in this watch (fallback, shift) keeps it
        }
        KP_LOGI("radio: watch %.3f MHz at %.0f MS/s", hz / 1e6, wsdr::Receiver::rate_of(rate) / 1e6);
        {
          const char *e = std::getenv("KP_CONCEAL");
          _conceal_on = !(e && *e == '0');
        }
        _watch_faults.clear();
        _drop_marks.clear();
        _watch_t0_ms = now_ms();
        _snr_psd.reset();
        _snr_t_ms = 0;
        {
          std::lock_guard<std::mutex> l(_m);
          _snr_db = -1;
        }
        tune(hz, rate, true);
      } else if (mode == Mode::Scan) {
        _plan = scan_plan();
        _plan_n = _plan.size();
        _at = 0;
        _step = 0;
        _cap.clear();
        _settle = -1;
        KP_LOGI("radio: scan, %zu steps", _plan.size());
        tune(_plan[0], 1);
      } else if (mode == Mode::Test) {
        int rate;
        {
          std::lock_guard<std::mutex> l(_m);
          rate = _req_rate;
          _test_result.clear();
        }
        _test_banks = _test_gaps = 0;
        _test_last_seq = 0;
        _test_t0_ms = 0;
        _test_fs = wsdr::Receiver::rate_of(rate);
        KP_LOGI("radio: stream test at %.1f MS/s", _test_fs / 1e6);
        tune(5769e6, rate);
      }
    }
    switch (mode) {
    case Mode::Watch: handle_watch(b); break;
    case Mode::Scan: handle_scan(b); break;
    case Mode::Test: handle_test(b); break;
    case Mode::Idle: break;
    }
    {
      std::lock_guard<std::mutex> l(_qm);
      if (_pool.size() < _qcap) _pool.push_back(std::move(b.data));
    }
    _busy_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const int64_t t = now_ms();
    if (t - _load_t0_ms >= 1000 && _rx) note_faults(_rx->status());
    if (t - _load_t0_ms >= 1000) {
      std::lock_guard<std::mutex> l(_m);
      _load = _busy_s / ((t - _load_t0_ms) / 1000.0);
      _busy_s = 0;
      _load_t0_ms = t;
      if (++_status_ticks % 5 == 0 && _mode == Mode::Watch)
        KP_LOGI("status: watch %.1f MHz, cpu demod %.0f%% decode %.0f%%, dropped stream %llu usb %llu decode %llu, "
                "restarts %llu, %s %s lock %.2f fps %.0f, acq %llu lost %llu rephase %llu, torn frames hidden %llu",
                _watch_hz / 1e6,
                _load * 100, _dload * 100, (unsigned long long)(_rx ? _rx->status().mid_drops : 0),
                (unsigned long long)_dropped.load(), (unsigned long long)_ddropped.load(),
                (unsigned long long)(_rx ? _rx->status().restarts : 0), _vstats.std_name,
                _vstats.colour ? "colour" : "grey", _vstats.lock_recent, _fps,
                (unsigned long long)_vstats.acquisitions, (unsigned long long)_vstats.losses,
                (unsigned long long)_vstats.rephases, (unsigned long long)_concealed.load());
    }
  }
}

void Radio::note_faults(const wsdr::RxStatus &st) {
  if (st.restarts == _faults_seen) return;
  _faults_seen = st.restarts;
  KP_LOGW("radio: stream fault %llu (%llu banks so far, %u repeated starts): %s",
          static_cast<unsigned long long>(st.restarts), static_cast<unsigned long long>(st.banks), st.start_retries,
          st.last_fault.c_str());
}

// A stream restart during a watch. At 40 MS/s, kFallbackFaults of them within kFallbackWindowMs switch the
// watch to 20 MS/s for the rest of the session; true when it did (the bank in hand is from the old stream).
// Not counted in the first kFallbackGraceMs of the watch, nor once 20 MS/s has wedged here (_no_fallback).
bool Radio::watch_fault(double hz) {
  if (_fe_fs != 40e6 || no_fallback()) return false;
  const int64_t t = now_ms();
  if (t - _watch_t0_ms < kFallbackGraceMs) return false;
  _watch_faults.push_back(t);
  while (!_watch_faults.empty() && t - _watch_faults.front() > kFallbackWindowMs) _watch_faults.pop_front();
  if (static_cast<int>(_watch_faults.size()) < kFallbackFaults) return false;
  KP_LOGW("radio: 40 MS/s restarted %zu times in %lld s: watching at 20 MS/s from now on", _watch_faults.size(),
          static_cast<long long>(kFallbackWindowMs / 1000));
  fall_back(hz);
  return true;
}

// The same for banks the stream dropped without restarting: kFallbackDrops of them within kFallbackWindowMs of
// a watch at 40 MS/s. The receiver counts them as they happen; that count is read every kDropPollMs. Those dropped
// in the first kFallbackGraceMs are not counted: the window starts when the grace ends.
bool Radio::watch_drops(double hz) {
  if (_fe_fs != 40e6 || !_rx || no_fallback()) return false;
  const int64_t t = now_ms();
  if (t - _drop_poll_ms < kDropPollMs) return false;
  _drop_poll_ms = t;
  if (t - _watch_t0_ms < kFallbackGraceMs) {
    _drop_marks.clear();
    return false;
  }
  const uint64_t count = _rx->status().mid_drops;
  _drop_marks.emplace_back(t, count);
  while (t - _drop_marks.front().first > kFallbackWindowMs) _drop_marks.pop_front();
  const uint64_t lost = count - _drop_marks.front().second;
  if (lost < static_cast<uint64_t>(kFallbackDrops)) return false;
  KP_LOGW("radio: 40 MS/s lost %llu blocks in %.1f s: watching at 20 MS/s from now on",
          static_cast<unsigned long long>(lost), (t - _drop_marks.front().first) / 1000.0);
  fall_back(hz);
  return true;
}

bool Radio::no_fallback() const {
  std::lock_guard<std::mutex> l(_m);
  return _no_fallback;
}

// The watch does not hold up at 40 MS/s: at 20 MS/s until open() or set_watch_rate().
void Radio::fall_back(double hz) {
  {
    std::lock_guard<std::mutex> l(_m);
    _fallback = true;
  }
  _watch_faults.clear();
  _drop_marks.clear();
  _fe.reset();
  _fe_gen = ~0u;
  tune(hz, 2, true);  // as a watch: off-centre tuning picks the centre for 20 MS/s
}

void Radio::measure_snr(const std::vector<cf> &iq, double fs, double shift_hz) {
  if (!_snr_psd) {
    if (now_ms() - _snr_t_ms < kSnrEveryMs) return;
    _snr_psd = std::make_unique<Psd>(1024);
    _snr_have = 0;
    _snr_need = static_cast<size_t>(kPsdSpan * fs);
  }
  _snr_psd->add(iq.data(), iq.size());
  _snr_have += iq.size();
  if (_snr_have < _snr_need) return;
  // Around the carrier (at shift_hz: beside the centre with off-centre tuning), within the band the RX filter
  // passes: the BW40 DFIR's +-kEdge at 40 MS/s, the BW20 set's +-kSnrSpan at 20 MS/s. With the carrier at the
  // centre this is the +-kSnrSpan it always was.
  const double pass = fs >= 40e6 ? kEdge : std::min(kSnrSpan, 0.4 * fs);
  const float snr = carrier_snr(_snr_psd->db(), fs, std::min(kSnrSpan, 0.4 * fs), shift_hz, pass);
  _snr_psd.reset();
  _snr_t_ms = now_ms();
  std::lock_guard<std::mutex> l(_m);
  _snr_db = snr;
}

void Radio::handle_watch(Bank &b) {
  double req;
  int req_rate;
  {
    std::lock_guard<std::mutex> l(_m);
    req = _req_hz;
    req_rate = _req_rate;
  }
  // ignore banks from before the retune (or the rate change of a fallback)
  if (std::fabs(b.centre_hz + b.shift_hz - req) > 1e3 || b.fs != wsdr::Receiver::rate_of(req_rate)) return;
  if (!_fe || b.fs != _fe_fs || b.shift_hz != _fe_shift) {
    DecoderOpts o;
    o.comb = _watch_comb;
    _fe = std::make_shared<FrontEnd>(b.fs, b.shift_hz, _gate, o);
    _fe_fs = b.fs;
    _fe_shift = b.shift_hz;
    // a new carrier or rate: no weights or noise estimates from the last one, and the paths' time offset is
    // measured again (it is a number of samples, so it depends on the rate)
    _combiner.reset(b.fs);
  } else if (b.gen != _fe_gen) {
    // same settings: keep the decoder; it re-phases onto the restarted stream within a few lines
    std::string why;
    if (_rx) why = _rx->status().last_fault;
    KP_LOGW("radio: stream restarted (gen %u, %s), decoder keeps going", b.gen, why.c_str());
    if (watch_fault(req)) return;
  }
  if (watch_drops(req)) return;
  // After a restart, or banks lost on the way here, this bank does not follow on from the last one: the
  // combiner must not delay path B with samples from before the hole, and the frames around it are torn.
  const bool gap = b.gen != _fe_gen || b.seq != _watch_seq + 1;
  if (gap) _combiner.cut();
  _fe_gen = b.gen;
  _watch_seq = b.seq;
  _iqb_fresh = false;
  unpack(b, _iq);
  measure_snr(_iq, b.fs, b.shift_hz);  // path A, as without combining
  record_bank(b);  // both paths as they came in
  // two-antenna video: B lined up and turned into step with A and both weighted by their SNR, in place of A
  const bool comb = _combine.load();
  if (comb != _combine_was) {
    _combiner.reset(b.fs);
    _combine_was = comb;
    KP_LOGI("radio: video antennas %s", comb ? "A + B combined" : "A only");
  }
  if (comb) {
    if (!_iqb_fresh) unpack_b(b, _iqb);
    _combiner.process(_iq.data(), _iqb.data(), std::min(_iq.size(), _iqb.size()));
  }
  const int64_t tnow = now_ms();
  if (tnow - _comb_t0_ms >= 1000) {
    _comb_t0_ms = tnow;
    ComboStatus cs;
    cs.on = comb;
    if (comb) cs.s = _combiner.take_stats();
    bool news;
    {
      std::lock_guard<std::mutex> l(_m);
      news = comb && (!_comb_status.on || cs.s.lag != _comb_status.s.lag ||
                      cs.s.lag_measured != _comb_status.s.lag_measured);
      _comb_status = cs;
    }
    if (news)
      KP_LOGI("radio: antenna B taken as %d samples (%.0f ns) ahead of A, %s", cs.s.lag, cs.s.lag / b.fs * 1e9,
              cs.s.lag_measured ? "measured" : "the adapter's usual figure");
  }
  std::vector<float> d;
  std::vector<uint8_t> bad;
  {
    std::lock_guard<std::mutex> l(_dqm);
    if (!_dpool.empty()) {
      d = std::move(_dpool.back());
      _dpool.pop_back();
    }
    if (!_bpool.empty()) {
      bad = std::move(_bpool.back());
      _bpool.pop_back();
    }
  }
  _fe->demod(_iq.data(), _iq.size(), d, &bad);  // bad stays empty while sparkle repair is off
  {
    std::lock_guard<std::mutex> l(_dqm);
    if (_dq.size() >= 128) {  // ~26 ms behind: drop the oldest (the decoder re-phases)
      // Not a gap for concealment: a decoder that falls behind drops blocks many times a second (a hot phone, a
      // weak signal), and hiding a frame for each would freeze the picture. A stream gap it carried moves on.
      const bool carried = _dq.front().gap;
      _dpool.push_back(std::move(_dq.front().d));
      _bpool.push_back(std::move(_dq.front().bad));
      _dq.pop_front();
      _ddropped++;
      if (carried && !_dq.empty()) _dq.front().gap = true;
    }
    _dq.push_back(DBlock{_fe, std::move(d), std::move(bad), gap});
  }
  _dqcv.notify_one();
}

// Debug recording: the watched IQ as int16 (LA value << 4, as wsdr_probe writes .cs16), whole banks.
void Radio::record_bank(const Bank &b) {
  if (!_rec) {
    double secs;
    std::string prefix;
    {
      std::lock_guard<std::mutex> l(_m);
      secs = _rec_req_s;
      prefix = _rec_req_prefix;
      _rec_req_s = 0;
    }
    if (secs <= 0) return;
    _rec = std::fopen((prefix + ".cs16").c_str(), "wb");
    if (!_rec) {
      KP_LOGE("record: cannot open %s.cs16", prefix.c_str());
      return;
    }
    std::setvbuf(_rec, nullptr, _IOFBF, 1 << 20);
    _rec_b = std::fopen((prefix + "_b.cs16").c_str(), "wb");
    if (_rec_b) std::setvbuf(_rec_b, nullptr, _IOFBF, 1 << 20);
    _rec_prefix = prefix;
    _rec_left = static_cast<uint64_t>(secs * b.fs);
    _rec_samples = 0;
    _rec_fs = b.fs;
    _rec_centre = b.centre_hz;
    _rec_shift = b.shift_hz;
    KP_LOGI("record: %.1f s at %.0f MS/s -> %s.cs16", secs, b.fs / 1e6, prefix.c_str());
  }
  const size_t ns = std::min<uint64_t>(_iq.size(), _rec_left);
  std::vector<int16_t> buf(2 * ns);
  for (size_t i = 0; i < ns; i++) {
    buf[2 * i] = static_cast<int16_t>(static_cast<int>(_iq[i].real()) * 16);
    buf[2 * i + 1] = static_cast<int16_t>(static_cast<int>(_iq[i].imag()) * 16);
  }
  std::fwrite(buf.data(), 4, ns, _rec);
  if (_rec_b) {
    if (!_iqb_fresh) unpack_b(b, _iqb);
    for (size_t i = 0; i < ns; i++) {
      buf[2 * i] = static_cast<int16_t>(static_cast<int>(_iqb[i].real()) * 16);
      buf[2 * i + 1] = static_cast<int16_t>(static_cast<int>(_iqb[i].imag()) * 16);
    }
    std::fwrite(buf.data(), 4, ns, _rec_b);
  }
  _rec_left -= ns;
  _rec_samples += ns;
  if (_rec_left == 0) {
    std::fclose(_rec);
    _rec = nullptr;
    const bool with_b = _rec_b != nullptr;
    if (_rec_b) std::fclose(_rec_b), _rec_b = nullptr;
    const std::string base = _rec_prefix.substr(_rec_prefix.find_last_of("/\\") + 1);
    if (FILE *j = std::fopen((_rec_prefix + ".json").c_str(), "w")) {
      std::fprintf(j, "{\"format\":\"cs16\",\"sample_rate\":%.0f,\"centre_hz\":%.0f,\"shift_hz\":%.0f,\"samples\":%llu,"
                   "\"source\":\"kepyur watch\"", _rec_fs, _rec_centre, _rec_shift, (unsigned long long)_rec_samples);
      if (with_b) std::fprintf(j, ",\"path_b\":\"%s_b.cs16\"", base.c_str());  // beside it, as Play RX writes it
      std::fprintf(j, "}\n");
      std::fclose(j);
    }
    KP_LOGI("record: done, %llu samples -> %s.cs16", (unsigned long long)_rec_samples, _rec_prefix.c_str());
  }
}

void Radio::decode_loop() {
  boost_thread("decode");
  keep_off_slow_cores("decode");
  std::shared_ptr<FrontEnd> cur;
  double busy = 0;
  int64_t t0 = now_ms();
  while (!_stop) {
    DBlock blk;
    {
      std::unique_lock<std::mutex> l(_dqm);
      _dqcv.wait_for(l, std::chrono::milliseconds(200), [&] { return !_dq.empty() || _stop; });
      if (_dq.empty()) continue;
      blk = std::move(_dq.front());
      _dq.pop_front();
    }
    const auto ta = std::chrono::steady_clock::now();
    if (blk.fe != cur) {
      cur = blk.fe;
      _frames_at_last = 0;
      _fps_t0_ms = now_ms();
    }
    // the frame this block is in, and the next while the decoder re-phases, are torn: not shown (see _gate). At
    // most once every kConcealEveryMs: a stream restarting several times a second shows torn frames rather
    // than a frozen one.
    if (blk.gap && _conceal_on) {
      const int64_t t = now_ms();
      if (t - _conceal_t_ms >= kConcealEveryMs) {
        _conceal = kConcealFrames;
        _conceal_t_ms = t;
      }
    }
    cur->decode(blk.d.data(), blk.d.size(), blk.bad.size() == blk.d.size() ? blk.bad.data() : nullptr);
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - ta).count();
    {
      std::lock_guard<std::mutex> l(_dqm);
      if (_dpool.size() < 256) _dpool.push_back(std::move(blk.d));
      if (_bpool.size() < 256) _bpool.push_back(std::move(blk.bad));
    }
    const int64_t t = now_ms();
    if (t - _fps_t0_ms >= 1000) {
      const auto s = cur->stats();
      std::lock_guard<std::mutex> l(_m);
      _vstats = s;
      _fps = static_cast<float>((s.frames - _frames_at_last) * 1000.0 / (t - _fps_t0_ms));
      _frames_at_last = s.frames;
      _fps_t0_ms = t;
    }
    if (t - t0 >= 1000) {
      std::lock_guard<std::mutex> l(_m);
      _dload = busy / ((t - t0) / 1000.0);
      busy = 0;
      t0 = t;
    }
  }
}

void Radio::handle_scan(Bank &b) {
  if (_at >= _plan.size()) return;
  const double req = _plan[_at];
  if (std::fabs(b.centre_hz + b.shift_hz - req) > 1e3 || b.fs != kFs) return;
  if (_settle < 0) {
    _settle = kSettleBanks;
    _cap_restarts = 0;
    _cap.clear();
    _cap_need = static_cast<size_t>(kPsdSpan * kFs);  // first just enough for the spectrum
    _humps.clear();
  }
  if (_settle > 0) {
    _settle--;
    _cap_last_seq = b.seq;
    _cap_gen = b.gen;
    return;
  }
  // A gap (banks dropped on the way, or the stream restarted): start the capture over. Not for ever: on a link
  // that loses a bank every few ms no capture would ever be whole and the scan would stay on this step, so after
  // kCapRestarts tries it is taken with its gaps (the sync check loses a few lines at each).
  const bool gap = b.seq != _cap_last_seq + 1 || b.gen != _cap_gen;
  if (!_cap.empty() && gap && _cap_restarts++ < kCapRestarts) _cap.clear();
  _cap_last_seq = b.seq;
  _cap_gen = b.gen;
  _cap_centre = b.centre_hz;
  const size_t ns = b.data.size() / 8;
  const uint8_t *p = b.data.data();
  for (size_t i = 0; i < ns && _cap.size() < _cap_need; i++) {
    uint64_t w;
    std::memcpy(&w, p + 8 * i, 8);
    _cap.emplace_back(static_cast<int16_t>(wsdr::la_i(w)), static_cast<int16_t>(wsdr::la_q(w)));
  }
  if (_cap.size() < _cap_need) return;
  if (_cap_need < static_cast<size_t>(kCapture * kFs)) {
    // spectrum first: a step with no candidate is done now; otherwise keep capturing for the sync check
    std::vector<cf> x(_cap.size());
    for (size_t i = 0; i < _cap.size(); i++) x[i] = cf(_cap[i].real(), _cap[i].imag());
    Psd psd(1024);
    psd.add(x.data(), x.size());
    _humps = find_humps(psd.db(), kFs, kEdge);
    if (!_humps.empty()) {
      _cap_need = static_cast<size_t>(kCapture * kFs);
      return;
    }
  }
  scan_step_done();
}

void Radio::scan_step_done() {
  const double centre = _cap_centre;  // the samples are centred on the hardware centre, not the request
  // start the next retune now; the chip settles while this step is analysed
  _at++;
  if (_at < _plan.size()) {
    tune(_plan[_at], 1);
    _settle = -1;
  }
  std::vector<cf> x(_humps.empty() ? 0 : _cap.size());
  for (size_t i = 0; i < x.size(); i++) x[i] = cf(_cap[i].real(), _cap[i].imag());
  const auto humps = _humps;
  std::vector<Pilot> found;
  for (const auto &h : humps) {
    const auto vc = check_video(x.data(), x.size(), kFs, h.offset_hz);
    const double mhz = (centre + h.offset_hz) / 1e6;
    KP_LOGI("scan: %.1f MHz hump %+.2f MHz (%.1f MHz wide, %.1f dB) -> %s %s lock %.2f", centre / 1e6,
            h.offset_hz / 1e6, h.width_hz / 1e6, h.snr_db, vc.video ? "VIDEO" : "no video", vc.std_name, vc.lock);
    if (!vc.video) continue;
    Pilot pl;
    pl.mhz = mhz;
    const auto *ch = nearest_channel(mhz);
    pl.name = ch ? ch->name : "";
    pl.std_name = vc.std_name;
    pl.snr_db = h.snr_db;
    pl.lock = vc.lock;
    pl.seen_ms = now_ms();
    found.push_back(pl);
  }
  const bool done = _at >= _plan.size();
  double next_watch = 0;
  {
    std::lock_guard<std::mutex> l(_m);
    // A mode call made since this bank's mode was read (from another thread, while this step was analysed)
    // has the last word: the status shows its progress (a scan called again starts at 0), and the end of
    // this scan does not put a watch of its own choosing over it. The next bank takes that call up.
    const bool current = _mode_epoch == _epoch_seen;
    if (current) _step = _at;
    for (const auto &f : found) {
      auto it = std::find_if(_pilots.begin(), _pilots.end(), [&](const Pilot &p) { return std::fabs(p.mhz - f.mhz) < 4; });
      if (it == _pilots.end()) _pilots.push_back(f);
      else if (f.snr_db >= it->snr_db - 3) *it = f;  // prefer the newer sighting unless much weaker
      else it->seen_ms = f.seen_ms;
    }
    std::sort(_pilots.begin(), _pilots.end(), [](const Pilot &a, const Pilot &b) { return a.snr_db > b.snr_db; });
    if (done && current) {
      // after a scan: the pilot watched before, if still there, else the strongest seen in this scan
      const int64_t scan_start = now_ms() - 60000;
      for (const auto &p : _pilots)
        if (_resume_hz > 0 && std::fabs(p.mhz * 1e6 - _resume_hz) < 4e6) next_watch = p.mhz * 1e6;
      if (next_watch == 0)
        for (const auto &p : _pilots)
          if (p.seen_ms >= scan_start) {
            next_watch = p.mhz * 1e6;
            break;
          }
      if (next_watch == 0 && _resume_hz > 0) next_watch = _resume_hz;
      KP_LOGI("scan: done, %zu pilot(s)%s", _pilots.size(), next_watch > 0 ? ", watching" : "");
      if (next_watch > 0) {
        _watch_hz = next_watch;
        _mode = Mode::Watch;
      } else {
        _mode = Mode::Idle;
      }
      _mode_epoch++;
    }
  }
  _cap.clear();
}

void Radio::handle_test(Bank &b) {
  if (std::fabs(b.fs - _test_fs) > 1) return;
  const int64_t t = now_ms();
  if (_test_t0_ms == 0) {
    _test_t0_ms = t;
    _test_last_seq = b.seq;
    _test_gen = b.gen;
    _test_runs = 1;
    KP_LOGI("test: first bank at %.1f MS/s (gen %u)", b.fs / 1e6, b.gen);
  } else if (b.gen != _test_gen) {
    _test_gen = b.gen;  // a stream restart (seq starts over): counted as a run, not as a gap
    _test_runs++;
    KP_LOGW("test: stream restarted (gen %u): %s", b.gen, _rx->status().last_fault.c_str());
  } else if (b.seq != _test_last_seq + 1) {
    _test_gaps++;
  }
  _test_last_seq = b.seq;
  _test_banks++;
  const double el = (t - _test_t0_ms) / 1000.0;
  const double expect = el * b.fs / wsdr::Stream8822b::kBankSamples;
  std::ostringstream o;
  o << "{\"rate_msps\":" << b.fs / 1e6 << ",\"seconds\":" << el << ",\"banks\":" << _test_banks
    << ",\"expected\":" << static_cast<uint64_t>(expect) << ",\"seq_gaps\":" << _test_gaps
    << ",\"stream_runs\":" << _test_runs
    << ",\"restarts\":" << _rx->status().restarts << ",\"queue_dropped\":" << _dropped.load()
    << ",\"done\":" << (el * 1000 >= _test_end_ms ? "true" : "false") << "}";
  std::lock_guard<std::mutex> l(_m);
  _test_result = o.str();
  // (not over a mode call made since this bank's mode was read: as at the end of a scan)
  if (el * 1000 >= _test_end_ms && _mode_epoch == _epoch_seen) {
    KP_LOGI("test: result %s", _test_result.c_str());
    _mode = Mode::Idle;
    _mode_epoch++;
  }
}

static std::string jstr(const std::string &s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) continue;
    o += c;
  }
  return o + "\"";
}

std::string Radio::status_json() {
  std::ostringstream o;
  wsdr::RxStatus rs;
  {
    std::lock_guard<std::mutex> l(_rxm);
    if (_rx) rs = _rx->status();
  }
  std::lock_guard<std::mutex> l(_m);
  // A JSON number or nothing: a nan or inf printed as it comes is not JSON, and a reader may choke on it or,
  // worse, take it for a string (Android's does).
  const auto num = [](double v) { return std::isfinite(v) ? v : 0.0; };
  static const char *modes[] = {"idle", "scan", "watch", "test"};
  o << "{\"open\":" << (_open ? "true" : "false") << ",\"usb3\":" << (_superspeed ? "true" : "false")
    << ",\"chip\":" << jstr(_chip) << ",\"rx_state\":" << jstr(wsdr::rx_state_name(rs.state))
    << ",\"rx_error\":" << jstr(rs.error) << ",\"last_fault\":" << jstr(rs.last_fault)
    << ",\"mode\":\"" << modes[static_cast<int>(_mode)] << "\",\"freq_hz\":" << num(_req_hz)
    << ",\"watch_hz\":" << num(_watch_hz) << ",\"fs\":" << num(rs.fs) << ",\"banks\":" << _banks.load()
    << ",\"dropped\":" << _dropped.load() << ",\"decode_dropped\":" << _ddropped.load()
    << ",\"stream_dropped\":" << rs.mid_drops
    << ",\"restarts\":" << rs.restarts << ",\"dsp_load\":" << num(_load) << ",\"decode_load\":" << num(_dload)
    << ",\"watch_msps\":" << (_mode == Mode::Watch ? wsdr::Receiver::rate_of(_req_rate) / 1e6 : 0)
    << ",\"fallback\":" << (_fallback ? "true" : "false") << ",\"dead\":" << (_dead ? "true" : "false")
    << ",\"wedged\":" << (_wedged ? "true" : "false")
    << ",\"offset_tuning\":" << (_offset ? "true" : "false") << ",\"centre_hz\":" << num(rs.centre_hz);
  if (_mode == Mode::Watch && _snr_db >= 0) o << ",\"snr_db\":" << num(_snr_db);
  if (_mode == Mode::Scan) o << ",\"scan_step\":" << _step.load() << ",\"scan_steps\":" << _plan_n.load();
  o << ",\"video\":{\"locked\":" << (!_vstats.acquiring && _vstats.lock_recent > 0.5f ? "true" : "false")
    << ",\"lock\":" << num(_vstats.lock_recent) << ",\"std\":" << jstr(_vstats.std_name) << ",\"fps\":" << num(_fps)
    << ",\"acquisitions\":" << _vstats.acquisitions << ",\"losses\":" << _vstats.losses
    << ",\"rephases\":" << _vstats.rephases << ",\"clicks_repaired\":" << _vstats.clicks_repaired
    << ",\"lines_mended\":" << _vstats.lines_mended << ",\"colour\":" << (_vstats.colour ? "true" : "false") << "}";
  {
    const auto &d = _comb_status.s;
    o << ",\"antennas\":{\"combine\":" << (_comb_status.on ? "true" : "false") << ",\"snr_a\":" << num(d.snr_a_db)
      << ",\"snr_b\":" << num(d.snr_b_db) << ",\"snr_c\":" << num(d.snr_c_db) << ",\"b_share\":" << num(d.b_share)
      << ",\"a_fade\":" << num(d.a_fade) << ",\"rescued\":" << num(d.rescued) << ",\"coherence\":" << num(d.coherence)
      << ",\"lag\":" << d.lag << ",\"lag_measured\":" << (d.lag_measured ? "true" : "false") << "}";
  }
  o << ",\"test\":" << (_test_result.empty() ? "null" : _test_result);
  o << ",\"pilots\":[";
  const int64_t t = now_ms();
  for (size_t i = 0; i < _pilots.size(); i++) {
    const auto &p = _pilots[i];
    o << (i ? "," : "") << "{\"mhz\":" << num(p.mhz) << ",\"name\":" << jstr(p.name) << ",\"std\":" << jstr(p.std_name)
      << ",\"snr\":" << num(p.snr_db) << ",\"age_s\":" << (t - p.seen_ms) / 1000 << "}";
  }
  o << "]}";
  return o.str();
}

}  // namespace kp
