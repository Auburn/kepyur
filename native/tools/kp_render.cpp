// kp_render: the app's video renderer (native/render) off screen, for looking at what a view setting does
// without a phone. Needs EGL + OpenGL ES 3 (Mesa's software renderer will do).
//
//   kp_render OUT.ppm [--in FRAME.rgbx --size 720x480] [--view 2400x1080] [--aspect 4:3|16:9]
//             [--fit fit|stretch|crop] [--scale 1.0] [--stereo] [--eye 0.0] [--gain 1] [--nofeed] [--label R8]
//             [--inset 0..1 --box X,Y,W,H --radius R]
//
// FRAME.rgbx is one frame as kp_replay --frames writes them (the last 720 x H x 4 bytes of its output);
// without --in the test card is drawn (test_card.h: a circle shows the aspect ratio, a border what is cut off).
// --nofeed: no frame at all, what the screen shows then (NO SIGNAL, or with --label R8 the chosen channel);
// --gain below 1 cross-fades a frame into it.
// --inset 1 with --box: the picture in the menu's preview at X,Y (W x H pixels, corners of radius R); between 0
// and 1, on its way there.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "test_card.h"
#include "video_renderer.h"

namespace {

bool pair_of(const char *s, char sep, double &a, double &b) {
  const char *p = std::strchr(s, sep);
  if (!p) return false;
  a = std::atof(s);
  b = std::atof(p + 1);
  return a > 0 && b > 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: kp_render OUT.ppm [--in F.rgbx --size WxH] [--view WxH] [--aspect 4:3] "
                         "[--fit fit|stretch|crop] [--scale S] [--stereo] [--eye E] [--gain G] [--nofeed] [--label L] "
                         "[--inset T --box X,Y,W,H --radius R]\n");
    return 2;
  }
  const std::string out = argv[1];
  std::string in;
  int fw = 720, fh = 480, vw = 2400, vh = 1080;
  bool feed = true;
  std::string label;
  kp::ViewOpts o;
  for (int i = 2; i < argc; i++) {
    const std::string k = argv[i];
    auto v = [&] { return i + 1 < argc ? argv[++i] : (std::fprintf(stderr, "missing value\n"), std::exit(2), ""); };
    double a, b;
    if (k == "--in") in = v();
    else if (k == "--size" && pair_of(v(), 'x', a, b)) fw = static_cast<int>(a), fh = static_cast<int>(b);
    else if (k == "--view" && pair_of(v(), 'x', a, b)) vw = static_cast<int>(a), vh = static_cast<int>(b);
    else if (k == "--aspect" && pair_of(v(), ':', a, b)) o.aspect = static_cast<float>(a / b);
    else if (k == "--fit") {
      const std::string m = v();
      o.fit = m == "stretch" ? kp::Fit::Stretch : m == "crop" ? kp::Fit::Crop : kp::Fit::Fit;
    } else if (k == "--scale") o.scale = static_cast<float>(std::atof(v()));
    else if (k == "--stereo") o.stereo = true;
    else if (k == "--eye") o.eye_shift = static_cast<float>(std::atof(v()));
    else if (k == "--gain") o.gain = static_cast<float>(std::atof(v()));
    else if (k == "--nofeed") feed = false;
    else if (k == "--label") label = v();
    else if (k == "--inset") o.inset = static_cast<float>(std::atof(v()));
    else if (k == "--radius") o.radius = static_cast<float>(std::atof(v()));
    else if (k == "--box") {
      float b[4];
      if (std::sscanf(v(), "%f,%f,%f,%f", &b[0], &b[1], &b[2], &b[3]) != 4)
        return std::fprintf(stderr, "--box takes X,Y,W,H\n"), 2;
      o.box = {b[0], b[1], b[2], b[3]};
    } else {
      std::fprintf(stderr, "unknown or bad option %s\n", k.c_str());
      return 2;
    }
  }
  std::vector<uint8_t> frame;
  if (in.empty()) {
    kp::test_card(frame, fw, fh);
  } else {
    FILE *f = std::fopen(in.c_str(), "rb");
    if (!f) return std::perror(in.c_str()), 1;
    const size_t need = static_cast<size_t>(fw) * fh * 4;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    if (len < static_cast<long>(need)) return std::fprintf(stderr, "%s: shorter than one %dx%d frame\n", in.c_str(), fw, fh), 1;
    std::fseek(f, len - static_cast<long>(need), SEEK_SET);  // the last frame
    frame.resize(need);
    if (std::fread(frame.data(), 1, need, f) != need) return std::perror("read"), 1;
    std::fclose(f);
  }

  // an off-screen ES 3 context: Mesa's surfaceless platform where there is one, else the default display
  EGLDisplay dpy = EGL_NO_DISPLAY;
  const char *cext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  const auto gpd = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (cext && std::strstr(cext, "EGL_MESA_platform_surfaceless") && gpd)
    dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
  if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(dpy, nullptr, nullptr)) return std::fprintf(stderr, "no EGL display\n"), 1;
  const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
  EGLConfig cfg;
  EGLint n = 0;
  if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) return std::fprintf(stderr, "no ES 3 config\n"), 1;
  const EGLint pb_attr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  const EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
  const EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) return std::fprintf(stderr, "no ES 3 context\n"), 1;

  // the "screen": a framebuffer of the viewport's size
  GLuint fbo, tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vw, vh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

  kp::VideoRenderer r;
  if (!r.init()) return std::fprintf(stderr, "renderer init failed\n"), 1;
  r.set_card_words(label);
  if (feed) r.upload(frame.data(), fw, fh);
  r.draw(fbo, vw, vh, o);
  if (const GLenum e = glGetError(); e != GL_NO_ERROR) std::fprintf(stderr, "GL error 0x%x\n", e);

  std::vector<uint8_t> px(static_cast<size_t>(vw) * vh * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glReadPixels(0, 0, vw, vh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  FILE *f = std::fopen(out.c_str(), "wb");
  if (!f) return std::perror(out.c_str()), 1;
  std::fprintf(f, "P6\n%d %d\n255\n", vw, vh);
  for (int y = vh - 1; y >= 0; y--)  // GL reads bottom row first
    for (int x = 0; x < vw; x++) std::fwrite(&px[(static_cast<size_t>(y) * vw + x) * 4], 1, 3, f);
  std::fclose(f);
  r.destroy();
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(dpy, ctx);
  eglDestroySurface(dpy, surf);
  eglTerminate(dpy);
  std::fprintf(stderr, "%s: %dx%d\n", out.c_str(), vw, vh);
  return 0;
}
