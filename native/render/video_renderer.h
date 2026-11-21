// kepyur: the picture on screen (OpenGL ES 3). Draws the decoder's frames into a viewport: laid out by aspect
// ratio, fit mode and scale, once or twice side by side (goggles that hold a phone), or small in the menu's
// preview with black all round it. Where there is no picture, the NO SIGNAL card (no_signal.h) takes its place.
//
// No EGL here: the caller makes a context current (the app: native/jni, on its render thread; the bench:
// native/tools/kp_render.cpp, off screen) and calls init(), then upload() / draw(), all on that one thread.
#pragma once
#include <cstdint>
#include <string>

namespace kp {

enum class Fit {
  Fit = 0,      // the whole picture, as large as fits its window (black bars where the shapes differ)
  Stretch = 1,  // the picture pulled to the window's shape
  Crop = 2,     // the window filled, the picture's overflow cut off
};

// A rectangle in pixels from the viewport's top left.
struct Rect {
  float x, y, w, h;
};

struct ViewOpts {
  float aspect = 4.f / 3;  // the shape the picture is shown at: 4:3, or 16:9 for a widescreen camera
  Fit fit = Fit::Fit;
  float scale = 1;         // the picture's size against what `fit` gives, about its window's centre
  bool stereo = false;     // two windows, the left and right halves of the viewport, the same picture in both
  float eye_shift = 0;     // stereo: each picture moved toward the middle of the viewport (+) or away (-), as a
                           // fraction of the viewport's width (lens spacing differs between goggles)
  float inset = 0;         // 0 = the picture where the settings above put it ... 1 = in `box` (the menu's
                           // preview), laid out there by aspect and fit alone, black all round; in between it moves.
                           // The NO SIGNAL card is dimmed there, under the menu's status.
  Rect box{0, 0, 0, 0};    // the preview
  float radius = 0;        // the preview's corner radius, pixels
  float gain = 1;          // how much of the picture: 1 = all of it, 0 = none (the NO SIGNAL card in its place,
                           // e.g. a stale picture faded out), in between the two cross-faded
  float bright = 1;        // the picture's own brightness (a stale one, held over a reopen, is dimmed)
};

// Where a picture lands in a window, in pixels from the window's top left (may reach outside it: Crop,
// scale > 1). Pure arithmetic, exposed for tests.
Rect layout(float win_w, float win_h, float aspect, Fit fit, float scale);

class VideoRenderer {
public:
  // With a current ES 3 context. False (and a log line) if a shader does not build.
  bool init();
  // Frees the GL objects (context current), or forgets them (context already gone).
  void destroy(bool context_alive = true);
  // A new frame: 4 bytes per pixel, R G B X, top row first (kp::Frame).
  void upload(const uint8_t *rgbx, int width, int height);
  void clear();  // no picture: the NO SIGNAL card until the next upload
  bool has_picture() const { return _have; }
  // What the card where there is no picture says: the chosen channel ("R8"), or "" for NO SIGNAL.
  void set_card_words(const std::string &words) { _words = words; }
  // The whole viewport, into framebuffer `target` (0 = the window surface).
  void draw(unsigned target, int view_w, int view_h, const ViewOpts &o);

private:
  void ensure_card(float aspect);
  void quad(unsigned tex, int tex_w, int tex_h, float gain, float alpha, bool bicubic);

  unsigned _prog = 0;
  unsigned _vbo = 0, _vao = 0, _tex = 0, _card = 0;
  int _tex_w = 0, _tex_h = 0, _card_w = 0, _card_h = 0;
  std::string _words, _card_words;  // what the card should say, and what the one made says
  bool _have = false;
  // uniform locations
  int _u_rect = -1, _u_size = -1, _u_gain = -1, _u_alpha = -1, _u_bicubic = -1, _u_clip = -1, _u_radius = -1;
};

}  // namespace kp
