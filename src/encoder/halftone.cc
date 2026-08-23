// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Atkinson (1980s, Bill Atkinson at Apple), serpentine:
//
//        *  1  1
//     1  1  1
//        1        / 8
//
// Only six eighths of the error is passed on; the remaining quarter is
// dropped. That is what gives Atkinson its crisper, higher-contrast look
// compared with Floyd–Steinberg, at the cost of flattening detail in the
// deepest shadows and brightest highlights.

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

float srgb_to_linear(uint8_t c) {
  const float v = c / 255.0f;
  return v <= 0.04045f ? v / 12.92f
                       : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float y) {
  y = std::clamp(y, 0.0f, 1.0f);
  return y <= 0.0031308f ? y * 12.92f
                         : 1.055f * std::pow(y, 1.0f / 2.4f) - 0.055f;
}

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
  // Weigh the channels in LINEAR light, then re-encode, which is what
  // ColorSync does when macOS hands the old CUPS path a grey raster. Doing
  // the sum on gamma-encoded values instead makes saturated colour far too
  // dark -- pure red came out at 65% coverage, near solid on paper -- and
  // the laser curve below is tuned for ColorSync-style greys, not for that.
  //
  // For a neutral input (r == g == b) the weights sum to 1 and the transfer
  // functions cancel, so greys pass through unchanged and device_gray_to_toner
  // keeps the exact behaviour the halftone was tuned against.
  const float y = linear_to_srgb(0.2126f * srgb_to_linear(r) +
                                 0.7152f * srgb_to_linear(g) +
                                 0.0722f * srgb_to_linear(b));
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

void nn_upsample_2x(std::vector<uint8_t>& toner, unsigned& width,
                    unsigned& height) {
  if (width == 0 || height == 0) {
    return;
  }
  const unsigned nw = width * 2;
  const unsigned nh = height * 2;
  std::vector<uint8_t> out(static_cast<size_t>(nw) * nh);
  for (unsigned y = 0; y < height; ++y) {
    const uint8_t* src = toner.data() + static_cast<size_t>(y) * width;
    uint8_t* dst0 = out.data() + static_cast<size_t>(2 * y) * nw;
    uint8_t* dst1 = dst0 + nw;
    for (unsigned x = 0; x < width; ++x) {
      const uint8_t v = src[x];
      dst0[2 * x] = dst0[2 * x + 1] = v;
      dst1[2 * x] = dst1[2 * x + 1] = v;
    }
  }
  toner.swap(out);
  width = nw;
  height = nh;
}

void scale_coverage(uint8_t* toner, size_t n, float gain) {
  if (!toner || gain == 1.0f) {
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    const int v =
        static_cast<int>(std::lround(static_cast<float>(toner[i]) * gain));
    toner[i] = static_cast<uint8_t>(std::clamp(v, 0, 255));
  }
}

void box_downsample_2x(std::vector<uint8_t>& toner, unsigned& width,
                       unsigned& height) {
  if (width < 2 || height < 2) {
    return;
  }
  const unsigned nw = width / 2;
  const unsigned nh = height / 2;
  std::vector<uint8_t> out(static_cast<size_t>(nw) * nh);
  for (unsigned y = 0; y < nh; ++y) {
    for (unsigned x = 0; x < nw; ++x) {
      const unsigned s =
          static_cast<unsigned>(toner[(2 * y) * width + (2 * x)]) +
          static_cast<unsigned>(toner[(2 * y) * width + (2 * x + 1)]) +
          static_cast<unsigned>(toner[(2 * y + 1) * width + (2 * x)]) +
          static_cast<unsigned>(toner[(2 * y + 1) * width + (2 * x + 1)]);
      out[static_cast<size_t>(y) * nw + x] =
          static_cast<uint8_t>((s + 2) / 4);
    }
  }
  toner.swap(out);
  width = nw;
  height = nh;
}

void atkinson(uint8_t* toner, unsigned width, unsigned height) {
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
    // Serpentine, as before: the kernel is mirrored on right-to-left rows so
    // the diffusion stays symmetric and does not build directional worms.
    const int dir = (y % 2 == 0) ? 1 : -1;
    const int x0 = (dir > 0) ? 0 : static_cast<int>(width) - 1;
    const int x1 = (dir > 0) ? static_cast<int>(width) : -1;
    for (int x = x0; x != x1; x += dir) {
      int& p = buf[static_cast<size_t>(y) * width + static_cast<unsigned>(x)];
      const int old = p;
      const int neu = old >= 128 ? 255 : 0;
      p = neu;
      const int err = (old - neu) / 8;
      const int yi = static_cast<int>(y);
      add(x + dir, yi, err);
      add(x + 2 * dir, yi, err);
      add(x - dir, yi + 1, err);
      add(x, yi + 1, err);
      add(x + dir, yi + 1, err);
      add(x, yi + 2, err);
    }
  }

  for (size_t i = 0; i < buf.size(); ++i) {
    toner[i] = buf[i] >= 128 ? 255 : 0;
  }
}

}  // namespace sisterhl2030
