// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
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

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++failures;
  }
}

std::vector<uint8_t> slurp(FILE* f) {
  std::vector<uint8_t> out;
  std::fseek(f, 0, SEEK_SET);
  int c;
  while ((c = std::fgetc(f)) != EOF) {
    out.push_back(static_cast<uint8_t>(c));
  }
  return out;
}

bool contains(const std::vector<uint8_t>& hay, const char* needle) {
  const size_t n = std::strlen(needle);
  if (n == 0 || n > hay.size()) {
    return false;
  }
  const auto* p = reinterpret_cast<const char*>(hay.data());
  return std::search(p, p + hay.size(), needle, needle + n) != p + hay.size();
}

}  // namespace

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
    expect(rgb_to_toner(255, 255, 0) < 80, "yellow is light, not solid black");
    {
      unsigned w = 4;
      unsigned h = 4;
      std::vector<uint8_t> block(16, 100);
      sisterhl2030::box_downsample_2x(block, w, h);
      expect(w == 2 && h == 2, "2x2 downsample halves both axes");
      expect(block.size() == 4 && block[0] == 100, "2x2 box of 100 stays 100");
    }
    {
      unsigned w = 2;
      unsigned h = 2;
      std::vector<uint8_t> block{10, 20, 30, 40};
      sisterhl2030::nn_upsample_2x(block, w, h);
      expect(w == 4 && h == 4, "nearest upsample doubles both axes");
      expect(block.size() == 16 && block[0] == 10 && block[1] == 10 &&
                 block[4] == 10 && block[5] == 10,
             "2x2 of 10 expands to a 2x2 block of 10");
      expect(block[2] == 20 && block[15] == 40, "upsample keeps other corners");
    }
    {
      uint8_t v[2] = {100, 200};
      sisterhl2030::scale_coverage(v, 2, 1.0f);
      expect(v[0] == 100 && v[1] == 200, "gain 1 leaves toner unchanged");
      sisterhl2030::scale_coverage(v, 2, 1.5f);
      expect(v[0] == 150 && v[1] == 255, "gain >1 darkens and clamps");
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
    expect(contains(bytes, "\033&l1h1001H"), "tray command");
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
  expect(sisterhl2030::rgb_to_toner(255, 255, 0) < 40, "yellow stays light");
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
