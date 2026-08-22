// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Floyd–Steinberg (1976), serpentine:
//
//     *  7
//   3  5  1     / 16

#include "encoder/halftone.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sisterhl2030 {
namespace {

// Perceptual luma (gamma-encoded sRGB), then a variable laser curve:
// shadows/midtones use gamma 1.8 (dot gain + toner save — mid-grey was
// ~75% in linear light), but highlights stay near gamma 1.0 so pastel
// blues keep a visible dither instead of washing out to paper.
constexpr float kHighlightGamma = 1.0f;
constexpr float kShadowGamma = 1.8f;
constexpr float kGammaBlendStart = 0.12f;
constexpr float kGammaBlendSpan = 0.55f;

uint8_t coverage_from_blackness(float blackness) {
  blackness = std::clamp(blackness, 0.0f, 1.0f);
  float t = (blackness - kGammaBlendStart) / kGammaBlendSpan;
  t = std::clamp(t, 0.0f, 1.0f);
  t = t * t * (3.0f - 2.0f * t);
  const float gamma = kHighlightGamma + (kShadowGamma - kHighlightGamma) * t;
  const float coverage = std::pow(blackness, gamma);
  const int v = static_cast<int>(std::lround(coverage * 255.0f));
  return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

}  // namespace

uint8_t rgb_to_toner(uint8_t r, uint8_t g, uint8_t b) {
  const float y =
      (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f;
  return coverage_from_blackness(1.0f - y);
}

uint8_t device_gray_to_toner(uint8_t gray) {
  return rgb_to_toner(gray, gray, gray);
}

uint8_t device_k_to_toner(uint8_t k) {
  return coverage_from_blackness(static_cast<float>(k) / 255.0f);
}

void pack_toner_row(const uint8_t* toner_row, unsigned width, uint8_t* packed) {
  const unsigned bpl = (width + 7) / 8;
  std::fill(packed, packed + bpl, 0);
  for (unsigned x = 0; x < width; ++x) {
    if (toner_row[x] >= 128) {
      packed[x / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
    }
  }
}

void floyd_steinberg(uint8_t* toner, unsigned width, unsigned height) {
  if (width == 0 || height == 0) {
    return;
  }
  std::vector<int> buf(static_cast<size_t>(width) * height);
  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = toner[i];
  }

  auto add = [&](int x, int y, int delta) {
    if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
        y >= static_cast<int>(height)) {
      return;
    }
    buf[static_cast<size_t>(y) * width + static_cast<unsigned>(x)] += delta;
  };

  for (unsigned y = 0; y < height; ++y) {
    const int dir = (y % 2 == 0) ? 1 : -1;
    const int x0 = (dir > 0) ? 0 : static_cast<int>(width) - 1;
    const int x1 = (dir > 0) ? static_cast<int>(width) : -1;
    for (int x = x0; x != x1; x += dir) {
      int& p = buf[static_cast<size_t>(y) * width + static_cast<unsigned>(x)];
      const int old = p;
      const int neu = old >= 128 ? 255 : 0;
      p = neu;
      const int err = old - neu;
      add(x + dir, static_cast<int>(y), err * 7 / 16);
      add(x - dir, static_cast<int>(y) + 1, err * 3 / 16);
      add(x, static_cast<int>(y) + 1, err * 5 / 16);
      add(x + dir, static_cast<int>(y) + 1, err / 16);
    }
  }

  for (size_t i = 0; i < buf.size(); ++i) {
    toner[i] = buf[i] >= 128 ? 255 : 0;
  }
}

}  // namespace sisterhl2030
