// kepyur video renderer. See video_renderer.h.
#include "video_renderer.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "log.h"

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

// The picture. Bicubic (Catmull-Rom) when it is drawn sharp: a 480-line frame is enlarged 2-3 times on a phone,
// and plain bilinear filtering turns that into soft blocks. 9 taps: each pair of inner weights shares one
// bilinear fetch.
const char *kVideoFs = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_size;      // texture size in texels
uniform float u_gain;
uniform int u_bicubic;
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
  vec3 c = texture(u_tex, vec2(t0.x, t0.y)).rgb * (w0.x * w0.y)
         + texture(u_tex, vec2(t12.x, t0.y)).rgb * (w12.x * w0.y)
         + texture(u_tex, vec2(t3.x, t0.y)).rgb * (w3.x * w0.y)
         + texture(u_tex, vec2(t0.x, t12.y)).rgb * (w0.x * w12.y)
         + texture(u_tex, vec2(t12.x, t12.y)).rgb * (w12.x * w12.y)
         + texture(u_tex, vec2(t3.x, t12.y)).rgb * (w3.x * w12.y)
         + texture(u_tex, vec2(t0.x, t3.y)).rgb * (w0.x * w3.y)
         + texture(u_tex, vec2(t12.x, t3.y)).rgb * (w12.x * w3.y)
         + texture(u_tex, vec2(t3.x, t3.y)).rgb * (w3.x * w3.y);
  return clamp(c, 0.0, 1.0);
}
void main() {
  vec3 c = u_bicubic != 0 ? bicubic(v_uv) : texture(u_tex, v_uv).rgb;
  o_col = vec4(c * u_gain, 1.0);
}
)";

// The blur: a pyramid of half-size steps down and back up ("dual filtering"), 5 and 8 bilinear taps a step.
// Wide and smooth for a few small passes, where one big Gaussian would cost hundreds of taps a pixel.
const char *kDownFs = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_half;
out vec4 o_col;
void main() {
  vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);
  vec3 s = texture(u_tex, uv).rgb * 4.0;
  s += texture(u_tex, uv - u_half).rgb;
  s += texture(u_tex, uv + u_half).rgb;
  s += texture(u_tex, uv + vec2(u_half.x, -u_half.y)).rgb;
  s += texture(u_tex, uv - vec2(u_half.x, -u_half.y)).rgb;
  o_col = vec4(s / 8.0, 1.0);
}
)";

const char *kUpFs = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_half;
out vec4 o_col;
void main() {
  vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);
  vec3 s = texture(u_tex, uv + vec2(-u_half.x * 2.0, 0.0)).rgb;
  s += texture(u_tex, uv + vec2(-u_half.x, u_half.y)).rgb * 2.0;
  s += texture(u_tex, uv + vec2(0.0, u_half.y * 2.0)).rgb;
  s += texture(u_tex, uv + vec2(u_half.x, u_half.y)).rgb * 2.0;
  s += texture(u_tex, uv + vec2(u_half.x * 2.0, 0.0)).rgb;
  s += texture(u_tex, uv + vec2(u_half.x, -u_half.y)).rgb * 2.0;
  s += texture(u_tex, uv + vec2(0.0, -u_half.y * 2.0)).rgb;
  s += texture(u_tex, uv + vec2(-u_half.x, -u_half.y)).rgb * 2.0;
  o_col = vec4(s / 12.0, 1.0);
}
)";

