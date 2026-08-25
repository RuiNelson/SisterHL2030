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
#include <map>
#include <memory>
#include <utility>
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
  const uint8_t luminance = coverage_from_blackness(1.0f - y);

  // How saturated is it? Zero for any neutral, so this whole stage is a no-op
  // on greys and the calibrated grey scale cannot be disturbed by it.
  const int hi = std::max(r, std::max(g, b));
  const int lo = std::min(r, std::min(g, b));
  if (hi == 0 || hi == lo) {
    return luminance;
  }
  const float saturation = static_cast<float>(hi - lo) / static_cast<float>(hi);
  const int floor_coverage = static_cast<int>(std::lround(
      kChromaFloor * std::pow(saturation, kChromaKnee) * 255.0f));
  return static_cast<uint8_t>(
      std::max(static_cast<int>(luminance), std::min(floor_coverage, 255)));
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

void nn_upsample_2x_x(std::vector<uint8_t>& toner, unsigned& width,
                      unsigned height) {
  if (width == 0 || height == 0) {
    return;
  }
  const unsigned nw = width * 2;
  std::vector<uint8_t> out(static_cast<size_t>(nw) * height);
  for (unsigned y = 0; y < height; ++y) {
    const uint8_t* src = toner.data() + static_cast<size_t>(y) * width;
    uint8_t* dst = out.data() + static_cast<size_t>(y) * nw;
    for (unsigned x = 0; x < width; ++x) {
      dst[2 * x] = dst[2 * x + 1] = src[x];
    }
  }
  toner.swap(out);
  width = nw;
}

namespace {

void halve_y(std::vector<uint8_t>& toner, unsigned width, unsigned& height) {
  if (width == 0 || height < 2) {
    return;
  }
  const unsigned nh = height / 2;
  std::vector<uint8_t> out(static_cast<size_t>(width) * nh);
  for (unsigned y = 0; y < nh; ++y) {
    const uint8_t* a = toner.data() + static_cast<size_t>(2 * y) * width;
    const uint8_t* b = a + width;
    uint8_t* dst = out.data() + static_cast<size_t>(y) * width;
    for (unsigned x = 0; x < width; ++x) {
      dst[x] = static_cast<uint8_t>((static_cast<unsigned>(a[x]) + b[x] + 1) / 2);
    }
  }
  toner.swap(out);
  height = nh;
}

void halve_x(std::vector<uint8_t>& toner, unsigned& width, unsigned height) {
  if (width < 2 || height == 0) {
    return;
  }
  const unsigned nw = width / 2;
  std::vector<uint8_t> out(static_cast<size_t>(nw) * height);
  for (unsigned y = 0; y < height; ++y) {
    const uint8_t* src = toner.data() + static_cast<size_t>(y) * width;
    uint8_t* dst = out.data() + static_cast<size_t>(y) * nw;
    for (unsigned x = 0; x < nw; ++x) {
      dst[x] = static_cast<uint8_t>(
          (static_cast<unsigned>(src[2 * x]) + src[2 * x + 1] + 1) / 2);
    }
  }
  toner.swap(out);
  width = nw;
}

void double_y(std::vector<uint8_t>& toner, unsigned width, unsigned& height) {
  if (width == 0 || height == 0) {
    return;
  }
  const unsigned nh = height * 2;
  std::vector<uint8_t> out(static_cast<size_t>(width) * nh);
  for (unsigned y = 0; y < height; ++y) {
    const uint8_t* src = toner.data() + static_cast<size_t>(y) * width;
    uint8_t* dst = out.data() + static_cast<size_t>(2 * y) * width;
    std::copy(src, src + width, dst);
    std::copy(src, src + width, dst + width);
  }
  toner.swap(out);
  height = nh;
}

}  // namespace

DeviceGrid device_grid(int pjl_resolution) {
  if (pjl_resolution <= 300) {
    return {300, 300};
  }
  if (pjl_resolution >= 1200) {
    return {1200, 600};  // RAS1200MODE is "1200x600dpi", see halftone.h.
  }
  return {600, 600};
}

void resample_to_grid(std::vector<uint8_t>& toner, unsigned& width,
                      unsigned& height, int src_dpi, const DeviceGrid& grid) {
  if (src_dpi <= 0 || width == 0 || height == 0) {
    return;
  }
  // Each axis independently, in powers of two -- the only ratios that occur
  // are 1, 2 and 1/2, and a box filter down / pixel replication up keeps this
  // exact. Down is a box average because throwing samples away would alias the
  // very gradients the halftone is about to render; up is replication because
  // there is nothing between the source samples to interpolate towards.
  for (int dpi = src_dpi; dpi >= grid.dpi_x * 2 && width >= 2; dpi /= 2) {
    halve_x(toner, width, height);
  }
  for (int dpi = src_dpi; dpi * 2 <= grid.dpi_x; dpi *= 2) {
    nn_upsample_2x_x(toner, width, height);
  }
  for (int dpi = src_dpi; dpi >= grid.dpi_y * 2 && height >= 2; dpi /= 2) {
    halve_y(toner, width, height);
  }
  for (int dpi = src_dpi; dpi * 2 <= grid.dpi_y; dpi *= 2) {
    double_y(toner, width, height);
  }
}

