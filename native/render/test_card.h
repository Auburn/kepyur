// kepyur: a test picture, for looking at the screen side of the app without a transmitter (the app's
// "testcard" extra) and for kp_render. A sky-to-ground gradient with a grid, so sharpness and scaling are easy to
// judge; a circle as a 4:3 camera would see one (round only when the picture is shown at 4:3); a red line
// around the very edge (what a crop cuts off); and a bar that moves with `tick`, so a frozen picture shows.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace kp {

// 4 bytes per pixel, R G B X, top row first (like kp::Frame).
inline void test_card(std::vector<uint8_t> &px, int w, int h, int tick = 0) {
  px.assign(static_cast<size_t>(w) * h * 4, 255);
  const double bar = std::fmod(tick * 0.01, 1.0);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint8_t *p = &px[(static_cast<size_t>(y) * w + x) * 4];
      const double u = (x + 0.5) / w, v = (y + 0.5) / h;
      double r = 0.20 + 0.5 * v, g = 0.35 + 0.3 * (1 - v), b = 0.75 * (1 - v) + 0.1;
      if (std::fmod(u * 12, 1.0) < 0.04 || std::fmod(v * 9, 1.0) < 0.05) r = g = b = 0.92;
      const double dx = (u - 0.5) * 4 / 3, dy = v - 0.5, d = std::sqrt(dx * dx + dy * dy);
      if (std::fabs(d - 0.36) < 0.012) r = 1, g = 0.85, b = 0.1;
      if (tick > 0 && v > 0.9 && v < 0.96 && std::fabs(u - bar) < 0.03) r = g = b = 1;
      if (x < 6 || y < 4 || x >= w - 6 || y >= h - 4) r = 1, g = 0.1, b = 0.1;
      p[0] = static_cast<uint8_t>(r * 255);
      p[1] = static_cast<uint8_t>(g * 255);
      p[2] = static_cast<uint8_t>(b * 255);
    }
}

}  // namespace kp
