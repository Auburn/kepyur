// kepyur video renderer. See video_renderer.h.
#include "video_renderer.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "log.h"
#include "no_signal.h"

namespace kp {

Rect layout(float win_w, float win_h, float aspect, Fit fit, float scale) {
  float w = win_w, h = win_h;
  if (fit != Fit::Stretch && win_w > 0 && win_h > 0 && aspect > 0) {
    const bool window_wider = win_w / win_h > aspect;
    // Fit: the window's short side limits the picture; Crop: its long side does
    if ((fit == Fit::Fit) == window_wider) w = win_h * aspect;
    else h = win_w / aspect;
  }
  w *= scale;
  h *= scale;
  return {(win_w - w) / 2, (win_h - h) / 2, w, h};
}

namespace {

const char *kQuadVs = R"(#version 300 es
layout(location = 0) in vec2 a_pos;   // 0..1, y down
uniform vec4 u_rect;                  // left, top, right, bottom in clip space
out vec2 v_uv;
void main() {
  v_uv = a_pos;
  gl_Position = vec4(mix(u_rect.x, u_rect.z, a_pos.x), mix(u_rect.y, u_rect.w, a_pos.y), 0.0, 1.0);
}
)";

// The picture (or the NO SIGNAL card). Bicubic (Catmull-Rom) where it is enlarged: a 480-line frame is enlarged
// 2-3 times on a phone, and plain bilinear filtering turns that into soft blocks. 9 taps: each pair of inner
// weights shares one bilinear fetch, all from the full-size level (the card has smaller ones, for the preview).
// Its window may have round corners (the menu's preview): outside them nothing, along them a pixel's blend.
// Premultiplied: u_alpha 1 lays it over what is there, 0 adds it (the second half of a cross-fade).
const char *kVideoFs = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_size;      // texture size in texels
uniform float u_gain;
uniform float u_alpha;
uniform int u_bicubic;
uniform vec4 u_clip;      // the window, in framebuffer pixels from the bottom left: x0 y0 x1 y1
uniform float u_radius;   // its corners' radius, pixels
out vec4 o_col;
vec3 bicubic(vec2 uv) {
  vec2 pos = uv * u_size;
  vec2 p1 = floor(pos - 0.5) + 0.5;
  vec2 f = pos - p1;
  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  vec2 w3 = f * f * (-0.5 + 0.5 * f);
  vec2 w12 = w1 + w2;
  vec2 t0 = (p1 - 1.0) / u_size;
  vec2 t3 = (p1 + 2.0) / u_size;
  vec2 t12 = (p1 + w2 / w12) / u_size;
  vec3 c = textureLod(u_tex, vec2(t0.x, t0.y), 0.0).rgb * (w0.x * w0.y)
         + textureLod(u_tex, vec2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y)
         + textureLod(u_tex, vec2(t3.x, t0.y), 0.0).rgb * (w3.x * w0.y)
         + textureLod(u_tex, vec2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y)
         + textureLod(u_tex, vec2(t12.x, t12.y), 0.0).rgb * (w12.x * w12.y)
         + textureLod(u_tex, vec2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y)
         + textureLod(u_tex, vec2(t0.x, t3.y), 0.0).rgb * (w0.x * w3.y)
         + textureLod(u_tex, vec2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y)
         + textureLod(u_tex, vec2(t3.x, t3.y), 0.0).rgb * (w3.x * w3.y);
  return clamp(c, 0.0, 1.0);
}
float inside() {
  if (u_radius <= 0.0) return 1.0;
  vec2 hs = 0.5 * (u_clip.zw - u_clip.xy);
  vec2 q = abs(gl_FragCoord.xy - 0.5 * (u_clip.xy + u_clip.zw)) - hs + u_radius;
  return clamp(0.5 - (length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_radius), 0.0, 1.0);
}
void main() {
  float a = inside();
  if (a <= 0.0) discard;
  vec3 c = u_bicubic != 0 ? bicubic(v_uv) : texture(u_tex, v_uv).rgb;
  o_col = vec4(c * (u_gain * a), u_alpha * a);
}
)";

GLuint compile(GLenum type, const char *src) {
  const GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024] = {};
    glGetShaderInfoLog(s, sizeof log - 1, nullptr, log);
    KP_LOGE("render: %s shader: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint link(const char *vs, const char *fs) {
  const GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  GLuint p = 0;
  if (v && f) {
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[1024] = {};
      glGetProgramInfoLog(p, sizeof log - 1, nullptr, log);
      KP_LOGE("render: link: %s", log);
      glDeleteProgram(p);
      p = 0;
    }
  }
  if (v) glDeleteShader(v);
  if (f) glDeleteShader(f);
  return p;
}

void tex_params(GLint min_filter) {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

Rect lerp(const Rect &a, const Rect &b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
}

}  // namespace