float boundary_density(const DotTransfer& dt, float c) {
  c = std::clamp(c, 0.0f, 1.0f);
  const float w = std::max(dt.pixel_w_um, 1e-6f);
  const float h = std::max(dt.pixel_h_um, 1e-6f);
  if (dt.clustered) {
    // One round dot per screen cell. A cell of radius k holds 2*k*k pixels
    // (halftone.h, ClusteredDotScreen), so at coverage c the dot covers
    // n = 2*k*k*c of them; as a disc that is a perimeter of 2*sqrt(pi*n)
    // pixels, and there is one such dot per 2*k*k pixels of paper. Beyond
    // half coverage the dots have merged and it is the white holes that
    // shrink, so the density is symmetric about c = 0.5.
    const float k = static_cast<float>(std::max(1u, dt.cell));
    const float p = std::sqrt(w * h);
    const float cc = std::min(c, 1.0f - c);
    return std::sqrt(2.0f * 3.14159265f * cc) / (k * p);
  }
  // Dispersed screen: single-pixel dots at density c/(w*h), each with two
  // edges of length h and two of length w, and a given edge is a real
  // toner/paper boundary only when its neighbour is the other colour --
  // probability (1-c). Hence the c(1-c) shape, which correctly vanishes at
  // both ends: solid black and bare paper have no interior boundary to lose.
  return 2.0f * c * (1.0f - c) * (1.0f / w + 1.0f / h);
}

float contrast_gain(const DotTransfer& dt) {
  // How much of the pattern lies inside the suppression band, evaluated at
  // mid-tone where the pattern has the most boundary. A dispersed 600 dpi
  // screen comes out above 1 -- there is no part of it further than the band
  // from an edge, which is exactly why it suffers most.
  return 1.0f + dt.suppression_um * boundary_density(dt, 0.5f);
}

float printed_coverage(const DotTransfer& dt, float pattern_coverage) {
  const float c = std::clamp(pattern_coverage, 0.0f, 1.0f);
  if (c <= 0.0f || c >= 1.0f) {
    return c * dt.economode_gain;
  }
  // A gain on the log-odds: the minority phase is pushed toward the majority
  // one, hard where the gain is high. Monotone for any gain, and pinned at 0,
  // 0.5 and 1, so solid stays solid and paper stays paper however this is
  // calibrated.
  const float g = contrast_gain(dt);
  const float black = std::pow(c, g);
  const float white = std::pow(1.0f - c, g);
  return (black / (black + white)) * dt.economode_gain;
}

const std::vector<uint8_t>& screen_response(bool clustered, unsigned cell) {
  // A patch big enough that error diffusion reaches its steady state, and
  // only its middle is counted so the transient along the top and left edge
  // is not part of the answer.
  constexpr unsigned kSide = 128;
  // Keyed, not single-slot: the caller holds the returned reference, so
  // asking for the other screen must not invalidate it.
  static thread_local std::map<std::pair<bool, unsigned>, std::vector<uint8_t>>
      cache;
  const auto key = std::make_pair(clustered, cell);
  const auto it = cache.find(key);
  if (it != cache.end()) {
    return it->second;
  }
  std::vector<uint8_t> table(256, 0);
  std::vector<uint8_t> patch(static_cast<size_t>(kSide) * kSide);
  for (int level = 0; level < 256; ++level) {
    std::fill(patch.begin(), patch.end(), static_cast<uint8_t>(level));
    if (clustered) {
      clustered_dot_45(patch.data(), kSide, kSide, cell);
    } else {
      atkinson(patch.data(), kSide, kSide);
    }
    long on = 0;
    long n = 0;
    for (unsigned y = kSide / 4; y < kSide * 3 / 4; ++y) {
      for (unsigned x = kSide / 4; x < kSide * 3 / 4; ++x) {
        ++n;
        if (patch[static_cast<size_t>(y) * kSide + x] >= 128) {
          ++on;
        }
      }
    }
    table[static_cast<size_t>(level)] =
        static_cast<uint8_t>(std::lround(255.0 * static_cast<double>(on) / n));
  }
  table[0] = 0;
  table[255] = 255;
  return cache.emplace(key, std::move(table)).first->second;
}

