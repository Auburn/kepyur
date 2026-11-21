// kepyur JNI: the Kotlin app's view of the radio and the screen (com.kepyur.app.Native).
// Frames go straight to the video Surface: a render thread owns an EGL context on it and draws each frame as
// it arrives (latest frame wins; the decode thread never waits for the display).
#include <EGL/egl.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log.h"
#include "radio.h"
#include "test_card.h"
#include "usbmode.h"
#include "video.h"
#include "video_renderer.h"

using namespace kp;

namespace {

// ---- log file ----
std::mutex g_log_m;
FILE *g_log = nullptr;
void file_sink(LogLevel l, const char *line) {
  std::lock_guard<std::mutex> lk(g_log_m);
  if (!g_log) return;
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  tm t;
  localtime_r(&ts.tv_sec, &t);
  static const char tag[] = {'D', 'I', 'W', 'E'};
  std::fprintf(g_log, "%02d:%02d:%02d.%03ld %c %s\n", t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000,
               tag[static_cast<int>(l)], line);
  std::fflush(g_log);
}

// ---- the screen ----
// One thread owns the EGL context and everything GL. Others hand it things under _m: a frame (post), a window
// (set_window, which waits until the old one has been let go), the view settings, the menu backdrop, where the
// menu's preview is and what the card says where there is no picture.
class Screen {
  using Clock = std::chrono::steady_clock;

public:
  Screen() : _th([this] { loop(); }) {}
  ~Screen() {
    {
      std::lock_guard<std::mutex> l(_m);
      _stop = true;
    }
    _cv.notify_all();
    _th.join();
  }

  // The Surface to draw on, or nullptr when it is going away. Takes the reference. Returns once the render
  // thread no longer touches the previous window: the app may destroy its Surface right after.
  void set_window(ANativeWindow *w) {
    std::unique_lock<std::mutex> l(_m);
    if (_win_next_set && _win_next) ANativeWindow_release(_win_next);  // never picked up
    _win_next = w;
    _win_next_set = true;
    const uint64_t want = ++_win_seq;
    _cv.notify_all();
    // a render thread that died (no GL at all) must not hang the UI thread
    _done.wait_for(l, std::chrono::milliseconds(1500), [&] { return _win_done >= want || _stop; });
  }

  // The picture stays for up to `ms` (dimmed once it is stale, not faded into NO SIGNAL), and clear() is ignored meanwhile:
  // for the few seconds the adapter is opened again after its USB data path wedged. The next frame ends it.
  void hold(int ms) {
    {
      std::lock_guard<std::mutex> l(_m);
      _hold_until = Clock::now() + std::chrono::milliseconds(std::max(0, ms));
    }
    _cv.notify_one();
  }

  // Decode thread: copy and go.
  void post(const Frame &f) {
    {
      std::lock_guard<std::mutex> l(_m);
      if (Clock::now() < _quiet_until) return;
      _hold_until = {};
      _w = f.width;
      _h = f.height;
      _pending.assign(f.data, f.data + static_cast<size_t>(f.width) * f.height * 4);
      _have = true;
    }
    _cv.notify_one();
  }

  // No picture any more (the radio closed, another channel was asked for): NO SIGNAL until the next frame. The
  // decoder may still have a frame of the old picture on its way: frames are refused for a moment (no new
  // picture can be locked that quickly). Not while a picture is held (hold()).
  void clear() {
    {
      std::lock_guard<std::mutex> l(_m);
      if (Clock::now() < _hold_until) return;
      _have = false;
      _clear = true;
      _quiet_until = Clock::now() + std::chrono::milliseconds(120);
    }
    _cv.notify_one();
  }

  // Layout (the inset, box, radius and gain fields are the screen's own business).
  void set_view(const ViewOpts &v) {
    {
      std::lock_guard<std::mutex> l(_m);
      _view = v;
      _dirty = true;
    }
    _cv.notify_one();
  }

  // The menu's backdrop: black, the picture moved into the preview. Eased in and out (the picture flies there and
  // back) unless `now`.
  void set_backdrop(bool on, bool now) {
    {
      std::lock_guard<std::mutex> l(_m);
      _inset_to = on ? 1.f : 0.f;
      if (now) _inset_snap = true;
      _dirty = true;
    }
    _cv.notify_one();
  }

  // What the card shown where there is no picture says: the chosen channel ("R8"), or "" for NO SIGNAL.
  void set_card_words(const std::string &words) {
    {
      std::lock_guard<std::mutex> l(_m);
      _words = words;
      _dirty = true;
    }
    _cv.notify_one();
  }

