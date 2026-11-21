// kepyur: the picture on screen (OpenGL ES 3). Draws the decoder's frames into a viewport: laid out by aspect
// ratio, fit mode and scale, once or twice side by side (goggles that hold a phone), and blurred and dimmed
// while the menu is in front of it.
//
// No EGL here: the caller makes a context current (the app: native/jni, on its render thread; the bench:
// native/tools/kp_render.cpp, off screen) and calls init(), then upload() / draw(), all on that one thread.
#pragma once
#include <cstdint>

namespace kp {

enum class Fit {
  Fit = 0,      // the whole picture, as large as fits its window (black bars where the shapes differ)
  Stretch = 1,  // the picture pulled to the window's shape
  Crop = 2,     // the window filled, the picture's overflow cut off
};

struct ViewOpts {
  float aspect = 4.f / 3;  // the shape the picture is shown at: 4:3, or 16:9 for a widescreen camera
  Fit fit = Fit::Fit;
  float scale = 1;         // the picture's size against what `fit` gives, about its window's centre
  bool stereo = false;     // two windows, the left and right halves of the viewport, the same picture in both
  float eye_shift = 0;     // stereo: each picture moved toward the middle of the viewport (+) or away (-), as a
                           // fraction of the viewport's width (lens spacing differs between goggles)
  float blur = 0;          // 0 = sharp ... 1 = the menu's backdrop: blurred and dimmed
  float gain = 1;          // picture brightness, 1 = as decoded (0: black, e.g. a stale picture faded out)
};

// Where a picture lands in a window, in pixels from the window's top left (may reach outside it: Crop,
// scale > 1). Pure arithmetic, exposed for tests.
struct Rect {
  float x, y, w, h;
};
Rect layout(float win_w, float win_h, float aspect, Fit fit, float scale);

class VideoRenderer {
public:
  // With a current ES 3 context. False (and a log line) if a shader does not build.
  bool init();
  // Frees the GL objects (context current), or forgets them (context already gone).
  void destroy(bool context_alive = true);
  // A new frame: 4 bytes per pixel, R G B X, top row first (kp::Frame).
  void upload(const uint8_t *rgbx, int width, int height);
  void clear();  // no picture: black until the next upload
  bool has_picture() const { return _have; }
  // The whole viewport, into framebuffer `target` (0 = the window surface).
  void draw(unsigned target, int view_w, int view_h, const ViewOpts &o);

private:
  struct Level {
    unsigned fbo = 0, tex = 0;
    int w = 0, h = 0;
  };
  static constexpr int kLevels = 6;  // blur pyramid: 1/2 ... 1/64 of the viewport
  void scene(int view_w, int view_h, const ViewOpts &o, bool bicubic);
  void ensure_levels(int view_w, int view_h);
  void free_levels();

  unsigned _prog_video = 0, _prog_down = 0, _prog_up = 0, _prog_mix = 0;
  unsigned _vbo = 0, _vao = 0, _tex = 0;
  int _tex_w = 0, _tex_h = 0;
  bool _have = false;
  Level _lv[kLevels];
  int _lv_view_w = 0, _lv_view_h = 0;
  // uniform locations
  int _u_v_rect = -1, _u_v_size = -1, _u_v_gain = -1, _u_v_bicubic = -1;
  int _u_d_half = -1, _u_u_half = -1, _u_m_alpha = -1, _u_m_dim = -1;
};

}  // namespace kp
