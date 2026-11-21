// kepyur: the NO SIGNAL card. See no_signal.h.
#include "no_signal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>

namespace kp {
namespace {

struct Pt {
  float x, y;
};

// A letter: how wide its strokes' centre lines reach (in cap heights) and the strokes, each a polyline.
struct Glyph {
  float w;
  std::vector<std::vector<Pt>> strokes;
};

constexpr float kStroke = 0.17f;  // stroke width, in cap heights
constexpr float kGap = 0.24f;     // between two letters, stroke edge to stroke edge
constexpr float kSpace = 0.34f;   // a space, on top of the gaps either side of it

// Points along an ellipse from angle a0 to a1, in degrees (0 = right, 90 = down).
std::vector<Pt> arc(float cx, float cy, float rx, float ry, float a0, float a1) {
  const int n = std::max(2, static_cast<int>(std::ceil(std::fabs(a1 - a0) / 7.5f)) + 1);
  std::vector<Pt> out;
  for (int i = 0; i < n; i++) {
    const float a = (a0 + (a1 - a0) * i / (n - 1)) * 3.14159265f / 180;
    out.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
  }
  return out;
}

// Polylines one after the other, as one.
std::vector<Pt> join(std::initializer_list<std::vector<Pt>> parts) {
  std::vector<Pt> out;
  for (const auto &p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

// What the card can say, in a plain geometric sans: the letters of NO SIGNAL and of the bands (A B E F L R), and
// the digits. Cap height 1, y down, the strokes' centre lines.
Glyph glyph(char c) {
  switch (c) {
    case 'R':
      return {0.58f, {join({{{0, 1}, {0, 0}, {0.3f, 0}}, arc(0.3f, 0.26f, 0.26f, 0.26f, -90, 90), {{0, 0.52f}}}),
                      {{0.24f, 0.52f}, {0.58f, 1}}}};
    case 'B':
      return {0.56f, {join({{{0, 1}, {0, 0}, {0.26f, 0}}, arc(0.26f, 0.24f, 0.24f, 0.24f, -90, 90), {{0, 0.48f}}}),
                      join({{{0, 0.48f}, {0.28f, 0.48f}}, arc(0.28f, 0.74f, 0.26f, 0.26f, -90, 90), {{0, 1}}})}};
    case 'E':
      return {0.5f, {{{0.5f, 0}, {0, 0}, {0, 1}, {0.5f, 1}}, {{0, 0.5f}, {0.42f, 0.5f}}}};
    case 'F':
      return {0.48f, {{{0.48f, 0}, {0, 0}, {0, 1}}, {{0, 0.5f}, {0.4f, 0.5f}}}};
    case '0':
      return {0.56f, {arc(0.28f, 0.5f, 0.28f, 0.5f, 0, 360)}};
    case '1':
      return {0.3f, {{{0, 0.2f}, {0.3f, 0}, {0.3f, 1}}}};
    case '2':
      return {0.56f, {join({arc(0.28f, 0.28f, 0.28f, 0.28f, 200, 400), {{0, 1}, {0.56f, 1}}})}};
    case '3':
      return {0.54f, {arc(0.26f, 0.25f, 0.26f, 0.25f, 210, 450), arc(0.27f, 0.74f, 0.27f, 0.26f, -90, 150),
                      {{0.14f, 0.49f}, {0.27f, 0.49f}}}};
    case '4':
      return {0.6f, {{{0.44f, 1}, {0.44f, 0}, {0, 0.68f}, {0.6f, 0.68f}}}};
    case '5':
      return {0.56f, {join({{{0.52f, 0}, {0.08f, 0}, {0.04f, 0.5f}}, arc(0.28f, 0.69f, 0.28f, 0.31f, -145, 150)})}};
    case '6':
      return {0.56f, {arc(0.28f, 0.69f, 0.28f, 0.31f, 0, 360), arc(0.3f, 0.5f, 0.3f, 0.5f, 290, 165)}};
    case '7':
      return {0.54f, {{{0, 0}, {0.54f, 0}, {0.16f, 1}}}};
    case '8':
      return {0.56f, {arc(0.28f, 0.25f, 0.24f, 0.25f, 0, 360), arc(0.28f, 0.74f, 0.28f, 0.26f, 0, 360)}};
    case '9':
      return {0.56f, {arc(0.28f, 0.31f, 0.28f, 0.31f, 0, 360), arc(0.26f, 0.5f, 0.3f, 0.5f, 110, -15)}};
    case 'N':
      return {0.62f, {{{0, 1}, {0, 0}, {0.62f, 1}, {0.62f, 0}}}};
    case 'O':
      return {0.74f, {arc(0.37f, 0.5f, 0.37f, 0.5f, 0, 360)}};
    case 'S': {
      // two bowls, the upper drawn from its top right round to the middle, the lower on from there
      const float w = 0.56f;
      std::vector<Pt> s = arc(w / 2, 0.25f, w / 2, 0.25f, 330, 90);
      const std::vector<Pt> lower = arc(w / 2, 0.75f, w / 2, 0.25f, -90, 150);
      s.insert(s.end(), lower.begin() + 1, lower.end());
      return {w, {s}};
    }
    case 'I':
      return {0, {{{0, 0}, {0, 1}}}};
    case 'G': {
      const float w = 0.72f;
      std::vector<Pt> g = arc(w / 2, 0.5f, w / 2, 0.5f, 318, 0);
      g.push_back({w * 0.56f, 0.5f});
      return {w, {g}};
    }
    case 'A':
      return {0.7f, {{{0, 1}, {0.35f, 0}, {0.7f, 1}}, {{0.7f * 0.17f, 0.66f}, {0.7f * 0.83f, 0.66f}}}};
    case 'L':
      return {0.5f, {{{0, 0}, {0, 1}, {0.5f, 1}}}};
    default:
      return {-1, {}};  // not one of ours
  }
}

float segment_distance(float x, float y, Pt a, Pt b) {
  const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
  const float t = l2 > 0 ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / l2, 0.f, 1.f) : 0.f;
  const float ex = x - (a.x + t * dx), ey = y - (a.y + t * dy);
  return std::sqrt(ex * ex + ey * ey);
}

struct Placed {
  float x;  // where its centre lines start, in cap heights from the text's left edge
  Glyph g;
};

void put(uint8_t *p, float r, float g, float b) {
  p[0] = static_cast<uint8_t>(std::lround(std::clamp(r, 0.f, 1.f) * 255));
  p[1] = static_cast<uint8_t>(std::lround(std::clamp(g, 0.f, 1.f) * 255));
  p[2] = static_cast<uint8_t>(std::lround(std::clamp(b, 0.f, 1.f) * 255));
}

}  // namespace

void no_signal_card(std::vector<uint8_t> &px, int w, int h, const std::string &words) {
  px.assign(static_cast<size_t>(std::max(w, 0)) * std::max(h, 0) * 4, 255);
  if (w <= 0 || h <= 0) return;

  // Colour bars as a studio test signal has them: seven at 75 %, a strip of the same reversed, and along the
  // bottom -I, white, +Q, black and the three near-blacks of a PLUGE.
  static const float top[7][3] = {{.75f, .75f, .75f}, {.75f, .75f, 0}, {0, .75f, .75f}, {0, .75f, 0},
                                  {.75f, 0, .75f},    {.75f, 0, 0},    {0, 0, .75f}};
  static const float mid[7][3] = {{0, 0, .75f},   {.075f, .075f, .075f}, {.75f, 0, .75f},   {.075f, .075f, .075f},
                                  {0, .75f, .75f}, {.075f, .075f, .075f}, {.75f, .75f, .75f}};
  struct Band {
    float to;  // right edge, in bar widths
    float c[3];
  };
  static const Band low[] = {{1.25f, {0, .24f, .40f}}, {2.5f, {1, 1, 1}},          {3.75f, {.25f, 0, .46f}},
                             {5.f, {.075f, .075f, .075f}}, {5.f + 1 / 3.f, {.035f, .035f, .035f}},
                             {5.f + 2 / 3.f, {.075f, .075f, .075f}}, {6.f, {.115f, .115f, .115f}},
                             {7.f, {.075f, .075f, .075f}}};
  for (int y = 0; y < h; y++) {
    const float v = (y + 0.5f) / h;
    for (int x = 0; x < w; x++) {
      const float u = (x + 0.5f) / w * 7;
      const float *c;
      if (v < 0.75f) {
        const int i = std::min(6, static_cast<int>(u));
        c = v < 0.67f ? top[i] : mid[i];
      } else {
        size_t i = 0;
        while (i + 1 < std::size(low) && u >= low[i].to) i++;
        c = low[i].c;
      }
      put(&px[(static_cast<size_t>(y) * w + x) * 4], c[0], c[1], c[2]);
    }
  }

  // the words: laid out in cap heights, then scaled to the card
  std::vector<Placed> text;
  float x = 0;
  bool space = false;
  for (const char c : words.empty() ? std::string("NO SIGNAL") : words) {
    Glyph g = glyph(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (g.w < 0) {  // a space, or something there is no letter for
      space = !text.empty();
      continue;
    }
    if (!text.empty()) x += space ? kGap + kSpace : kGap;
    space = false;
    text.push_back({x + kStroke / 2, std::move(g)});
    x += text.back().g.w + kStroke;
  }
  if (text.empty()) return;  // nothing we can draw: the bars alone
  // at most 62 % of the card's width, capitals at most 15 % of its height (a channel's name, being short, is
  // drawn larger than NO SIGNAL)
  const float wide = x;                                          // in cap heights
  const float cap = std::min(0.15f * h, 0.62f * w / wide);      // px
  const float tw = wide * cap, th = (1 + kStroke) * cap;
  const float ox = (w - tw) / 2, oy = (h - cap) / 2;             // the cap line's left end

  // a black box round them
  const int bx0 = std::max(0, static_cast<int>(std::lround(ox - 0.62f * cap)));
  const int bx1 = std::min(w, static_cast<int>(std::lround(ox + tw + 0.62f * cap)));
  const int by0 = std::max(0, static_cast<int>(std::lround((h - th) / 2 - 0.5f * cap)));
  const int by1 = std::min(h, static_cast<int>(std::lround((h + th) / 2 + 0.5f * cap)));
  for (int y = by0; y < by1; y++)
    for (int x = bx0; x < bx1; x++) {
      // the letters, white, their edges smoothed over a pixel
      const float gu = (x + 0.5f - ox) / cap, gv = (y + 0.5f - oy) / cap;
      float d = 1e9f;
      for (const Placed &p : text) {
        if (gu < p.x - kStroke || gu > p.x + p.g.w + kStroke) continue;
        for (const auto &line : p.g.strokes)
          for (size_t i = 1; i < line.size(); i++)
            d = std::min(d, segment_distance(gu - p.x, gv, line[i - 1], line[i]));
      }
      const float ink = std::clamp((kStroke / 2 - d) * cap + 0.5f, 0.f, 1.f);
      const float l = 0.02f + (0.96f - 0.02f) * ink;
      put(&px[(static_cast<size_t>(y) * w + x) * 4], l, l, l);
    }
}

}  // namespace kp