  // Where the menu's preview is on the window, in pixels, and its corner radius.
  void set_preview(const Rect &box, float radius) {
    {
      std::lock_guard<std::mutex> l(_m);
      _box = box;
      _radius = radius;
      _dirty = true;
    }
    _cv.notify_one();
  }

  void redraw() {
    {
      std::lock_guard<std::mutex> l(_m);
      _dirty = true;
    }
    _cv.notify_one();
  }

private:
  static constexpr float kInsetSeconds = 0.28f;  // the picture's way into the preview and back
  static constexpr double kStaleSeconds = 1.2;   // no frame for this long: the picture is not live any more ...
  static constexpr float kFadeSeconds = 0.5f;    // ... and fades into NO SIGNAL over this, dimming on the way ...
  static constexpr float kHoldGain = 0.6f;       // ... to this; while held (hold()) it stays up, only dimmed

  // EGL sorts the configs that match by colour depth, deepest first, so the first one is 10-bit wherever the
  // driver has such a config. The picture is 8-bit, and its buffer format then follows the phone, with a 2-bit
  // alpha the compositor may not put on an overlay. Of the configs that are exactly 8 bits a colour, take the
  // one with the least besides: no multisampling first (it costs the GPU on every frame), then opaque, then no
  // depth or stencil buffer (memory only; the emulator's EGL has no config without them). Only a driver with no
  // 8-bit config at all gets its first choice.
  static EGLConfig pick_config(EGLDisplay dpy) {
    const EGLint attr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attr, nullptr, 0, &n) || n < 1) return nullptr;
    std::vector<EGLConfig> all(n);
    if (!eglChooseConfig(dpy, attr, all.data(), n, &n) || n < 1) return nullptr;
    const auto get = [dpy](EGLConfig c, EGLint a) {
      EGLint v = -1;
      eglGetConfigAttrib(dpy, c, a, &v);
      return v;
    };
    EGLConfig best = all[0];
    int best_cost = 1 << 30;  // of an 8-bit config; lower is better, the driver's order decides between equals
    for (EGLint i = 0; i < n; i++) {
      const EGLConfig c = all[i];
      if (get(c, EGL_RED_SIZE) != 8 || get(c, EGL_GREEN_SIZE) != 8 || get(c, EGL_BLUE_SIZE) != 8) continue;
      const EGLint a = get(c, EGL_ALPHA_SIZE);
      const int cost = (get(c, EGL_SAMPLE_BUFFERS) != 0 || get(c, EGL_SAMPLES) > 0 ? 100 : 0) +
                       (a == 0 ? 0 : a == 8 ? 10 : 20) + (get(c, EGL_DEPTH_SIZE) != 0 ? 2 : 0) +
                       (get(c, EGL_STENCIL_SIZE) != 0 ? 1 : 0);
      if (cost < best_cost) best = c, best_cost = cost;
    }
    KP_LOGI("screen: EGL config R%d G%d B%d A%d, depth %d, stencil %d, samples %d, native visual 0x%x%s",
            get(best, EGL_RED_SIZE), get(best, EGL_GREEN_SIZE), get(best, EGL_BLUE_SIZE), get(best, EGL_ALPHA_SIZE),
            get(best, EGL_DEPTH_SIZE), get(best, EGL_STENCIL_SIZE), get(best, EGL_SAMPLES),
            get(best, EGL_NATIVE_VISUAL_ID), best_cost == 1 << 30 ? " (no 8-bit one: the driver's first)" : "");
    return best;
  }

  bool egl_up() {
    if (_dpy != EGL_NO_DISPLAY) return _ctx != EGL_NO_CONTEXT;
    _dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (_dpy == EGL_NO_DISPLAY || !eglInitialize(_dpy, nullptr, nullptr)) {
      KP_LOGE("screen: no EGL display (0x%x)", eglGetError());
      return false;
    }
    _cfg = pick_config(_dpy);
    if (!_cfg) {
      KP_LOGE("screen: no OpenGL ES 3 config (0x%x)", eglGetError());
      return false;
    }
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    _ctx = eglCreateContext(_dpy, _cfg, EGL_NO_CONTEXT, ctx_attr);
    if (_ctx == EGL_NO_CONTEXT) KP_LOGE("screen: no OpenGL ES 3 context (0x%x)", eglGetError());
    return _ctx != EGL_NO_CONTEXT;
  }

  void drop_surface() {
    if (_surf != EGL_NO_SURFACE) {
      eglMakeCurrent(_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroySurface(_dpy, _surf);
      _surf = EGL_NO_SURFACE;
    }
    if (_win) {
      ANativeWindow_release(_win);
      _win = nullptr;
    }
  }

  void take_window(ANativeWindow *w) {
    drop_surface();
    if (!w) return;
    _win = w;
    if (!egl_up()) return;
    EGLint format = 0;
    eglGetConfigAttrib(_dpy, _cfg, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(w, 0, 0, format);
    _surf = eglCreateWindowSurface(_dpy, _cfg, w, nullptr);
    if (_surf == EGL_NO_SURFACE || !eglMakeCurrent(_dpy, _surf, _surf, _ctx)) {
      KP_LOGE("screen: no window surface (0x%x)", eglGetError());
      drop_surface();
      return;
    }
    // Never wait for the display's refresh: a frame is drawn when it is decoded and the compositor shows the
    // newest one. Waiting would add up to a refresh period of delay and stall this thread behind it.
    eglSwapInterval(_dpy, 0);
    if (!_gl_ready) {
      _gl_ready = _gl.init();
      if (!_gl_ready) KP_LOGE("screen: renderer did not start");
      else KP_LOGI("screen: OpenGL ES renderer ready");
      _tex_stale = true;
    }
  }

  void loop() {
    std::vector<uint8_t> px, last;  // the frame in hand, and a copy of the newest (re-uploaded after a GL restart)
    int w = 0, h = 0, lw = 0, lh = 0;
    float inset = 0, gain = 0, bright = 1;  // gain: the picture against NO SIGNAL; bright: the picture's own
    std::string words;
    auto t_prev = Clock::now(), t_frame = t_prev;
    bool live = false;  // a frame within kStaleSeconds
    for (;;) {
      bool got = false, cleared = false, animating;
      ViewOpts view;
      float inset_to;
      bool held;  // a stale picture stays up, dimmed, instead of fading into NO SIGNAL (hold())
      {
        std::unique_lock<std::mutex> l(_m);
        const auto ready = [&] { return _stop || _win_next_set || _have || _clear || _dirty; };
        held = Clock::now() < _hold_until;
        animating = inset != _inset_to ||
                    (live ? gain < 1 || bright < 1 : bright > kHoldGain || (!held && gain > 0));
        if (animating) _cv.wait_for(l, std::chrono::milliseconds(8), ready);
        else if (live) _cv.wait_until(l, t_frame + std::chrono::duration<double>(kStaleSeconds), ready);
        else if (held) _cv.wait_until(l, _hold_until, ready);  // then into NO SIGNAL, unless a frame came
        else _cv.wait(l, ready);
        if (_stop) break;
        held = Clock::now() < _hold_until;
        if (_win_next_set) {
          ANativeWindow *nw = _win_next;
          _win_next = nullptr;
          _win_next_set = false;
          const uint64_t seq = _win_seq;
          l.unlock();
          take_window(nw);
          l.lock();
          _win_done = seq;
          _done.notify_all();
        }
        if (_have) {
          px.swap(_pending);
          w = _w;
          h = _h;
          _have = false;
          got = true;
        }
        cleared = _clear;
        _clear = false;
        _dirty = false;
        view = _view;
        view.box = _box;
        view.radius = _radius;
        words = _words;
        inset_to = _inset_to;
        if (_inset_snap) inset = inset_to;
        _inset_snap = false;
      }
      const auto now = Clock::now();
      // time since the last step of a fade in progress; a fade that starts now starts from here
      const float dt = animating ? std::min(std::chrono::duration<float>(now - t_prev).count(), 0.05f) : 0.f;
      t_prev = now;
      if (cleared && !got) {
        live = false;
        gain = 0;
        last.clear();
      }
      if (got) {
        t_frame = now;
        live = true;
      } else if (live && std::chrono::duration<double>(now - t_frame).count() >= kStaleSeconds) {
        live = false;
      }
      // ease the picture's way into the preview, and its fade, toward where they should be: a live picture comes
      // up quickly; a stale one dims (never brightened while stale) and, unless it is held, fades into NO SIGNAL
      const float istep = dt / kInsetSeconds, gstep = dt / kFadeSeconds;
      inset = inset < inset_to ? std::min(inset_to, inset + istep) : std::max(inset_to, inset - istep);
      if (live) {
        gain = std::min(1.f, gain + 4 * gstep);
        bright = std::min(1.f, bright + 4 * gstep);
      } else {
        if (bright > kHoldGain) bright = std::max(kHoldGain, bright - gstep);
        if (!held) gain = std::max(0.f, gain - gstep);
      }
      if (_surf == EGL_NO_SURFACE || !_gl_ready) {
        if (got) last.swap(px), lw = w, lh = h;
        continue;
      }
      if (got) {
        _gl.upload(px.data(), w, h);
        last.swap(px);
        lw = w, lh = h;
        _tex_stale = false;
      } else if (_tex_stale && !last.empty()) {
        _gl.upload(last.data(), lw, lh);
        _tex_stale = false;
      }
      if (cleared && !got) _gl.clear();
      EGLint sw = 0, sh = 0;
      eglQuerySurface(_dpy, _surf, EGL_WIDTH, &sw);
      eglQuerySurface(_dpy, _surf, EGL_HEIGHT, &sh);
      view.inset = inset * inset * (3 - 2 * inset);  // smoothstep
      view.gain = gain;
      view.bright = bright;
      _gl.set_card_words(words);
      _gl.draw(0, sw, sh, view);
      if (!eglSwapBuffers(_dpy, _surf)) {
        const EGLint e = eglGetError();
        KP_LOGW("screen: swap failed (0x%x)", e);
        if (e == EGL_CONTEXT_LOST) {  // the GPU was reset under us: everything GL is gone, start over
          _gl.destroy(false);
          _gl_ready = false;
          ANativeWindow *keep = _win;
          if (keep) ANativeWindow_acquire(keep);
          drop_surface();
          eglDestroyContext(_dpy, _ctx);
          _ctx = EGL_NO_CONTEXT;
          eglTerminate(_dpy);
          _dpy = EGL_NO_DISPLAY;
          take_window(keep);
        }
      }
    }
    if (_gl_ready && _surf != EGL_NO_SURFACE) _gl.destroy();
    drop_surface();
    if (_dpy != EGL_NO_DISPLAY) {
      if (_ctx != EGL_NO_CONTEXT) eglDestroyContext(_dpy, _ctx);
      eglTerminate(_dpy);
    }
    std::lock_guard<std::mutex> l(_m);
    if (_win_next) ANativeWindow_release(_win_next);
    _win_next = nullptr;
    _win_done = _win_seq;
    _done.notify_all();
  }

  std::mutex _m;
  std::condition_variable _cv, _done;
  // handed over under _m
  ANativeWindow *_win_next = nullptr;
  bool _win_next_set = false;
  uint64_t _win_seq = 0, _win_done = 0;
  std::vector<uint8_t> _pending;
  int _w = 0, _h = 0;
  bool _have = false, _clear = false, _dirty = false, _stop = false;
  std::chrono::steady_clock::time_point _quiet_until{};
  std::chrono::steady_clock::time_point _hold_until{};
  ViewOpts _view;
  float _inset_to = 0;
  bool _inset_snap = false;
  Rect _box{0, 0, 0, 0};
  float _radius = 0;
  std::string _words;
  // render thread only
  EGLDisplay _dpy = EGL_NO_DISPLAY;
  EGLConfig _cfg = nullptr;
  EGLContext _ctx = EGL_NO_CONTEXT;
  EGLSurface _surf = EGL_NO_SURFACE;
  ANativeWindow *_win = nullptr;
  VideoRenderer _gl;
  bool _gl_ready = false, _tex_stale = false;
  std::thread _th;
};

std::unique_ptr<Screen> g_screen;
std::unique_ptr<Radio> g_radio;
// Keeps an open and a close from overlapping. Only the worker that opens and closes takes it: the mode calls (scan,
// watch, idle, streamTest) come from the main thread, which must never wait out a radio bring-up, and they need
// no more than the radio's own lock (each only sets the mode, the radio ignores them while it is not open, and
// an open or close sets the mode itself when it is done).
std::mutex g_api;

// ---- test picture ----
// The screen side of the app without an adapter (the "testcard" extra): the test card as if it were decoded
// video, 30 times a second, with its bar moving.
std::mutex g_card_m;
std::thread g_card;
std::atomic<bool> g_card_on{false};

void card_stop() {
  std::lock_guard<std::mutex> l(g_card_m);
  g_card_on = false;
  if (g_card.joinable()) g_card.join();
}

void card_start() {
  card_stop();
  std::lock_guard<std::mutex> l(g_card_m);
  g_card_on = true;
  g_card = std::thread([] {
    std::vector<uint8_t> px;
    for (int tick = 1; g_card_on; tick++) {
      test_card(px, 720, 480, tick);
      if (g_screen) g_screen->post(Frame{px.data(), 720, 480, &kNtsc});
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
  });
}

std::string jstring_of(JNIEnv *env, jstring s) {
  const char *p = env->GetStringUTFChars(s, nullptr);
  std::string out = p ? p : "";
  if (p) env->ReleaseStringUTFChars(s, p);
  return out;
}

// wifi-sdr reports what its stream does (a start that had to be repeated, a bank dropped, where the chip's CPU got
// to) on stderr, which Android sends nowhere. Into the log with it, a line at a time.
void forward_stderr() {
  int p[2];
  if (pipe(p) != 0) return;
  if (dup2(p[1], STDERR_FILENO) < 0) {
    close(p[0]);
    close(p[1]);
    return;
  }
  close(p[1]);
  setvbuf(stderr, nullptr, _IOLBF, 0);
  std::thread([fd = p[0]] {
    std::string line;
    char buf[512];
    for (;;) {
      const ssize_t n = read(fd, buf, sizeof buf);
      if (n <= 0) return;
      for (ssize_t i = 0; i < n; i++) {
        if (buf[i] != '\n') {
          line += buf[i];
          continue;
        }
        if (!line.empty()) KP_LOGW("wsdr: %s", line.c_str());
        line.clear();
      }
    }
  }).detach();
}
}  // namespace

// Native is a Kotlin object: these are its instance methods, and the second argument is that one instance
// (unused), whatever its type is spelled as.
extern "C" {

// Every new activity calls this on its main thread, possibly while an earlier one's radio is still being opened
// (seconds, or for ever if the adapter hangs): so not under g_api, and the screen and radio are made only once.
JNIEXPORT void JNICALL Java_com_kepyur_app_Native_init(JNIEnv *env, jclass, jstring log_path) {
  const std::string p = jstring_of(env, log_path);
  {
    std::lock_guard<std::mutex> lk(g_log_m);
    if (!g_log) g_log = std::fopen(p.c_str(), "a");
  }
  set_log_sink(file_sink);
  static std::once_flag made;
  std::call_once(made, [] {
    forward_stderr();
    g_screen = std::make_unique<Screen>();
    g_radio = std::make_unique<Radio>([](const Frame &f) { g_screen->post(f); });
  });
  KP_LOGI("kepyur native ready");
}

JNIEXPORT jstring JNICALL Java_com_kepyur_app_Native_usb3Switch(JNIEnv *env, jclass, jint fd) {
  std::string detail;
  const auto r = wsdr::request_usb3_fd(fd, &detail);
  std::string out = wsdr::usb3_switch_name(r);
  if (!detail.empty()) out += ": " + detail;
  KP_LOGI("usb3 switch: %s", out.c_str());
  return env->NewStringUTF(out.c_str());
}

JNIEXPORT jstring JNICALL Java_com_kepyur_app_Native_open(JNIEnv *env, jclass, jint fd) {
  std::lock_guard<std::mutex> l(g_api);
  try {
    g_radio->open(fd);
    return env->NewStringUTF("");
  } catch (const std::exception &e) {
    KP_LOGE("open failed: %s", e.what());
    return env->NewStringUTF(e.what()[0] ? e.what() : "open failed");
  }
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_close(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> l(g_api);
  if (g_radio) g_radio->close();
  if (g_screen) g_screen->clear();
}

JNIEXPORT jboolean JNICALL Java_com_kepyur_app_Native_superspeed(JNIEnv *, jclass) {
  return g_radio && g_radio->superspeed();
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_scan(JNIEnv *, jclass, jdouble prefer_hz) {
  if (g_radio && g_radio->is_open()) g_radio->scan(prefer_hz);
  if (g_screen) g_screen->clear();
}

// fresh: another channel (the old picture goes at once), not the same one again after a settings change
JNIEXPORT void JNICALL Java_com_kepyur_app_Native_watch(JNIEnv *, jclass, jdouble hz, jboolean fresh) {
  if (g_radio && g_radio->is_open()) g_radio->watch(hz);
  if (fresh && g_screen) g_screen->clear();
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_idle(JNIEnv *, jclass) {
  if (g_radio && g_radio->is_open()) g_radio->idle();
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_streamTest(JNIEnv *, jclass, jint rate_sel, jdouble seconds) {
  if (g_radio && g_radio->is_open()) g_radio->stream_test(rate_sel, seconds);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setWatchRate(JNIEnv *, jclass, jint rate_sel) {
  if (g_radio) g_radio->set_watch_rate(rate_sel);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setGain(JNIEnv *, jclass, jdouble db) {
  if (g_radio) g_radio->set_gain(db);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setComb(JNIEnv *, jclass, jboolean on) {
  if (g_radio) g_radio->set_comb(on);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setDiversity(JNIEnv *, jclass, jboolean on) {
  if (g_radio) g_radio->set_combine(on);
}

JNIEXPORT jboolean JNICALL Java_com_kepyur_app_Native_setOffsetTuning(JNIEnv *, jclass, jboolean on) {
  return g_radio && g_radio->set_offset_tuning(on);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setPicture(JNIEnv *, jclass, jboolean luma_comb, jboolean adaptive,
                                                             jboolean smooth, jboolean repair) {
  set_luma_comb(luma_comb);
  set_adaptive_comb(adaptive);
  set_smooth_lines(smooth);
  set_repair(repair);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setView(JNIEnv *, jclass, jfloat aspect, jint fit, jfloat scale,
                                                          jboolean stereo, jfloat eye_shift) {
  if (!g_screen) return;
  ViewOpts v;
  v.aspect = aspect > 0 ? aspect : 4.f / 3;
  v.fit = fit == 1 ? Fit::Stretch : fit == 2 ? Fit::Crop : Fit::Fit;
  v.scale = std::clamp(scale, 0.2f, 3.f);
  v.stereo = stereo;
  v.eye_shift = std::clamp(eye_shift, -0.25f, 0.25f);
  g_screen->set_view(v);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setBackdrop(JNIEnv *, jclass, jboolean on, jboolean now) {
  if (g_screen) g_screen->set_backdrop(on, now);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setCardLabel(JNIEnv *env, jclass, jstring label) {
  if (g_screen) g_screen->set_card_words(jstring_of(env, label));
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setPreview(JNIEnv *, jclass, jint left, jint top, jint right,
                                                             jint bottom, jfloat radius) {
  if (!g_screen) return;
  const float w = static_cast<float>(std::max(0, right - left)), h = static_cast<float>(std::max(0, bottom - top));
  g_screen->set_preview(Rect{static_cast<float>(left), static_cast<float>(top), w, h}, std::max(0.f, radius));
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_record(JNIEnv *env, jclass, jdouble seconds, jstring prefix) {
  const std::string p = jstring_of(env, prefix);
  if (g_radio) g_radio->record(seconds, p);
}

JNIEXPORT jstring JNICALL Java_com_kepyur_app_Native_status(JNIEnv *env, jclass) {
  const std::string s = g_radio ? g_radio->status_json() : std::string("{\"open\":false}");
  return env->NewStringUTF(s.c_str());
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setSurface(JNIEnv *env, jclass, jobject surface) {
  if (!g_screen) return;
  g_screen->set_window(surface ? ANativeWindow_fromSurface(env, surface) : nullptr);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_redraw(JNIEnv *, jclass) {
  if (g_screen) g_screen->redraw();
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_testCard(JNIEnv *, jclass, jboolean on) {
  KP_LOGI("test picture %s", on ? "on" : "off");
  if (on) {
    card_start();
  } else {
    card_stop();
    if (g_screen) g_screen->clear();
  }
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_log(JNIEnv *env, jclass, jstring msg) {
  KP_LOGI("app: %s", jstring_of(env, msg).c_str());
}

// Keep the picture on screen (dimmed once stale) for up to `ms`, through the close and reopen of a wedged adapter.
JNIEXPORT void JNICALL Java_com_kepyur_app_Native_holdPicture(JNIEnv *, jclass, jint ms) {
  if (g_screen) g_screen->hold(ms);
}

// Debug: an environment switch for wifi-sdr / devourer (WSDR_URB_DEPTH and the like), read at the next open or
// stream start. An empty value removes it.
JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setDebugEnv(JNIEnv *env, jclass, jstring key, jstring value) {
  const std::string k = jstring_of(env, key), v = jstring_of(env, value);
  if (k.empty()) return;
  if (v.empty()) unsetenv(k.c_str());
  else setenv(k.c_str(), v.c_str(), 1);
  KP_LOGI("debug: env %s=%s", k.c_str(), v.c_str());
}

}  // extern "C"
