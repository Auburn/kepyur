// A stand-in for wsdr::Receiver that plays a recording back as a live, tunable 8812BU (kp_sim only).
// The recording's VTX stays at its real frequency: tuning elsewhere moves it in the band, and a step far
// from it shows only noise. Bank pacing is real time. Env: KP_SIM_FILE (.cs16, 40 MS/s, centre 5770 MHz).
// RxSettings::centre is honoured like the chip does (the hardware centre moves beside the pilot, shift_hz = freq -
// centre, and the recording moves with the centre). Path B is a second antenna for the combiner as the 8812BU
// delivers one: the same signal turned by 90 degrees at half amplitude, with noise of its own, and ahead of
// path A by the adapter's 146 ns (6 samples at 40 MS/s, 3 at 20 MS/s: wifi-sdr FINDINGS.md, "8822B dual-path
// tap"). A combiner that adds the two as they come loses the colour on it.
// KP_SIM_DROP_EVERY=N loses every Nth bank the way the adapter's stream drops one it collected too late: not
// delivered, counted in RxStatus::mid_drops, and BankMeta::seq steps over it.
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "receiver.h"
#include "stream88xx.h"

namespace wsdr {

const char *rx_state_name(RxState s) {
  switch (s) {
  case RxState::Closed: return "closed";
  case RxState::Opening: return "opening";
  case RxState::Starting: return "starting";
  case RxState::Streaming: return "streaming";
  case RxState::Lost: return "lost";
  case RxState::Error: return "error";
  }
  return "?";
}

namespace {
struct Sim {
  std::vector<int16_t> rec;  // I,Q (value << 4)
  size_t pos = 0;
  double rec_centre = 5770e6;
  unsigned drop_every = 0;  // KP_SIM_DROP_EVERY
};
Sim *g_sim = nullptr;
constexpr double kLeadB = 146e-9;  // path B ahead of path A
}  // namespace

Receiver::Receiver(Logger_t log) : _log(std::move(log)) {}
Receiver::~Receiver() { close(); }

void Receiver::set_state(RxState s, const std::string &err) {
  std::lock_guard<std::mutex> l(_m);
  _st.state = s;
  _st.error = err;
}

void Receiver::open(uint16_t pid, const RxSettings &initial, int, const std::string &, int) {
  const char *f = std::getenv("KP_SIM_FILE");
  if (!f) throw std::runtime_error("KP_SIM_FILE not set");
  FILE *fp = std::fopen(f, "rb");
  if (!fp) throw std::runtime_error(std::string("cannot open ") + f);
  g_sim = new Sim;
  std::fseek(fp, 0, SEEK_END);
  const long bytes = std::min<long>(std::ftell(fp), 40000000L * 4 * 3);  // up to 3 s
  std::fseek(fp, 0, SEEK_SET);
  g_sim->rec.resize(bytes / 2);
  if (std::fread(g_sim->rec.data(), 2, g_sim->rec.size(), fp) != g_sim->rec.size()) throw std::runtime_error("short read");
  std::fclose(fp);
  if (const char *d = std::getenv("KP_SIM_DROP_EVERY")) g_sim->drop_every = static_cast<unsigned>(std::atoi(d));
  _set = initial;
  _caps = {};
  _caps.pid = pid;
  _caps.superspeed = std::getenv("KP_SIM_USB2") == nullptr;
  _caps.chip = "SIM (recording)";
  _caps.min_rate_sel = _caps.superspeed ? 1 : 4;
  _open = true;
  set_state(RxState::Starting);
}

void Receiver::close() {
  stop();
  _open = false;
  delete g_sim;
  g_sim = nullptr;
}

void Receiver::start(BankSink sink) {
  _sink = std::move(sink);
  _active = true;
  _worker = std::thread([this] { worker(); });
}

void Receiver::stop() {
  _active = false;
  if (_worker.joinable()) _worker.join();
}

void Receiver::set(const RxSettings &s) {
  std::lock_guard<std::mutex> l(_m);
  _set = s;
  _restart = true;
}

RxSettings Receiver::settings() const {
  std::lock_guard<std::mutex> l(_m);
  return _set;
}

RxStatus Receiver::status() const {
  std::lock_guard<std::mutex> l(_m);
  RxStatus s = _st;
  s.running = _active;
  return s;
}

void Receiver::worker() {
  std::mt19937 rng(7);
  std::normal_distribution<float> nd(0, 1);
  const size_t N = Stream8822b::kBankSamples;
  std::vector<uint8_t> bank(N * 8);
  // path B's own noise, from a table: the thread has no time for a second pair of Gaussian draws per sample, and
  // path A's draws must stay as they are (a watch without the combiner decodes the same frames as ever)
  std::vector<float> nb(4096);
  {
    std::mt19937 rb(11);
    for (auto &v : nb) v = nd(rb);
  }
  uint32_t xs = 2463534242u;
  std::vector<std::complex<float>> clean(N + 16);  // the bank before noise, and the next samples path B is ahead by
  uint64_t seq = 0, run_banks = 0;
  double ph = 0;
  auto t_next = std::chrono::steady_clock::now();
  RxSettings s;
  BankMeta m;
  bool first = true;
  while (_active) {
    bool restart;
    {
      std::lock_guard<std::mutex> l(_m);
      restart = _restart || first;
      _restart = false;
      s = _set;
    }
    if (restart) {
      if (!first) std::this_thread::sleep_for(std::chrono::milliseconds(150));  // retune + stream restart
      first = false;
      m = {};
      m.gen = ++_gen;
      m.fs = rate_of(s.rate_sel);
      // the chip's 40 MHz channels put the centre on the 5 MHz grid, nearest RxSettings::centre if one is set
      m.centre_hz = std::lround((s.centre > 0 ? s.centre : s.freq) / 5e6) * 5e6;
      m.shift_hz = s.freq - m.centre_hz;
      seq = 0;
      run_banks = 0;
      t_next = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> l(_m);
      _st.state = RxState::Streaming;
      _st.fs = m.fs;
      _st.centre_hz = m.centre_hz;
      _st.gen = m.gen;
    }
    // the recording at 40 MS/s, moved by (rec centre - hw centre); decimated naively for lower rates
    const int dec = static_cast<int>(std::lround(40e6 / m.fs));
    const double move = g_sim->rec_centre - m.centre_hz;
    const bool visible = std::fabs(move) < 30e6;
    const std::complex<float> rot(static_cast<float>(std::cos(2 * M_PI * move / 40e6)),
                                  static_cast<float>(std::sin(2 * M_PI * move / 40e6)));
    std::complex<float> lo(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    // one output sample, before noise
    const auto next = [&](size_t &pos, std::complex<float> &osc) {
      float re = 0, im = 0;
      for (int k = 0; k < dec; k++) {
        const size_t p = pos;
        pos = (pos + 2) % g_sim->rec.size();
        if (visible) {
          const std::complex<float> x(g_sim->rec[p] / 16.f, g_sim->rec[p + 1] / 16.f);
          const auto y = x * osc;
          re += y.real();
          im += y.imag();
        }
        osc *= rot;
      }
      return std::complex<float>(re / dec, im / dec);
    };
    for (size_t i = 0; i < N; i++) clean[i] = next(g_sim->pos, lo);
    ph = std::arg(lo);
    // path B runs ahead: its last samples of this bank are the first of the next one (looked at, not consumed)
    const size_t lead = static_cast<size_t>(std::lround(kLeadB * m.fs));
    {
      size_t pos = g_sim->pos;
      std::complex<float> osc = lo;
      for (size_t i = 0; i < lead; i++) clean[N + i] = next(pos, osc);
    }
    for (size_t i = 0; i < N; i++) {
      const float re = clean[i].real() + 3 * nd(rng);  // a little noise so empty steps have a floor
      const float im = clean[i].imag() + 3 * nd(rng);
      const int ai = static_cast<int>(std::lround(re)), aq = static_cast<int>(std::lround(im));
      const uint64_t li = static_cast<uint64_t>(ai & 0xFFF);
      const uint64_t lq = static_cast<uint64_t>(aq & 0xFFF);
      // path B (kDmaBothPaths8822b: I [35:24], Q [47:36]) = j * A / 2, `lead` samples on, plus its own noise
      // (rounded by hand: two more calls of lround per sample are time this thread does not have)
      xs ^= xs << 13, xs ^= xs >> 17, xs ^= xs << 5;
      const std::complex<float> b = clean[i + lead];
      const float fi = -0.5f * b.imag() + 3 * nb[xs & 4095], fq = 0.5f * b.real() + 3 * nb[(xs >> 12) & 4095];
      const int pi = static_cast<int>(fi + (fi < 0 ? -0.5f : 0.5f));
      const int pq = static_cast<int>(fq + (fq < 0 ? -0.5f : 0.5f));
      const uint64_t bi = static_cast<uint64_t>(pi & 0xFFF);
      const uint64_t bq = static_cast<uint64_t>(pq & 0xFFF);
      const uint64_t w = li | (lq << 12) | (bi << 24) | (bq << 36);
      std::memcpy(bank.data() + 8 * i, &w, 8);
    }
    run_banks++;
    if (g_sim->drop_every && run_banks > 4 && run_banks % g_sim->drop_every == 0) {
      seq++;  // lost: the next bank's seq shows the hole
      std::lock_guard<std::mutex> l(_m);
      _st.mid_drops++;
    } else {
      m.seq = seq++;
      _sink(bank.data(), bank.size(), m);
      std::lock_guard<std::mutex> l(_m);
      _st.banks++;
    }
    t_next += std::chrono::nanoseconds(static_cast<int64_t>(N / m.fs * 1e9));
    std::this_thread::sleep_until(t_next);
  }
}

}  // namespace wsdr
