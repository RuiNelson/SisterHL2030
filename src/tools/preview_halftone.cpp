// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Screen preview of the same Atkinson halftone used when printing.
// Writes a BMP (white paper, black toner) so Preview.app can open it.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "encoder/halftone.h"

namespace {

void usage() {
  std::cerr
      << "usage: sister-preview [--chart] [--cell N] [-o out.bmp] [image]\n"
      << "  --chart     built-in gray ramp, midtones, and colour patches\n"
      << "  --cell N    AM screen dot radius in pixels (default 5, ~85 lpi\n"
      << "              at 600 dpi)\n"
      << "  image       JPEG/PNG/TIFF/BMP (converted with sips on macOS)\n"
      << "Without arguments, writes a chart next to the binary.\n";
}

bool write_bmp_bgr(const char* path, int w, int h,
                   const std::vector<uint8_t>& bgr) {
  const int rowb = (w * 3 + 3) & ~3;
  const uint32_t img = static_cast<uint32_t>(rowb) * static_cast<uint32_t>(h);
  const uint32_t off = 54;
  const uint32_t size = off + img;
  std::vector<uint8_t> row(static_cast<size_t>(rowb), 0);
  FILE* f = std::fopen(path, "wb");
  if (!f) {
    return false;
  }
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto i32 = [&](int32_t v) { std::fwrite(&v, 4, 1, f); };
  std::fputc('B', f);
  std::fputc('M', f);
  u32(size);
  u16(0);
  u16(0);
  u32(off);
  u32(40);
  i32(w);
  i32(h);
  u16(1);
  u16(24);
  u32(0);
  u32(img);
  i32(2835);
  i32(2835);
  u32(0);
  u32(0);
  for (int y = h - 1; y >= 0; --y) {
    std::memset(row.data(), 0, row.size());
    std::memcpy(row.data(), bgr.data() + static_cast<size_t>(y) * w * 3,
                static_cast<size_t>(w) * 3);
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

bool read_bmp_bgr(const char* path, int* w, int* h, std::vector<uint8_t>* bgr) {
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    return false;
  }
  unsigned char hdr[54];
  if (std::fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
    std::fclose(f);
    return false;
  }
  auto u16 = [&](int o) { return static_cast<int>(hdr[o] | (hdr[o + 1] << 8)); };
  auto u32 = [&](int o) {
    return hdr[o] | (hdr[o + 1] << 8) | (hdr[o + 2] << 16) | (hdr[o + 3] << 24);
  };
  const int bits = u16(28);
  const uint32_t off = u32(10);
  int32_t width = static_cast<int32_t>(u32(18));
  int32_t height = static_cast<int32_t>(u32(22));
  bool flip = false;
  if (height < 0) {
    height = -height;
    flip = true;
  }
  if (width <= 0 || height <= 0 || (bits != 24 && bits != 32)) {
    std::fclose(f);
    return false;
  }
  const int bpp = bits / 8;
  const int rowb = (width * bpp + 3) & ~3;
  std::fseek(f, static_cast<long>(off), SEEK_SET);
  std::vector<uint8_t> raw(static_cast<size_t>(rowb) * static_cast<size_t>(height));
  if (std::fread(raw.data(), 1, raw.size(), f) != raw.size()) {
    std::fclose(f);
    return false;
  }
  std::fclose(f);
  *w = width;
  *h = height;
  // Paper is white. 32-bit BMP from sips keeps PNG alpha; transparent
  // pixels are typically RGB 0 with A 0, which must not become toner.
  bgr->assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3, 255);
  for (int y = 0; y < height; ++y) {
    const int src_y = flip ? y : (height - 1 - y);
    const uint8_t* src = raw.data() + static_cast<size_t>(src_y) * rowb;
    uint8_t* dst = bgr->data() + static_cast<size_t>(y) * width * 3;
    for (int x = 0; x < width; ++x) {
      const uint8_t* px = src + x * bpp;
      if (bits == 32) {
        const int a = px[3];
        for (int c = 0; c < 3; ++c) {
          dst[x * 3 + c] = static_cast<uint8_t>(
              (px[c] * a + 255 * (255 - a) + 127) / 255);
        }
      } else {
        dst[x * 3 + 0] = px[0];
        dst[x * 3 + 1] = px[1];
        dst[x * 3 + 2] = px[2];
      }
    }
  }
  return true;
}

void fill_rect(std::vector<uint8_t>& rgb, int w, int h, int x0, int y0, int x1,
               int y1, uint8_t r, uint8_t g, uint8_t b) {
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  x1 = std::min(w, x1);
  y1 = std::min(h, y1);
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      uint8_t* p = rgb.data() + (static_cast<size_t>(y) * w + x) * 3;
      p[0] = r;
      p[1] = g;
      p[2] = b;
    }
  }
}

