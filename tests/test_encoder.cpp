// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "encoder/halftone.h"
#include "encoder/job.h"
#include "encoder/line.h"

namespace {

int failures = 0;

// Count a failed assertion. The process still runs the rest of the suite.
void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++failures;
  }
}

// Rewind and read the whole FILE* into a byte vector.
std::vector<uint8_t> slurp(FILE* f) {
  std::vector<uint8_t> out;
  std::fseek(f, 0, SEEK_SET);
  int c;
  while ((c = std::fgetc(f)) != EOF) {
    out.push_back(static_cast<uint8_t>(c));
  }
  return out;
}

// True if `needle` occurs as a byte sequence in `hay`.
bool contains(const std::vector<uint8_t>& hay, const char* needle) {
  const size_t n = std::strlen(needle);
  if (n == 0 || n > hay.size()) {
    return false;
  }
  const auto* p = reinterpret_cast<const char*>(hay.data());
  return std::search(p, p + hay.size(), needle, needle + n) != p + hay.size();
}

}  // namespace

// Black pixels in one packed MSB-first row of `width` bits.
int popcount_row(const uint8_t* packed, unsigned width) {
  unsigned n = 0;
  for (unsigned x = 0; x < width; ++x) {
    if (packed[x / 8] & static_cast<uint8_t>(0x80 >> (x % 8))) {
      ++n;
    }
  }
  return static_cast<int>(n);
}