// The blurred picture over the sharp one. A little noise (under one 8-bit step) keeps the dark, smooth
// gradients of a blurred picture from showing bands.
const char *kMixFs = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform float u_alpha;
uniform float u_dim;
out vec4 o_col;
void main() {
  vec3 c = texture(u_tex, vec2(v_uv.x, 1.0 - v_uv.y)).rgb * u_dim;
  float n = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
  o_col = vec4(c + n / 255.0, u_alpha);
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

void tex_params(GLint filter) {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

const float kWhole[4] = {-1, 1, 1, -1};  // u_rect for the full target

}  // namespace

bool VideoRenderer::init() {
  _prog_video = link(kQuadVs, kVideoFs);
  _prog_down = link(kQuadVs, kDownFs);
  _prog_up = link(kQuadVs, kUpFs);
  _prog_mix = link(kQuadVs, kMixFs);
  if (!_prog_video || !_prog_down || !_prog_up || !_prog_mix) return false;
  _u_v_rect = glGetUniformLocation(_prog_video, "u_rect");
  _u_v_size = glGetUniformLocation(_prog_video, "u_size");
  _u_v_gain = glGetUniformLocation(_prog_video, "u_gain");
  _u_v_bicubic = glGetUniformLocation(_prog_video, "u_bicubic");
  _u_d_half = glGetUniformLocation(_prog_down, "u_half");
  _u_u_half = glGetUniformLocation(_prog_up, "u_half");
  _u_m_alpha = glGetUniformLocation(_prog_mix, "u_alpha");
  _u_m_dim = glGetUniformLocation(_prog_mix, "u_dim");
  for (GLuint p : {_prog_down, _prog_up, _prog_mix}) {
    glUseProgram(p);
    glUniform4fv(glGetUniformLocation(p, "u_rect"), 1, kWhole);
  }
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
  _have = false;
  return true;
}

void VideoRenderer::free_levels() {
  for (Level &l : _lv) {
    if (l.fbo) glDeleteFramebuffers(1, &l.fbo);
    if (l.tex) glDeleteTextures(1, &l.tex);
    l = Level{};
  }
  _lv_view_w = _lv_view_h = 0;
}

void VideoRenderer::destroy(bool context_alive) {
  if (context_alive) {
    free_levels();
    for (GLuint p : {_prog_video, _prog_down, _prog_up, _prog_mix})
      if (p) glDeleteProgram(p);
    if (_vbo) glDeleteBuffers(1, &_vbo);
    if (_vao) glDeleteVertexArrays(1, &_vao);
    if (_tex) glDeleteTextures(1, &_tex);
  }
  for (Level &l : _lv) l = Level{};
  _lv_view_w = _lv_view_h = 0;
  _prog_video = _prog_down = _prog_up = _prog_mix = 0;
  _vbo = _vao = _tex = 0;
  _tex_w = _tex_h = 0;
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

void VideoRenderer::ensure_levels(int view_w, int view_h) {
  if (view_w == _lv_view_w && view_h == _lv_view_h && _lv[0].fbo) return;
  free_levels();
  for (int i = 0; i < kLevels; i++) {
    Level &l = _lv[i];
    l.w = std::max(1, view_w >> (i + 1));
    l.h = std::max(1, view_h >> (i + 1));
    glGenTextures(1, &l.tex);
    glBindTexture(GL_TEXTURE_2D, l.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, l.w, l.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    tex_params(GL_LINEAR);
    glGenFramebuffers(1, &l.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, l.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l.tex, 0);
  }
  _lv_view_w = view_w;
  _lv_view_h = view_h;
}

// The picture(s) into the bound framebuffer, whose size is view_w x view_h.
void VideoRenderer::scene(int view_w, int view_h, const ViewOpts &o, bool bicubic) {
  glUseProgram(_prog_video);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, _tex);
  glUniform2f(_u_v_size, static_cast<float>(_tex_w), static_cast<float>(_tex_h));
  glUniform1f(_u_v_gain, std::clamp(o.gain, 0.f, 1.f));
  glUniform1i(_u_v_bicubic, bicubic ? 1 : 0);
  glBindVertexArray(_vao);
  glEnable(GL_SCISSOR_TEST);
  const int windows = o.stereo ? 2 : 1;
  const float ww = static_cast<float>(view_w) / windows, wh = static_cast<float>(view_h);
  for (int i = 0; i < windows; i++) {
    const float wx = ww * i;
    Rect r = layout(ww, wh, o.aspect, o.fit, std::max(o.scale, 0.01f));
    r.x += wx;
    if (o.stereo) r.x += (i == 0 ? 1 : -1) * o.eye_shift * view_w;
    // each window keeps to itself: a cropped or enlarged picture must not run into the other eye's
    const int sx0 = static_cast<int>(std::lround(wx)), sx1 = static_cast<int>(std::lround(wx + ww));
    glScissor(sx0, 0, sx1 - sx0, view_h);
    const float rect[4] = {2 * r.x / view_w - 1, 1 - 2 * r.y / view_h, 2 * (r.x + r.w) / view_w - 1,
                           1 - 2 * (r.y + r.h) / view_h};
    glUniform4fv(_u_v_rect, 1, rect);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glDisable(GL_SCISSOR_TEST);
}

void VideoRenderer::draw(unsigned target, int view_w, int view_h, const ViewOpts &o) {
  if (view_w <= 0 || view_h <= 0) return;
  const float t = std::clamp(o.blur, 0.f, 1.f);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, target);
  glViewport(0, 0, view_w, view_h);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  if (!_prog_video || !_have || o.gain <= 0) return;
  if (t < 1) scene(view_w, view_h, o, true);
  if (t <= 0) return;

  // the same scene at half size, down the pyramid and back up, then over the sharp one
  ensure_levels(view_w, view_h);
  glBindFramebuffer(GL_FRAMEBUFFER, _lv[0].fbo);
  glViewport(0, 0, _lv[0].w, _lv[0].h);
  glClear(GL_COLOR_BUFFER_BIT);
  scene(_lv[0].w, _lv[0].h, o, false);
  glBindVertexArray(_vao);
  glActiveTexture(GL_TEXTURE0);
  const float spread = 1.f + t;  // the blur widens as it fades in
  glUseProgram(_prog_down);
  for (int i = 1; i < kLevels; i++) {
    const Level &src = _lv[i - 1], &dst = _lv[i];
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    glBindTexture(GL_TEXTURE_2D, src.tex);
    glUniform2f(_u_d_half, spread / src.w, spread / src.h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glUseProgram(_prog_up);
  for (int i = kLevels - 2; i >= 0; i--) {
    const Level &src = _lv[i + 1], &dst = _lv[i];
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    glBindTexture(GL_TEXTURE_2D, src.tex);
    glUniform2f(_u_u_half, 0.5f * spread / src.w, 0.5f * spread / src.h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, target);
  glViewport(0, 0, view_w, view_h);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(_prog_mix);
  glBindTexture(GL_TEXTURE_2D, _lv[0].tex);
  glUniform1f(_u_m_alpha, t);
  glUniform1f(_u_m_dim, 1.f - 0.42f * t);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisable(GL_BLEND);
}

}  // namespace kp
