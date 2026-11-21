// kepyur: what the screen shows where there is no picture (no adapter, a scan, a channel with nothing on it, a
// picture that has stopped): colour bars, with the chosen channel ("R8") in a black box in the middle, or NO
// SIGNAL when no channel has been chosen.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kp {

// 4 bytes per pixel, R G B X, top row first (like kp::Frame). `words`: what the box says ("" = NO SIGNAL); band
// letters, the letters of NO SIGNAL, digits and spaces (anything else is left out). They keep their shape
// whatever w:h is; short words are drawn larger.
void no_signal_card(std::vector<uint8_t> &px, int w, int h, const std::string &words = "");

}  // namespace kp