void make_chart(int w, int h, std::vector<uint8_t>* rgb) {
  rgb->assign(static_cast<size_t>(w) * h * 3, 255);
  // Horizontal ramp.
  for (int x = 0; x < w; ++x) {
    const uint8_t g = static_cast<uint8_t>(x * 255 / std::max(1, w - 1));
    fill_rect(*rgb, w, h, x, 20, x + 1, 120, g, g, g);
  }
  // 25 / 50 / 75 % bands.
  fill_rect(*rgb, w, h, 40, 150, w / 3, 280, 191, 191, 191);
  fill_rect(*rgb, w, h, w / 3, 150, (2 * w) / 3, 280, 128, 128, 128);
  fill_rect(*rgb, w, h, (2 * w) / 3, 150, w - 40, 280, 64, 64, 64);
  // Colour patches (should become different greys, not solid black).
  const uint8_t cols[][3] = {
      {255, 0, 0},   {0, 255, 0},   {0, 0, 255},   {255, 255, 0},
      {0, 255, 255}, {255, 0, 255}, {255, 128, 0}, {128, 128, 255},
  };
  const int pw = w / 8;
  for (int i = 0; i < 8; ++i) {
    fill_rect(*rgb, w, h, i * pw, 310, (i + 1) * pw - 8, 430, cols[i][0],
              cols[i][1], cols[i][2]);
  }
  // Soft radial "photo".
  const int cx = w / 2;
  const int cy = 700;
  for (int y = 460; y < h - 40; ++y) {
    for (int x = 40; x < w - 40; ++x) {
      const float dx = static_cast<float>(x - cx) / 280.0f;
      const float dy = static_cast<float>(y - cy) / 280.0f;
      float t = dx * dx + dy * dy;
      if (t > 1.0f) {
        t = 1.0f;
      }
      const uint8_t g = static_cast<uint8_t>(40 + (1.0f - t) * 200);
      uint8_t* p = rgb->data() + (static_cast<size_t>(y) * w + x) * 3;
      p[0] = g;
      p[1] = static_cast<uint8_t>(g * 9 / 10);
      p[2] = static_cast<uint8_t>(g * 8 / 10);
    }
  }
}

enum class Screen { kAtkinson, kClusteredDot45 };

void halftone_rgb(const std::vector<uint8_t>& rgb, int w, int h, Screen screen,
                  unsigned cell, std::vector<uint8_t>* paper_bgr) {
  paper_bgr->assign(static_cast<size_t>(w) * h * 3, 255);
  std::vector<uint8_t> toner(static_cast<size_t>(w) * static_cast<size_t>(h));
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = rgb.data() + static_cast<size_t>(y) * w * 3;
    for (int x = 0; x < w; ++x) {
      const uint8_t b = row[x * 3 + 0];
      const uint8_t g = row[x * 3 + 1];
      const uint8_t r = row[x * 3 + 2];
      toner[static_cast<size_t>(y) * w + x] = sisterhl2030::rgb_to_toner(r, g, b);
    }
  }
  if (screen == Screen::kAtkinson) {
    sisterhl2030::atkinson(toner.data(), static_cast<unsigned>(w),
                          static_cast<unsigned>(h));
  } else {
    sisterhl2030::clustered_dot_45(toner.data(), static_cast<unsigned>(w),
                                  static_cast<unsigned>(h), cell);
  }
  for (int y = 0; y < h; ++y) {
    uint8_t* dst = paper_bgr->data() + static_cast<size_t>(y) * w * 3;
    const uint8_t* trow = toner.data() + static_cast<size_t>(y) * w;
    for (int x = 0; x < w; ++x) {
      const uint8_t v = trow[x] >= 128 ? 0 : 255;
      dst[x * 3 + 0] = v;
      dst[x * 3 + 1] = v;
      dst[x * 3 + 2] = v;
    }
  }
}