bool VideoRenderer::init() {
  _prog = link(kQuadVs, kVideoFs);
  if (!_prog) return false;
  _u_rect = glGetUniformLocation(_prog, "u_rect");
  _u_size = glGetUniformLocation(_prog, "u_size");
  _u_gain = glGetUniformLocation(_prog, "u_gain");
  _u_alpha = glGetUniformLocation(_prog, "u_alpha");
  _u_bicubic = glGetUniformLocation(_prog, "u_bicubic");
  _u_clip = glGetUniformLocation(_prog, "u_clip");
  _u_radius = glGetUniformLocation(_prog, "u_radius");
  static const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
  glGenVertexArrays(1, &_vao);
  glBindVertexArray(_vao);
  glGenBuffers(1, &_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, _vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glGenTextures(1, &_tex);
  glBindTexture(GL_TEXTURE_2D, _tex);
  tex_params(GL_LINEAR);
  _tex_w = _tex_h = 0;
  _card = 0;
  _card_w = _card_h = 0;
  _have = false;
  return true;
}

void VideoRenderer::destroy(bool context_alive) {
  if (context_alive) {
    if (_prog) glDeleteProgram(_prog);
    if (_vbo) glDeleteBuffers(1, &_vbo);
    if (_vao) glDeleteVertexArrays(1, &_vao);
    if (_tex) glDeleteTextures(1, &_tex);
    if (_card) glDeleteTextures(1, &_card);
  }
  _prog = 0;
  _vbo = _vao = _tex = _card = 0;
  _tex_w = _tex_h = _card_w = _card_h = 0;
  _card_words.clear();
  _have = false;
}

void VideoRenderer::upload(const uint8_t *rgbx, int width, int height) {
  if (!_tex || !rgbx || width <= 0 || height <= 0) return;
  glBindTexture(GL_TEXTURE_2D, _tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  if (width != _tex_w || height != _tex_h) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbx);
    _tex_w = width;
    _tex_h = height;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbx);
  }
  _have = true;
}

void VideoRenderer::clear() { _have = false; }

// The NO SIGNAL card at the picture's shape (its words keep theirs), made on the CPU once per shape and per
// channel: 720 lines, with smaller levels for the preview, where it is shown at a fraction of that.
void VideoRenderer::ensure_card(float aspect) {
  const int h = 720, w = std::clamp(static_cast<int>(std::lround(h * std::max(aspect, 0.1f))), 360, 1920);
  if (_card && w == _card_w && h == _card_h && _words == _card_words) return;
  std::vector<uint8_t> px;
  no_signal_card(px, w, h, _words);
  _card_words = _words;
  if (!_card) glGenTextures(1, &_card);
  glBindTexture(GL_TEXTURE_2D, _card);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glGenerateMipmap(GL_TEXTURE_2D);
  tex_params(GL_LINEAR_MIPMAP_LINEAR);
  _card_w = w;
  _card_h = h;
}

// One texture into the window set up by draw() (rect, scissor, clip and radius already given).
void VideoRenderer::quad(unsigned tex, int tex_w, int tex_h, float gain, float alpha, bool bicubic) {
  glBindTexture(GL_TEXTURE_2D, tex);
  glUniform2f(_u_size, static_cast<float>(tex_w), static_cast<float>(tex_h));
  glUniform1f(_u_gain, gain);
  glUniform1f(_u_alpha, alpha);
  glUniform1i(_u_bicubic, bicubic ? 1 : 0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void VideoRenderer::draw(unsigned target, int view_w, int view_h, const ViewOpts &o) {
  if (view_w <= 0 || view_h <= 0) return;
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, target);
  glViewport(0, 0, view_w, view_h);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  if (!_prog) return;
  // how much of the picture, and of the card in its place
  const float g = _have ? std::clamp(o.gain, 0.f, 1.f) : 0.f;
  if (g < 1) ensure_card(o.aspect);
  const bool card = g < 1 && _card;
  if (!card && g <= 0) return;

  const float t = std::clamp(o.inset, 0.f, 1.f);
  // in the preview the card is dimmed, so that the menu's status reads over it
  constexpr float kCardInPreview = 0.4f;
  const float card_level = 1 - (1 - kCardInPreview) * t;
  const float vw = static_cast<float>(view_w), vh = static_cast<float>(view_h);
  glUseProgram(_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(_vao);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_SCISSOR_TEST);
  // in the preview: the picture as the screen would show it at its own size, one of it
  Rect small = layout(o.box.w, o.box.h, o.aspect, o.fit, 1);
  small.x += o.box.x;
  small.y += o.box.y;
  const int windows = o.stereo ? 2 : 1;
  const float ww = vw / windows;
  for (int i = 0; i < windows; i++) {
    const float wx = ww * i;
    Rect r = layout(ww, vh, o.aspect, o.fit, std::max(o.scale, 0.01f));
    r.x += wx;
    if (o.stereo) r.x += (i == 0 ? 1 : -1) * o.eye_shift * vw;
    // each window keeps to itself (a cropped or enlarged picture must not run into the other eye's), and on the
    // way into the preview, it and the picture in it move there together
    const Rect win = lerp({wx, 0, ww, vh}, o.box, t);
    r = lerp(r, small, t);
    const long sx0 = std::lround(win.x), sx1 = std::lround(win.x + win.w);
    const long sy0 = std::lround(vh - win.y - win.h), sy1 = std::lround(vh - win.y);
    if (sx1 <= sx0 || sy1 <= sy0 || r.w <= 0 || r.h <= 0) continue;
    glScissor(static_cast<GLint>(sx0), static_cast<GLint>(sy0), static_cast<GLsizei>(sx1 - sx0),
              static_cast<GLsizei>(sy1 - sy0));
    glUniform4f(_u_clip, win.x, vh - win.y - win.h, win.x + win.w, vh - win.y);
    glUniform1f(_u_radius, std::min(o.radius * t, 0.5f * std::min(win.w, win.h)));
    const float rect[4] = {2 * r.x / vw - 1, 1 - 2 * r.y / vh, 2 * (r.x + r.w) / vw - 1, 1 - 2 * (r.y + r.h) / vh};
    glUniform4fv(_u_rect, 1, rect);
    // the card where the picture is not (bicubic only where it is enlarged: smaller, its levels do better), then
    // the picture over it, or added to it while the two cross-fade
    if (card) quad(_card, _card_w, _card_h, (1 - g) * card_level, 1, r.w >= _card_w);
    if (g > 0) quad(_tex, _tex_w, _tex_h, g * std::clamp(o.bright, 0.f, 1.f), card ? 0 : 1, true);
  }
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
}

}  // namespace kp
