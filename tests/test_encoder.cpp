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
  using sisterhl2030::floyd_steinberg;
  using sisterhl2030::pack_toner_row;
  using sisterhl2030::rgb_to_toner;

  {
    const unsigned w = 256;
    const unsigned h = 64;
    std::vector<uint8_t> toner(w * h, 128);
    floyd_steinberg(toner.data(), w, h);
    std::vector<uint8_t> packed((w + 7) / 8);
    int black = 0;
    for (unsigned y = 0; y < h; ++y) {
      pack_toner_row(toner.data() + y * w, w, packed.data());
      black += popcount_row(packed.data(), w);
    }
    const int total = static_cast<int>(w * h);
    expect(black > total / 5 && black < (total * 4) / 5,
           "mid-gray Floyd-Steinberg is mixed, not a 50% hard threshold slab");
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
    floyd_steinberg(ramp_page.data(), w, 8);
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

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("ok");
  return 0;
}