float paper_coverage(const DotTransfer& dt, float nominal) {
  const float c = std::clamp(nominal, 0.0f, 1.0f);
  if (!dt.linearize) {
    return printed_coverage(dt, c);
  }
  const std::vector<uint8_t>& screen = screen_response(dt.clustered, dt.cell);
  const int level = static_cast<int>(std::lround(c * 255.0f));
  return printed_coverage(dt, screen[static_cast<size_t>(level)] / 255.0f);
}

DotTransfer engine_transfer(const ScreenCalibration& screen,
                            const DeviceGrid& grid, bool economode) {
  DotTransfer dt;
  dt.pixel_w_um = pixel_um(grid.dpi_x);
  dt.pixel_h_um = pixel_um(grid.dpi_y);
  dt.suppression_um = screen.suppression_um;
  dt.economode_gain = economode ? screen.economode_gain : 1.0f;
  dt.density = screen.density;
  dt.max_toner = economode ? screen.max_toner_economode : screen.max_toner_full;
  dt.clustered = screen.clustered;
  dt.cell = screen.cell > 0 ? screen.cell : 1;
  dt.linearize = screen.linearize;
  return dt;
}

std::vector<uint8_t> dot_transfer_lut(const DotTransfer& dt) {
  // Ink limit: the table is never allowed to ask for more than this,
  // whatever the darkness model above computes. 255 (the default) is a
  // no-op -- `std::min` against it leaves every other calibration
  // byte-for-byte unchanged.
  const uint8_t ink_cap = static_cast<uint8_t>(
      std::lround(std::clamp(dt.max_toner, 0.0f, 1.0f) * 255.0f));
  auto cap_ink = [ink_cap](std::vector<uint8_t>& table) {
    for (uint8_t& v : table) {
      v = std::min(v, ink_cap);
    }
  };

  std::vector<uint8_t> lut(256, 0);
  if (dt.suppression_um == 0.0f && dt.economode_gain == 1.0f &&
      dt.density == 1.0f && !dt.linearize) {
    // A screen calibrated as needing no correction. Short-circuit to an exact
    // identity rather than inverting a curve that is already the identity:
    // it is not just faster, it guarantees byte-for-byte that this screen's
    // output cannot drift when the other screen's numbers are re-measured.
    for (int i = 0; i < 256; ++i) {
      lut[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
    }
    cap_ink(lut);
    return lut;
  }

  // Sample the forward chain once, under a running maximum so the inversion
  // below always has a monotone curve to walk.
  constexpr int kSteps = 4096;
  std::vector<float> forward(kSteps + 1);
  float best = 0.0f;
  for (int i = 0; i <= kSteps; ++i) {
    best = std::max(best, paper_coverage(dt, static_cast<float>(i) / kSteps));
    forward[static_cast<size_t>(i)] = best;
  }

  const float inv_density = dt.density > 0.0f ? 1.0f / dt.density : 1.0f;
  int step = 0;
  for (int i = 0; i < 256; ++i) {
    // What this level should actually put on paper. `density` bends the
    // request before the engine is inverted, so it changes how dark the page
    // is without touching the shape the model corrects for.
    const float want =
        dt.density == 1.0f ? i / 255.0f : std::pow(i / 255.0f, inv_density);
    while (step < kSteps && forward[static_cast<size_t>(step)] < want) {
      ++step;
    }
    lut[static_cast<size_t>(i)] = static_cast<uint8_t>(
        std::lround(static_cast<float>(step) * 255.0f / kSteps));
  }

  // Both ends are anchored rather than inverted. Solid black must come out
  // as solid black: a dispersed screen saturates well before 255, so the
  // "smallest input that reaches this coverage" rule would map full black to
  // something like 218 -- identical on a flat field, but on text and edges
  // it hands error diffusion a value it can dither instead of a solid. Ramp
  // whatever the model left on the saturated plateau back up to 255 so the
  // table stays monotone and reaches the corner.
  size_t plateau = 255;
  while (plateau > 0 && lut[plateau - 1] >= lut[255]) {
    --plateau;
  }
  if (plateau < 255) {
    const float lo = lut[plateau];
    for (size_t i = plateau; i <= 255; ++i) {
      const float t = static_cast<float>(i - plateau) / (255 - plateau);
      lut[i] = static_cast<uint8_t>(std::lround(lo + t * (255.0f - lo)));
    }
  }
  lut[0] = 0;      // paper stays paper
  lut[255] = 255;  // and solid stays solid, unless an ink cap pulls it back
  cap_ink(lut);
  return lut;
}

void apply_transfer(uint8_t* toner, size_t n, const std::vector<uint8_t>& lut) {
  if (!toner || lut.size() < 256) {
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    toner[i] = lut[toner[i]];
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