void panels_side_by_side(const std::vector<const std::vector<uint8_t>*>& panels,
                         int w, int h, std::vector<uint8_t>* out, int* ow,
                         int* oh) {
  const int gap = 16;
  const int n = static_cast<int>(panels.size());
  *ow = w * n + gap * (n - 1);
  *oh = h;
  out->assign(static_cast<size_t>(*ow) * *oh * 3, 230);
  for (int y = 0; y < h; ++y) {
    for (int i = 0; i < n; ++i) {
      std::memcpy(out->data() + (static_cast<size_t>(y) * *ow + i * (w + gap)) * 3,
                  panels[i]->data() + static_cast<size_t>(y) * w * 3,
                  static_cast<size_t>(w) * 3);
    }
  }
}

bool sips_to_bmp(const char* in, const char* out) {
  std::string cmd = "sips -s format bmp --out ";
  cmd += "'";
  cmd += out;
  cmd += "' '";
  cmd += in;
  cmd += "' >/dev/null";
  return std::system(cmd.c_str()) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool chart = false;
  const char* input = nullptr;
  std::string output;
  unsigned cell = 5;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
      usage();
      return 0;
    }
    if (std::strcmp(a, "--chart") == 0) {
      chart = true;
    } else if (std::strcmp(a, "--cell") == 0) {
      if (i + 1 >= argc) {
        usage();
        return 2;
      }
      cell = static_cast<unsigned>(std::atoi(argv[++i]));
    } else if (std::strcmp(a, "-o") == 0) {
      if (i + 1 >= argc) {
        usage();
        return 2;
      }
      output = argv[++i];
    } else if (a[0] != '-') {
      input = a;
    } else {
      usage();
      return 2;
    }
  }

  if (output.empty()) {
    output = "/tmp/sister-halftone-preview.bmp";
  }

  int w = 1200;
  int h = 900;
  std::vector<uint8_t> rgb;
  if (input) {
    char tmp[] = "/tmp/sister-preview-in-XXXXXX.bmp";
    const int fd = mkstemp(tmp);
    if (fd < 0) {
      std::perror("mkstemp");
      return 1;
    }
    close(fd);
    std::remove(tmp);
    if (!sips_to_bmp(input, tmp)) {
      std::cerr << "sips failed to convert " << input << "\n";
      return 1;
    }
    if (!read_bmp_bgr(tmp, &w, &h, &rgb)) {
      std::cerr << "could not read converted BMP\n";
      return 1;
    }
    std::remove(tmp);
    // BMP is BGR; rgb_to_toner wants R,G,B. Convert in place to RGB order
    // stored as R,G,B in the same buffer layout we call "rgb" but the BMP
    // reader stored B,G,R. halftone_rgb currently treats [0]=B [1]=G [2]=R
    // and passes (r,g,b) correctly if we keep BGR and swap at call site.
  } else {
    chart = true;
    make_chart(w, h, &rgb);
    // chart is stored RGB in R,G,B order — convert to BGR for the same path.
    for (size_t i = 0; i < rgb.size(); i += 3) {
      std::swap(rgb[i], rgb[i + 2]);
    }
  }

  std::vector<uint8_t> atk;
  halftone_rgb(rgb, w, h, Screen::kAtkinson, cell, &atk);
  std::vector<uint8_t> am;
  halftone_rgb(rgb, w, h, Screen::kClusteredDot45, cell, &am);

  int ow = 0;
  int oh = 0;
  std::vector<uint8_t> all;
  panels_side_by_side({&rgb, &atk, &am}, w, h, &all, &ow, &oh);
  if (!write_bmp_bgr(output.c_str(), ow, oh, all)) {
    std::perror(output.c_str());
    return 1;
  }
  std::cerr << "Wrote " << output << " (" << ow << "x" << oh
            << "). Left = original, middle = Atkinson, right = AM 45° "
            << "clustered-dot (cell " << cell << "), as on paper.\n";
  if (chart) {
    std::cerr << "Chart: ramp, 25/50/75% gray, colour patches, soft disc.\n"
              << "If the panels read as gray photos, the algorithms are fine\n"
              << "and a too-dark print is polarity in the CUPS raster.\n";
  }
  return 0;
}