int main() {
  using sisterhl2030::encode_line;
  using sisterhl2030::Job;
  using sisterhl2030::PageParams;
  using sisterhl2030::device_gray_to_toner;
  using sisterhl2030::device_k_to_toner;
  using sisterhl2030::atkinson;
  using sisterhl2030::pack_toner_row;
  using sisterhl2030::rgb_to_toner;

  {
    const unsigned w = 256;
    const unsigned h = 64;
    std::vector<uint8_t> toner(w * h, 128);
    atkinson(toner.data(), w, h);
    std::vector<uint8_t> packed((w + 7) / 8);
    int black = 0;
    for (unsigned y = 0; y < h; ++y) {
      pack_toner_row(toner.data() + y * w, w, packed.data());
      black += popcount_row(packed.data(), w);
    }
    const int total = static_cast<int>(w * h);
    expect(black > total / 5 && black < (total * 4) / 5,
           "mid-gray Atkinson is mixed, not a 50% hard threshold slab");
    std::vector<uint8_t> ramp(w);
    for (unsigned x = 0; x < w; ++x) {
      ramp[x] = rgb_to_toner(static_cast<uint8_t>(x), static_cast<uint8_t>(x),
                             static_cast<uint8_t>(x));
    }
    std::vector<uint8_t> ramp_page = ramp;
    ramp_page.resize(w * 8);
    for (unsigned y = 1; y < 8; ++y) {
      std::memcpy(ramp_page.data() + y * w, ramp.data(), w);
    }
    atkinson(ramp_page.data(), w, 8);
    int dark = 0, light = 0;
    for (unsigned y = 0; y < 8; ++y) {
      for (unsigned x = 0; x < 32; ++x) {
        if (ramp_page[y * w + x] >= 128) {
          ++dark;
        }
        if (ramp_page[y * w + (w - 32 + x)] >= 128) {
          ++light;
        }
      }
    }
    expect(dark > light * 2, "ramp keeps contrast (dark end much blacker)");
    expect(rgb_to_toner(255, 255, 255) == 0, "white RGB is no toner");
    expect(rgb_to_toner(0, 0, 0) == 255, "black RGB is full toner");
    {
      const int mid = rgb_to_toner(128, 128, 128);
      expect(mid > 40 && mid < 120,
             "sRGB mid-grey is a light-mid toner load, not ~75%");
      const int pastel = rgb_to_toner(162, 184, 229);
      expect(pastel > 45 && pastel < 110,
             "pastel blue keeps a visible dither, not ~10% toner");
    }
    expect(device_gray_to_toner(255) == 0, "DeviceGray 255 is white");
    expect(device_k_to_toner(0) == 0, "DeviceK 0 is white");
    expect(device_k_to_toner(255) == 255, "DeviceK 255 is black");
    expect(rgb_to_toner(255, 255, 0) < 100, "yellow is light, not solid black");
    {
      std::vector<uint8_t> t(64 * 48, 128);
      atkinson(t.data(), 64, 48);
      int black = 0;
      for (uint8_t v : t) {
        if (v >= 128) ++black;
      }
      expect(black == 1536, "streaming Atkinson matches full-buffer mid-grey");
    }
    {
      std::vector<uint8_t> t(17 * 9, 90);
      atkinson(t.data(), 17, 9);
      int black = 0;
      for (uint8_t v : t) {
        if (v >= 128) ++black;
      }
      expect(black == 44, "streaming Atkinson matches full-buffer grey-90");
    }
    {
      using sisterhl2030::clustered_dot_45;
      const unsigned w = 240;
      const unsigned h = 240;
      std::vector<uint8_t> white(w * h, 0);
      clustered_dot_45(white.data(), w, h);
      expect(std::all_of(white.begin(), white.end(),
                         [](uint8_t v) { return v == 0; }),
             "white input stays white through the 45° screen");

      std::vector<uint8_t> black(w * h, 255);
      clustered_dot_45(black.data(), w, h);
      expect(std::all_of(black.begin(), black.end(),
                         [](uint8_t v) { return v == 255; }),
             "solid black input stays solid through the 45° screen");

      std::vector<uint8_t> mid(w * h, 128);
      clustered_dot_45(mid.data(), w, h);
      int black_px = 0;
      for (uint8_t v : mid) {
        if (v >= 128) ++black_px;
      }
      const int total = static_cast<int>(w * h);
      expect(black_px > total / 5 && black_px < (total * 4) / 5,
             "mid-grey 45° screen is a mixed dot pattern, not a solid slab");

      // Same coverage fed through two different cell sizes should still
      // land near the same overall tone -- the screen ruling changes the
      // dot pitch, not the reproduced grey level.
      std::vector<uint8_t> mid_coarse(w * h, 128);
      clustered_dot_45(mid_coarse.data(), w, h, 6);
      int coarse_black = 0;
      for (uint8_t v : mid_coarse) {
        if (v >= 128) ++coarse_black;
      }
      expect(std::abs(coarse_black - black_px) < total / 10,
             "cell size changes dot pitch, not the reproduced grey level");
    }
    {
      using sisterhl2030::clustered_dot_45;
      using sisterhl2030::rgb_to_toner;
      const unsigned w = 256;
      const unsigned h = 64;
      std::vector<uint8_t> ramp(w);
      for (unsigned x = 0; x < w; ++x) {
        ramp[x] = rgb_to_toner(static_cast<uint8_t>(x), static_cast<uint8_t>(x),
                               static_cast<uint8_t>(x));
      }
      std::vector<uint8_t> ramp_page(static_cast<size_t>(w) * h);
      for (unsigned y = 0; y < h; ++y) {
        std::memcpy(ramp_page.data() + static_cast<size_t>(y) * w, ramp.data(), w);
      }
      clustered_dot_45(ramp_page.data(), w, h);
      int dark = 0, light = 0;
      for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < 32; ++x) {
          if (ramp_page[y * w + x] >= 128) ++dark;
          if (ramp_page[y * w + (w - 32 + x)] >= 128) ++light;
        }
      }
      expect(dark > light * 2,
             "45° screen ramp keeps contrast (dark end much blacker)");
    }
    {
      // Tiling correctness: the screen must repeat with period 2*cell in
      // both x and y, since that is the whole point of the (x+y, x-y)
      // lattice trick -- a bug there shows up as visible seams on paper.
      using sisterhl2030::clustered_dot_45;
      const unsigned cell = 6;
      const unsigned period = 2 * cell;
      const unsigned w = period * 5;
      const unsigned h = period * 5;
      std::vector<uint8_t> a(static_cast<size_t>(w) * h, 96);
      clustered_dot_45(a.data(), w, h, cell);
      bool periodic = true;
      for (unsigned y = 0; y < h - period && periodic; ++y) {
        for (unsigned x = 0; x < w - period; ++x) {
          if (a[y * w + x] != a[(y + period) * w + (x + period)]) {
            periodic = false;
            break;
          }
        }
      }
      expect(periodic, "45° screen tiles losslessly with period 2*cell");
    }
    {
      // The device grid the halftone runs on. HQ1200 is the interesting one:
      // 1200 across the scan, still 600 down the page.
      using sisterhl2030::device_grid;
      expect(device_grid(300).dpi_x == 300 && device_grid(300).dpi_y == 300,
             "draft halftones on a 300x300 grid");
      expect(device_grid(600).dpi_x == 600 && device_grid(600).dpi_y == 600,
             "normal halftones on a 600x600 grid");
      expect(device_grid(1200).dpi_x == 1200 && device_grid(1200).dpi_y == 600,
             "HQ1200 halftones on a 1200x600 grid");
    }
    {
      using sisterhl2030::resample_to_grid;
      unsigned w = 8;
      unsigned h = 8;
      std::vector<uint8_t> page(64, 120);
      resample_to_grid(page, w, h, 600, sisterhl2030::device_grid(1200));
      expect(w == 16 && h == 8 && page.size() == 128,
             "600 dpi raster widens to the 1200x600 grid, height untouched");
      expect(page[0] == 120 && page[1] == 120,
             "widening replicates, it does not interpolate");

      w = 8;
      h = 8;
      page.assign(64, 120);
      resample_to_grid(page, w, h, 600, sisterhl2030::device_grid(300));
      expect(w == 4 && h == 4 && page.size() == 16,
             "600 dpi raster box-filters down to the 300 dpi grid");

      w = 8;
      h = 8;
      page.assign(64, 120);
      resample_to_grid(page, w, h, 600, sisterhl2030::device_grid(600));
      expect(w == 8 && h == 8, "a raster already on the grid is left alone");
    }
    {
      // The darkness model. What matters is not the exact numbers -- those
      // move with kAtkinsonSuppressionUm -- but that the shape is right:
      // white, mid and solid are fixed points, the minority phase is pushed
      // toward the majority one in BOTH directions, a finer grid suffers
      // more than a coarse one, and a clustered screen with its far shorter
      // boundary suffers least.
      using sisterhl2030::DotTransfer;
      using sisterhl2030::dot_transfer_lut;
      using sisterhl2030::pixel_um;
      using sisterhl2030::contrast_gain;
      using sisterhl2030::printed_coverage;

      auto grid_transfer = [](int dpi_x, int dpi_y, bool clustered) {
        DotTransfer dt;
        dt.pixel_w_um = pixel_um(dpi_x);
        dt.pixel_h_um = pixel_um(dpi_y);
        dt.suppression_um = sisterhl2030::kAtkinsonSuppressionUm;
        dt.clustered = clustered;
        return dt;
      };
      const DotTransfer at300 = grid_transfer(300, 300, false);
      const DotTransfer at600 = grid_transfer(600, 600, false);
      const DotTransfer am600 = grid_transfer(600, 600, true);

      expect(printed_coverage(at600, 0.0f) == 0.0f,
             "paper prints as paper however hard the engine suppresses");
      expect(printed_coverage(at600, 1.0f) == 1.0f,
             "and a solid has no minority phase to suppress");
      expect(std::abs(printed_coverage(at600, 0.5f) - 0.5f) < 0.01f,
             "mid-tone is the fixed point the suppression turns around");
      expect(printed_coverage(at600, 0.25f) < 0.25f,
             "isolated black dots do not develop, so highlights lose ink");
      expect(printed_coverage(at600, 0.75f) > 0.75f,
             "isolated white holes fill in, so shadows gain it");
      expect(printed_coverage(at600, 0.25f) < printed_coverage(at300, 0.25f),
             "a finer grid is more boundary, so it suffers more");
      expect(printed_coverage(am600, 0.25f) > printed_coverage(at600, 0.25f),
             "one clustered blob suffers less than the same area scattered");
      expect(contrast_gain(at600) > contrast_gain(at300),
             "and that shows up directly as a higher contrast gain");

      const std::vector<uint8_t> lut600 = dot_transfer_lut(at600);
      const std::vector<uint8_t> lut300 = dot_transfer_lut(at300);
      expect(lut600.size() == 256, "the transfer table covers every level");
      expect(lut600[0] == 0 && lut600[255] == 255,
             "white and solid are asked for unchanged");
      expect(dot_transfer_lut(am600)[255] == 255 && dot_transfer_lut(am600)[0] == 0,
             "and the same holds for the clustered screen");
      // Mid-tone is the fixed point, so the interesting asymmetry is at the
      // ends: ask for more in the highlights, less in the shadows.
      expect(lut600[64] > 64, "reaching 25 %% on paper asks for more than 25 %%");
      expect(lut600[191] < 191, "and reaching 75 %% asks for less than 75 %%");
      expect(lut600[64] > lut300[64],
             "the finer grid has to ask for more to land in the same place");
      bool monotone = true;
      for (int i = 1; i < 256; ++i) {
        if (lut600[static_cast<size_t>(i)] < lut600[static_cast<size_t>(i - 1)]) {
          monotone = false;
        }
      }
      expect(monotone, "the transfer table never doubles back");

      // The screen's own flat-field response, which the table also inverts.
      using sisterhl2030::screen_response;
      const std::vector<uint8_t>& fm = screen_response(false, 4);
      const std::vector<uint8_t>& am = screen_response(true, 4);
      expect(fm.size() == 256 && am.size() == 256, "the screen response covers every level");
      expect(fm[0] == 0 && fm[255] == 255 && am[0] == 0 && am[255] == 255,
             "both screens leave paper and solid alone");
      int am_worst = 0;
      for (int i = 0; i < 256; ++i) {
        am_worst = std::max(am_worst,
                            std::abs(static_cast<int>(am[static_cast<size_t>(i)]) - i));
      }
      // cell=4 gives 32 gray levels, so ~8/255 of quantisation is the floor.
      expect(am_worst <= 10, "an ordered screen blackens what it was asked for");
      expect(fm[26] == 0, "Atkinson drops a flat 10 %% field entirely");
      expect(fm[230] == 255, "Atkinson takes a flat 90 %% field to solid");
      expect(fm[160] > 160, "Atkinson gains contrast above mid-grey");

      // Round trip over the range the screen can actually reproduce -- the
      // clipped ends are a property of Atkinson, not something to invert away.
      int worst = 0;
      for (int target = 30; target <= 200; target += 5) {
        const float got = sisterhl2030::paper_coverage(
            at600, lut600[static_cast<size_t>(target)] / 255.0f);
        const int err = std::abs(static_cast<int>(std::lround(got * 255.0f)) -
                                 target);
        worst = std::max(worst, err);
      }
      expect(worst <= 4, "inverting the model reproduces the requested tone");

      // The two screens are calibrated independently: AM45 was judged right
      // on paper as it is, so its table must be an exact identity on every
      // grid and in both ECONOMODE states. Nothing anyone does to the
      // Atkinson constants can move a single AM45 pixel, and this is the
      // assertion that keeps it that way.
      using sisterhl2030::engine_transfer;
      bool am45_identity = true;
      for (int res : {300, 600, 1200}) {
        const std::vector<uint8_t> table = dot_transfer_lut(engine_transfer(
            sisterhl2030::kAm45Screen, sisterhl2030::device_grid(res)));
        for (int i = 0; i < 256; ++i) {
          if (table[static_cast<size_t>(i)] != i) {
            am45_identity = false;
          }
        }
      }
      expect(am45_identity, "AM45 asks for exactly what it was given");
      expect(dot_transfer_lut(engine_transfer(
                 sisterhl2030::kAtkinsonScreen,
                 sisterhl2030::device_grid(600)))[128] != 128,
             "and Atkinson does not, so the two really are separate");
      expect(sisterhl2030::active_screen().name != nullptr,
             "the build knows which screen it prints with");

      // ECONOMODE is the printer's business, not the halftone's: it must not
      // reach the darkness model at all. DotTransfer no longer has a term for
      // it, so the check that keeps it out is that one grid gives one curve --
      // draft and normal share the ECONOMODE flag but differ here purely by
      // geometry, and nothing in the model can tell whether the flag is set.
      using sisterhl2030::ink_limit;
      const std::vector<uint8_t> draft_lut =
          dot_transfer_lut(engine_transfer(sisterhl2030::kAtkinsonScreen,
                                           sisterhl2030::device_grid(300)));
      const std::vector<uint8_t> normal_lut =
          dot_transfer_lut(engine_transfer(sisterhl2030::kAtkinsonScreen,
                                           sisterhl2030::device_grid(600)));
      const std::vector<uint8_t> fine_lut =
          dot_transfer_lut(engine_transfer(sisterhl2030::kAtkinsonScreen,
                                           sisterhl2030::device_grid(1200)));
      expect(draft_lut != normal_lut,
             "the grid alone separates the modes, with no ECONOMODE term");

      // The ink limit: draft and normal cap at 80 % of black, fine at 90 %,
      // and the cap binds the top of the table rather than scaling it.
      auto peak = [](const std::vector<uint8_t>& t) {
        return *std::max_element(t.begin(), t.end());
      };
      expect(peak(draft_lut) == 204 && peak(normal_lut) == 204,
             "draft and normal never ask for more than 80 %% of black");
      expect(peak(fine_lut) == 230, "and fine stops at 90 %%");
      expect(ink_limit(sisterhl2030::kAm45Screen,
                       sisterhl2030::device_grid(600)) == 1.0f &&
                 ink_limit(sisterhl2030::kAm45Screen,
                           sisterhl2030::device_grid(1200)) == 1.0f,
             "while AM45 is uncapped, as it was judged on paper");
    }
  }

  // White line compresses to a single 0xFF.
  const std::vector<uint8_t> white(16, 0);
  const auto blank = encode_line(white);
  expect(blank.size() == 1 && blank[0] == 0xFF, "white line is 0xFF");

  // Absolute encoding of a non-white line starts with edit count 1.
  std::vector<uint8_t> black(16, 0xFF);
  const auto abs = encode_line(black);
  expect(!abs.empty() && abs[0] == 1, "absolute line edit count is 1");

  // Identical delta should produce zero edits (empty payload after count).
  const auto delta_same = encode_line(black, black);
  expect(delta_same.size() == 1 && delta_same[0] == 0,
         "unchanged line has zero edits");

  // A full white page must still carry PJL + mode 1030 + form feed.
  char tmpl[] = "/tmp/sisterhl2030-test-XXXXXX";
  const int fd = mkstemp(tmpl);
  expect(fd >= 0, "mkstemp");
  FILE* f = fd >= 0 ? fdopen(fd, "w+b") : nullptr;
  expect(f != nullptr, "fdopen");
  if (f) {
    const int width_bytes = 8;
    const int height = 4;
    int row = 0;
    auto next = [&](std::vector<uint8_t>& buf) {
      if (row >= height) {
        return false;
      }
      std::fill(buf.begin(), buf.end(), 0);
      ++row;
      return true;
    };
    {
      Job job(f, "test page");
      PageParams p;
      job.encode_page(p, height, width_bytes, next);
      expect(job.pages() == 1, "one page encoded");
    }
    const auto bytes = slurp(f);
    std::fclose(f);
    std::remove(tmpl);

    expect(!bytes.empty() && bytes[0] == 0x1b, "starts with UEL ESC");
    expect(contains(bytes, "\033%-12345X@PJL\n"), "UEL + PJL");
    expect(contains(bytes, "@PJL SET RAS1200MODE = OFF\n"), "ras1200 off");
    expect(contains(bytes, "@PJL SET RESOLUTION = 600\n"), "resolution");
    expect(contains(bytes, "@PJL SET ECONOMODE = OFF\n"), "economode off by default");
    expect(contains(bytes, "@PJL SET MEDIATYPE = REGULAR\n"), "media");
    expect(contains(bytes, "@PJL ENTER LANGUAGE = PCL\n"), "enter PCL");
    expect(contains(bytes, "\033&l1h1001H"), "tray 1 command");
    expect(contains(bytes, "\033*b1030m"), "mode 1030");
    expect(contains(bytes, "1030M"), "leave mode 1030");
    expect(!contains(bytes, "@PJL JOB NAME="), "no JOB NAME");
    expect(!contains(bytes, "@PJL EOJ"), "no EOJ");
    expect(std::find(bytes.begin(), bytes.end(), static_cast<uint8_t>('\f')) !=
               bytes.end(),
           "form feed");
    expect(std::find(bytes.begin(), bytes.end(), static_cast<uint8_t>(0xFF)) !=
               bytes.end(),
           "white-line 0xFF in stream");
  }

  {
    char econo_tmpl[] = "/tmp/sisterhl2030-econo-XXXXXX";
    const int efd = mkstemp(econo_tmpl);
    expect(efd >= 0, "mkstemp economode");
    FILE* ef = efd >= 0 ? fdopen(efd, "w+b") : nullptr;
    expect(ef != nullptr, "fdopen economode");
    if (ef) {
      int row = 0;
      auto next = [&](std::vector<uint8_t>& buf) {
        if (row >= 2) {
          return false;
        }
        std::fill(buf.begin(), buf.end(), 0);
        ++row;
        return true;
      };
      {
        Job job(ef, "econo");
        PageParams p;
        p.economode = true;
        job.encode_page(p, 2, 8, next);
      }
      const auto bytes = slurp(ef);
      std::fclose(ef);
      std::remove(econo_tmpl);
      expect(contains(bytes, "@PJL SET ECONOMODE = ON\n"),
             "economode on sets PJL ECONOMODE");
      expect(!contains(bytes, "@PJL SET ECONOMODE = OFF\n"),
             "economode on does not also emit OFF");
    }
  }

  {
    char hq_tmpl[] = "/tmp/sisterhl2030-hq-XXXXXX";
    const int hfd = mkstemp(hq_tmpl);
    expect(hfd >= 0, "mkstemp hq1200");
    FILE* hf = hfd >= 0 ? fdopen(hfd, "w+b") : nullptr;
    expect(hf != nullptr, "fdopen hq1200");
    if (hf) {
      int row = 0;
      auto next = [&](std::vector<uint8_t>& buf) {
        if (row >= 2) {
          return false;
        }
        std::fill(buf.begin(), buf.end(), 0);
        ++row;
        return true;
      };
      {
        Job job(hf, "hq");
        PageParams p;
        p.resolution = 1200;
        job.encode_page(p, 2, 8, next);
      }
      const auto bytes = slurp(hf);
      std::fclose(hf);
      std::remove(hq_tmpl);
      expect(contains(bytes, "@PJL SET RAS1200MODE = TRUE\n"),
             "HQ1200 sets RAS1200MODE TRUE");
      expect(contains(bytes, "@PJL SET RESOLUTION = 600\n"),
             "HQ1200 still sets RESOLUTION 600");
    }
  }

  auto encode_params = [](const PageParams& p, const char* tmpl_base,
                          std::vector<uint8_t>* out) {
    char tmpl[64];
    std::snprintf(tmpl, sizeof(tmpl), "%s", tmpl_base);
    const int fd = mkstemp(tmpl);
    expect(fd >= 0, "mkstemp tray");
    FILE* f = fd >= 0 ? fdopen(fd, "w+b") : nullptr;
    expect(f != nullptr, "fdopen tray");
    if (!f) {
      return;
    }
    int row = 0;
    auto next = [&](std::vector<uint8_t>& buf) {
      if (row >= 2) {
        return false;
      }
      std::fill(buf.begin(), buf.end(), 0);
      ++row;
      return true;
    };
    {
      Job job(f, "tray");
      job.encode_page(p, 2, 8, next);
    }
    *out = slurp(f);
    std::fclose(f);
    std::remove(tmpl);
  };

  {
    PageParams p;
    p.sourcetray = "MANUAL";
    std::vector<uint8_t> bytes;
    encode_params(p, "/tmp/sisterhl2030-manual-XXXXXX", &bytes);
    expect(contains(bytes, "\033&l2H"), "manual feed is ESC &l2H");
    expect(!contains(bytes, "\033&l1h1001H"), "manual feed is not tray 1");
  }

  {
    PageParams p;
    p.mediatype = "ENVELOPES";
    std::vector<uint8_t> bytes;
    encode_params(p, "/tmp/sisterhl2030-env-XXXXXX", &bytes);
    expect(contains(bytes, "\033&l3H"),
           "envelope auto-selects manual envelope feed");
    expect(!contains(bytes, "\033&l1h1001H"), "envelope is not tray 1");
  }

  {
    PageParams p;
    p.mediatype = "THICK";
    std::vector<uint8_t> bytes;
    encode_params(p, "/tmp/sisterhl2030-thick-XXXXXX", &bytes);
    expect(contains(bytes, "\033&l2H"), "thick stock auto-selects manual feed");
  }

  {
    char d_tmpl[] = "/tmp/sisterhl2030-draft-XXXXXX";
    const int dfd = mkstemp(d_tmpl);
    expect(dfd >= 0, "mkstemp draft");
    FILE* df = dfd >= 0 ? fdopen(dfd, "w+b") : nullptr;
    expect(df != nullptr, "fdopen draft");
    if (df) {
      int row = 0;
      auto next = [&](std::vector<uint8_t>& buf) {
        if (row >= 2) {
          return false;
        }
        std::fill(buf.begin(), buf.end(), 0);
        ++row;
        return true;
      };
      {
        Job job(df, "draft");
        PageParams p;
        p.resolution = 300;
        job.encode_page(p, 2, 8, next);
      }
      const auto bytes = slurp(df);
      std::fclose(df);
      std::remove(d_tmpl);
      expect(contains(bytes, "@PJL SET RAS1200MODE = OFF\n"),
             "300 dpi leaves RAS1200 off");
      expect(contains(bytes, "@PJL SET RESOLUTION = 300\n"),
             "draft sets RESOLUTION 300");
    }
  }

  // Greys must survive the colour path untouched: the laser curve was tuned
  // against the grey rasters macOS produces, and device_gray_to_toner routes
  // through rgb_to_toner.
  for (int v = 0; v <= 255; v += 17) {
    const uint8_t g = static_cast<uint8_t>(v);
    expect(sisterhl2030::rgb_to_toner(g, g, g) ==
               sisterhl2030::device_gray_to_toner(g),
           "neutral grey is unchanged by the colour path");
  }
  // Saturated colour must not slam to near-solid coverage.
  expect(sisterhl2030::rgb_to_toner(255, 0, 0) < 128,
         "pure red stays below half coverage");
  // ... but it must not vanish into the paper either. Luminance alone puts
  // yellow at 3 % coverage, which prints as nothing; the chroma floor is what
  // keeps a saturated colour distinguishable from bare paper.
  const int floor8 =
      static_cast<int>(std::lround(sisterhl2030::kChromaFloor * 255.0f));
  for (auto c : {std::array<uint8_t, 3>{255, 255, 0},
                 std::array<uint8_t, 3>{0, 255, 255},
                 std::array<uint8_t, 3>{0, 255, 0}}) {
    expect(sisterhl2030::rgb_to_toner(c[0], c[1], c[2]) >= floor8 - 1,
           "a fully saturated colour reaches the chroma floor");
  }
  expect(sisterhl2030::rgb_to_toner(255, 255, 0) < 100,
         "yellow is lifted off the paper, not turned into a dark grey");
  // The knee is calibrated, not chosen: on the white-to-green ramp the density
  // that used to need full strength was wanted by 8.5 % of the way along, and
  // 8.5 % saturation is (233,255,233). Pin it -- this number came off paper.
  {
    const uint8_t pale_green = sisterhl2030::rgb_to_toner(233, 255, 233);
    expect(pale_green >= 30 && pale_green <= 40,
           "8.5 %% saturation carries the coverage the print asked for");
  }
  // And pin what that costs, so it cannot surprise anyone later: saturation is
  // all the floor knows, so a warm near-white gets lifted just as far as a
  // pale green does. Raise kChromaKnee toward 1 to give that back.
  expect(sisterhl2030::rgb_to_toner(255, 250, 240) > 20,
         "the knee cannot tell a pale tint from a pale colour");
  expect(sisterhl2030::rgb_to_toner(0, 0, 0) == 255, "black is full toner");
  expect(sisterhl2030::rgb_to_toner(255, 255, 255) == 0, "white is no toner");

  // A flat mid-grey must come out as a dot pattern, never a solid block.
  // Regression guard: when the PAPPL app was handed the red channel instead
  // of luma, saturated colour collapsed to pure black/white, and this is what
  // that failure looks like by the time it reaches the packing stage.
  {
    const unsigned w = 64, h = 64;
    const uint8_t mid = sisterhl2030::rgb_to_toner(128, 128, 128);
    expect(mid != 0 && mid != 255, "mid-grey maps to a mid toner value");

    std::vector<uint8_t> toner(static_cast<size_t>(w) * h, mid);
    sisterhl2030::atkinson(toner.data(), w, h);

    std::vector<uint8_t> packed((w + 7) / 8);
    unsigned mixed = 0;
    for (unsigned y = 0; y < h; ++y) {
      sisterhl2030::pack_toner_row(toner.data() + static_cast<size_t>(y) * w, w,
                                   packed.data());
      for (uint8_t b : packed) {
        if (b != 0x00 && b != 0xFF) ++mixed;
      }
    }
    expect(mixed > 0, "mid-grey dithers instead of going solid");
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("ok");
  return 0;
}
