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
#include <memory>
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

void pack_toner_row_2x(const uint8_t* toner_row, unsigned width,
                       uint8_t* packed) {
  const unsigned out_w = width * 2;
  const unsigned bpl = (out_w + 7) / 8;
  std::fill(packed, packed + bpl, 0);
  for (unsigned x = 0; x < width; ++x) {
    if (toner_row[x] >= 128) {
      const unsigned x0 = 2 * x;
      packed[x0 / 8] |= static_cast<uint8_t>(0x80 >> (x0 % 8));
      packed[(x0 + 1) / 8] |= static_cast<uint8_t>(0x80 >> ((x0 + 1) % 8));
    }
  }
}

void atkinson(uint8_t* toner, unsigned width, unsigned height) {
  if (width == 0 || height == 0) {
    return;
  }
  // Same kernel as a full-page int buffer, but only three rows of error:
  // current, y+1, y+2. Atkinson never looks further ahead.
  std::vector<int> rows(static_cast<size_t>(width) * 3, 0);
  int* r[3] = {rows.data(), rows.data() + width, rows.data() + 2 * width};

  auto load = [&](int* dest, unsigned y) {
    if (y >= height) {
      std::fill(dest, dest + width, 0);
      return;
    }
    const uint8_t* src = toner + static_cast<size_t>(y) * width;
    for (unsigned x = 0; x < width; ++x) {
      dest[x] = src[x];
    }
  };
  load(r[0], 0);
  load(r[1], 1);
  load(r[2], 2);

  const int w = static_cast<int>(width);
  for (unsigned y = 0; y < height; ++y) {
    const int dir = (y % 2 == 0) ? 1 : -1;
    const int x0 = (dir > 0) ? 0 : w - 1;
    const int x1 = (dir > 0) ? w : -1;
    for (int x = x0; x != x1; x += dir) {
      const int old = r[0][x];
      const int neu = old >= 128 ? 255 : 0;
      r[0][x] = neu;
      const int err = (old - neu) / 8;
      auto add = [&](int xx, int* dest) {
        if (xx >= 0 && xx < w) {
          dest[xx] += err;
        }
      };
      add(x + dir, r[0]);
      add(x + 2 * dir, r[0]);
      if (y + 1 < height) {
        add(x - dir, r[1]);
        add(x, r[1]);
        add(x + dir, r[1]);
      }
      if (y + 2 < height) {
        add(x, r[2]);
      }
    }
    uint8_t* out = toner + static_cast<size_t>(y) * width;
    for (unsigned x = 0; x < width; ++x) {
      out[x] = r[0][x] >= 128 ? 255 : 0;
    }
    int* done = r[0];
    r[0] = r[1];
    r[1] = r[2];
    r[2] = done;
    load(r[2], y + 3);
  }
}

namespace {

// A 45° clustered-dot screen tiles the raster exactly if the cell is the
// square spanned by the two device-pixel vectors (cell,cell) and
// (cell,-cell) -- both length cell*sqrt(2), perpendicular, so the tile is a
// square rotated 45° with no gaps or overlap. Substituting s = x+y, d = x-y
// turns that rotated square into an axis-aligned box: the vector (cell,cell)
// is s += 2*cell, and (cell,-cell) is d += 2*cell, so a pixel's position
// inside its cell is just (s mod 2*cell, d mod 2*cell). x+y and x-y always
// share parity, so only half of that (2*cell)^2 box is ever hit -- exactly
// the 2*cell*cell pixels the rotated cell actually contains.
//
// Ranking those pixels by distance from the cell centre and normalizing the
// rank into a 0..255 threshold gives an ordered-dither matrix: thresholding
// coverage against it grows a round dot outward from the centre as coverage
// rises, which is what "AM halftoning" means, while the rank-based (rather
// than raw-distance) threshold keeps exactly the right pixel count black at
// every coverage level.
class ClusteredDotScreen {
 public:
  explicit ClusteredDotScreen(unsigned k)
      : period_(2 * k), lut_(static_cast<size_t>(period_) * period_, 0) {
    struct Entry {
      unsigned s;
      unsigned d;
      long distsq;
    };
    std::vector<Entry> entries;
    entries.reserve(static_cast<size_t>(period_) * k);
    const int c = static_cast<int>(k);
    for (unsigned s = 0; s < period_; ++s) {
      for (unsigned d = 0; d < period_; ++d) {
        if ((s ^ d) & 1u) {
          continue;  // x=(s+d)/2 needs s,d same parity.
        }
        const int ds = static_cast<int>(s) - c;
        const int dd = static_cast<int>(d) - c;
        entries.push_back(
            {s, d, static_cast<long>(ds) * ds + static_cast<long>(dd) * dd});
      }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
      if (a.distsq != b.distsq) {
        return a.distsq < b.distsq;
      }
      if (a.s != b.s) {
        return a.s < b.s;
      }
      return a.d < b.d;
    });
    const size_t n = entries.size();
    for (size_t i = 0; i < n; ++i) {
      const Entry& e = entries[i];
      lut_[static_cast<size_t>(e.s) * period_ + e.d] =
          static_cast<uint8_t>((i * 256) / n);
    }
  }

  uint8_t threshold(int x, int y) const {
    const int p = static_cast<int>(period_);
    int s = (x + y) % p;
    if (s < 0) {
      s += p;
    }
    int d = (x - y) % p;
    if (d < 0) {
      d += p;
    }
    return lut_[static_cast<size_t>(s) * period_ + d];
  }

 private:
  unsigned period_;
  std::vector<uint8_t> lut_;
};

}  // namespace

void clustered_dot_45(uint8_t* toner, unsigned width, unsigned height,
                      unsigned cell) {
  if (width == 0 || height == 0) {
    return;
  }
  if (cell == 0) {
    cell = 1;
  }
  static thread_local std::unique_ptr<ClusteredDotScreen> screen;
  static thread_local unsigned screen_cell = 0;
  if (!screen || screen_cell != cell) {
    screen = std::make_unique<ClusteredDotScreen>(cell);
    screen_cell = cell;
  }
  for (unsigned y = 0; y < height; ++y) {
    uint8_t* row = toner + static_cast<size_t>(y) * width;
    for (unsigned x = 0; x < width; ++x) {
      const uint8_t th = screen->threshold(static_cast<int>(x), static_cast<int>(y));
      row[x] = (row[x] > th) ? 255 : 0;
    }
  }
}

}  // namespace sisterhl2030
