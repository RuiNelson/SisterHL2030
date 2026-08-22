// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// CUPS filter: application/vnd.cups-raster → Brother HL-2030 mode-1030.

#include <cups/raster.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "encoder/halftone.h"
#include "encoder/job.h"

namespace {

volatile sig_atomic_t interrupted = 0;

void on_sigterm(int) { interrupted = 1; }

std::string upper(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return s;
}

// Value of `key=` in a CUPS option string. Keys match as whole tokens
// so `media` does not steal `media-type`.
std::string option_value(const char* options, const char* key) {
  if (!options || !key || !*key) {
    return {};
  }
  const size_t klen = std::strlen(key);
  const char* p = options;
  while (*p) {
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (std::strncmp(p, key, klen) == 0 && p[klen] == '=') {
      p += klen + 1;
      const char* end = p;
      while (*end && *end != ' ' && *end != '\t') {
        ++end;
      }
      return std::string(p, static_cast<size_t>(end - p));
    }
    while (*p && *p != ' ' && *p != '\t') {
      ++p;
    }
  }
  return {};
}

std::string pjl_media(const char* media_type, unsigned cups_media) {
  std::string m = media_type ? media_type : "";
  m = upper(m);
  if (m == "THIN") {
    return "THIN";
  }
  if (m == "THICK") {
    return "THICK";
  }
  if (m == "THICKPAPER2" || m == "THICKER" || m == "THICKERPAPER2") {
    return "THICK2";
  }
  if (m == "TRANSPARENCY" || m == "TRANSPARENCIES" || m == "TRANS") {
    return "TRANSPARENCY";
  }
  if (m == "ENV" || m == "ENVELOPES") {
    return "ENVELOPES";
  }
  if (m == "ENVTHICK" || m == "ENV.THICK") {
    return "ENVTHICK";
  }
  if (m == "ENVTHIN" || m == "ENV.THIN") {
    return "ENVTHIN";
  }
  if (m == "RECYCLED") {
    return "RECYCLED";
  }
  switch (cups_media) {
    case 2:
      return "THIN";
    case 3:
      return "THICK";
    case 4:
      return "THICK2";
    case 6:
      return "TRANSPARENCY";
    case 7:
      return "ENVELOPES";
    case 8:
      return "ENVTHICK";
    case 9:
      return "ENVTHIN";
    case 10:
      return "RECYCLED";
    default:
      return "REGULAR";
  }
}

std::string pjl_paper(const cups_page_header2_t& h) {
  std::string name = h.cupsPageSizeName[0] ? h.cupsPageSizeName : "";
  name = upper(name);
  if (name == "LETTER" || name == "USLETTER") {
    return "LETTER";
  }
  if (name == "LEGAL") {
    return "LEGAL";
  }
  if (name == "EXECUTIVE" || name == "EXECTIVE") {
    return "EXECUTIVE";
  }
  if (name == "A4" || name == "A5" || name == "A6" || name == "B5" ||
      name == "B6" || name == "C5" || name == "DL") {
    return name;
  }
  if (name.find("MONARCH") != std::string::npos) {
    return "MONARCH";
  }
  if (name.find("ENV10") != std::string::npos || name == "COM10") {
    return "COM10";
  }
  // Fall back on page size in points.
  const unsigned w = h.PageSize[0];
  const unsigned l = h.PageSize[1];
  if (w >= 580 && w <= 610 && l >= 820 && l <= 860) {
    return "A4";
  }
  if (w >= 600 && w <= 625 && l >= 780 && l <= 805) {
    return "LETTER";
  }
  return name.empty() ? "A4" : name;
}

int pjl_resolution(const cups_page_header2_t& h) {
  const unsigned dpi = h.HWResolution[0];
  if (dpi >= 900) {
    return 1200;
  }
  if (dpi >= 450) {
    return 600;
  }
  return 300;
}

// Qualidade: draft=300 dpi, normal=600 dpi, high=HQ1200.
// Rascunho and Normal also set ECONOMODE; Alta (HQ1200) uses full toner.
int requested_resolution(const char* options, const cups_page_header2_t& h) {
  std::string q = upper(option_value(options, "print-quality"));
  if (q.empty()) {
    q = upper(option_value(options, "cupsPrintQuality"));
  }
  if (q == "DRAFT" || q == "3") {
    return 300;
  }
  if (q == "HIGH" || q == "5" || q == "BEST") {
    return 1200;
  }
  if (q == "NORMAL" || q == "4") {
    return 600;
  }
  const std::string pr = option_value(options, "printer-resolution");
  if (pr.find("1200") != std::string::npos) {
    return 1200;
  }
  if (pr.find("300") != std::string::npos) {
    return 300;
  }
  if (pr.find("600") != std::string::npos) {
    return 600;
  }
  return pjl_resolution(h);
}

const char* mode_name(int dpi) {
  if (dpi >= 900) {
    return "HQ1200";
  }
  if (dpi >= 450) {
    return "600dpi";
  }
  return "300dpi";
}

uint8_t sample8(const unsigned char* p, int bpc) {
  if (bpc >= 16) {
    return p[0];  // high byte of 16-bit big-endian-ish sample
  }
  return p[0];
}

uint8_t toner_at(const cups_page_header2_t& h, const unsigned char* pixel) {
  const int bpc = static_cast<int>(h.cupsBitsPerColor);
  const unsigned n = std::max(1u, h.cupsNumColors);

  if (h.cupsColorSpace == CUPS_CSPACE_K) {
    return sisterhl2030::device_k_to_toner(sample8(pixel, bpc));
  }
  if (h.cupsColorSpace == CUPS_CSPACE_W || h.cupsColorSpace == CUPS_CSPACE_SW ||
      h.cupsColorSpace == CUPS_CSPACE_WHITE ||
      h.cupsColorSpace == CUPS_CSPACE_GRAYE || n == 1) {
    return sisterhl2030::device_gray_to_toner(sample8(pixel, bpc));
  }

  const int bytes_pc = std::max(1, bpc / 8);
  const uint8_t r = sample8(pixel, bpc);
  const uint8_t g = n > 1 ? sample8(pixel + bytes_pc, bpc) : r;
  const uint8_t b = n > 2 ? sample8(pixel + 2 * bytes_pc, bpc) : r;
  return sisterhl2030::rgb_to_toner(r, g, b);
}

void row_to_toner(const cups_page_header2_t& h, const unsigned char* src,
                  std::vector<uint8_t>& toner) {
  const unsigned width = h.cupsWidth;
  toner.resize(width);
  const unsigned bytes_pp = std::max(1u, h.cupsBitsPerPixel / 8);
  for (unsigned x = 0; x < width; ++x) {
    toner[x] = toner_at(h, src + x * bytes_pp);
  }
}

void pack_1bit_row(const cups_page_header2_t& h, const unsigned char* src,
                   std::vector<uint8_t>& dst) {
  const unsigned width = h.cupsWidth;
  const unsigned out_bpl = (width + 7) / 8;
  dst.assign(out_bpl, 0);
  const unsigned n = std::min(h.cupsBytesPerLine, out_bpl);
  std::memcpy(dst.data(), src, n);
  if (h.cupsColorSpace == CUPS_CSPACE_W || h.cupsColorSpace == CUPS_CSPACE_SW) {
    for (unsigned i = 0; i < n; ++i) {
      dst[i] = static_cast<uint8_t>(~dst[i]);
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  setbuf(stderr, nullptr);
  std::fprintf(stderr, "INFO: SisterHL2030 raster filter\n");

  if (argc != 6 && argc != 7) {
    std::fprintf(stderr,
                 "ERROR: rastertosisterhl2030 job-id user title copies options "
                 "[file]\n");
    return 1;
  }

  const char* job_title = argv[3];
  const char* options = argv[5];
  const char* filename = argc == 7 ? argv[6] : nullptr;

  signal(SIGTERM, on_sigterm);
  signal(SIGPIPE, SIG_IGN);

  int fd = STDIN_FILENO;
  if (filename) {
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
      std::fprintf(stderr, "ERROR: Unable to open raster file\n");
      return 1;
    }
  }

  cups_raster_t* ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  if (!ras) {
    std::fprintf(stderr, "ERROR: Cannot read CUPS raster data\n");
    return 1;
  }

  sisterhl2030::Job job(stdout, job_title ? job_title : "SisterHL2030");
  cups_page_header2_t header;
  std::vector<uint8_t> raw_line;
  std::vector<uint8_t> packed;

  while (!interrupted && cupsRasterReadHeader2(ras, &header)) {
    if (header.cupsWidth == 0 || header.cupsHeight == 0 ||
        header.cupsBytesPerLine == 0 || header.cupsBytesPerLine > 65536) {
      std::fprintf(stderr, "ERROR: Bogus raster header\n");
      cupsRasterClose(ras);
      return 1;
    }

    sisterhl2030::PageParams params;
    params.resolution = requested_resolution(options, header);
    params.copies = static_cast<int>(std::max(1u, header.NumCopies));
    params.economode = params.resolution < 900;
    params.papersize = pjl_paper(header);
    params.mediatype = pjl_media(header.MediaType, header.cupsMediaType);

    const bool already_1bit =
        header.cupsBitsPerPixel == 1 && header.cupsNumColors <= 1;
    const bool down_to_300 =
        params.resolution == 300 && pjl_resolution(header) >= 450;
    std::fprintf(stderr,
                 "INFO: page %ux%u %ubit/%u colorspace=%u 1bit=%s "
                 "mode=%s ECONOMODE=%s\n",
                 header.cupsWidth, header.cupsHeight, header.cupsBitsPerPixel,
                 header.cupsNumColors, header.cupsColorSpace,
                 already_1bit ? "passthrough" : "Floyd-Steinberg",
                 mode_name(params.resolution),
                 params.economode ? "ON" : "OFF");

    raw_line.resize(header.cupsBytesPerLine);

    unsigned out_w = header.cupsWidth;
    unsigned out_h = header.cupsHeight;
    std::vector<uint8_t> page_bits;

    if (already_1bit && !down_to_300) {
      packed.resize((out_w + 7) / 8);
      page_bits.resize(packed.size() * out_h);
      for (unsigned y = 0; y < out_h; ++y) {
        if (cupsRasterReadPixels(ras, raw_line.data(), header.cupsBytesPerLine) !=
            header.cupsBytesPerLine) {
          std::fprintf(stderr, "ERROR: short raster read\n");
          cupsRasterClose(ras);
          return 1;
        }
        pack_1bit_row(header, raw_line.data(), packed);
        std::memcpy(page_bits.data() + static_cast<size_t>(y) * packed.size(),
                    packed.data(), packed.size());
      }
    } else {
      std::vector<uint8_t> toner(static_cast<size_t>(out_w) * out_h);
      std::vector<uint8_t> row_toner;
      long toner_sum = 0;
      for (unsigned y = 0; y < header.cupsHeight; ++y) {
        if (cupsRasterReadPixels(ras, raw_line.data(), header.cupsBytesPerLine) !=
            header.cupsBytesPerLine) {
          std::fprintf(stderr, "ERROR: short raster read\n");
          cupsRasterClose(ras);
          return 1;
        }
        if (already_1bit) {
          packed.resize((header.cupsWidth + 7) / 8);
          pack_1bit_row(header, raw_line.data(), packed);
          for (unsigned x = 0; x < header.cupsWidth; ++x) {
            const bool on =
                (packed[x / 8] & static_cast<uint8_t>(0x80 >> (x % 8))) != 0;
            toner[static_cast<size_t>(y) * header.cupsWidth + x] =
                on ? 255 : 0;
          }
        } else {
          row_to_toner(header, raw_line.data(), row_toner);
          std::memcpy(toner.data() + static_cast<size_t>(y) * header.cupsWidth,
                      row_toner.data(), header.cupsWidth);
          for (uint8_t t : row_toner) {
            toner_sum += t;
          }
        }
      }
      if (down_to_300) {
        sisterhl2030::box_downsample_2x(toner, out_w, out_h);
        std::fprintf(stderr, "INFO: draft downsample to %ux%u (300 dpi)\n",
                     out_w, out_h);
      }
      if (!already_1bit || down_to_300) {
        if (!already_1bit) {
          const double mean =
              static_cast<double>(toner_sum) /
              (static_cast<double>(header.cupsWidth) * header.cupsHeight);
          std::fprintf(stderr, "INFO: mean toner before dither %.1f / 255\n",
                       mean);
          sisterhl2030::floyd_steinberg(toner.data(), out_w, out_h);
        } else {
          // 1-bit draft: 2×2 box already produced 0..255; re-threshold.
          for (uint8_t& t : toner) {
            t = t >= 128 ? 255 : 0;
          }
        }
      }
      packed.resize((out_w + 7) / 8);
      page_bits.resize(packed.size() * out_h);
      for (unsigned y = 0; y < out_h; ++y) {
        sisterhl2030::pack_toner_row(
            toner.data() + static_cast<size_t>(y) * out_w, out_w,
            packed.data());
        std::memcpy(page_bits.data() + static_cast<size_t>(y) * packed.size(),
                    packed.data(), packed.size());
      }
    }

    unsigned row = 0;
    auto next_line = [&](std::vector<uint8_t>& buf) {
      if (row >= out_h) {
        return false;
      }
      buf.assign(page_bits.data() + static_cast<size_t>(row) * packed.size(),
                 page_bits.data() + static_cast<size_t>(row + 1) * packed.size());
      ++row;
      return true;
    };

    job.encode_page(params, static_cast<int>(out_h),
                    static_cast<int>(packed.size()), next_line);
    std::fprintf(stderr, "PAGE: %d %u\n", job.pages(), header.NumCopies);
  }

  cupsRasterClose(ras);
  if (fd != STDIN_FILENO) {
    close(fd);
  }
  if (ferror(stdout)) {
    std::fprintf(stderr, "ERROR: Failed to write print data\n");
    return 1;
  }
  if (job.pages() == 0) {
    std::fprintf(stderr, "ERROR: No pages in raster\n");
    return 1;
  }
  return 0;
}
