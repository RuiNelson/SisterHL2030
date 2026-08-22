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

bool option_is_on(const char* options, const char* key) {
  if (!options || !key) {
    return false;
  }
  const std::string pat = std::string(key) + "=";
  const char* p = std::strstr(options, pat.c_str());
  if (!p) {
    return false;
  }
  p += pat.size();
  return std::strncmp(p, "On", 2) == 0 || std::strncmp(p, "true", 4) == 0 ||
         std::strncmp(p, "TRUE", 4) == 0;
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

bool pixel_is_black(const cups_page_header2_t& h, const unsigned char* pixel) {
  const int bpp = static_cast<int>(h.cupsBitsPerPixel);
  const int bpc = static_cast<int>(h.cupsBitsPerColor);
  if (bpp == 1 || (bpc == 1 && h.cupsNumColors <= 1)) {
    return false;  // handled as packed bits
  }
  if (h.cupsColorSpace == CUPS_CSPACE_K || h.cupsColorSpace == CUPS_CSPACE_W ||
      h.cupsColorSpace == CUPS_CSPACE_SW || h.cupsColorSpace == CUPS_CSPACE_WHITE ||
      h.cupsColorSpace == CUPS_CSPACE_GRAYE || h.cupsNumColors <= 1) {
    unsigned v = pixel[0];
    if (bpc == 16) {
      v = (pixel[0] << 8) | pixel[1];
      if (h.cupsColorSpace == CUPS_CSPACE_K) {
        return v >= 32768;
      }
      return v < 32768;  // DeviceGray: 0 is black
    }
    if (h.cupsColorSpace == CUPS_CSPACE_K) {
      return v >= 128;  // 0 = white
    }
    return v < 128;  // DeviceGray: 0 = black
  }
  // RGB-ish: toner if not near-white.
  const int r = pixel[0];
  const int g = h.cupsNumColors > 1 ? pixel[1] : r;
  const int b = h.cupsNumColors > 2 ? pixel[2] : r;
  return (r + g + b) < 384;
}

void pack_row(const cups_page_header2_t& h, const unsigned char* src,
              std::vector<uint8_t>& dst) {
  const unsigned width = h.cupsWidth;
  const unsigned out_bpl = (width + 7) / 8;
  dst.assign(out_bpl, 0);

  if (h.cupsBitsPerPixel == 1 && h.cupsNumColors <= 1) {
    const unsigned in_bpl = h.cupsBytesPerLine;
    const unsigned n = std::min(in_bpl, out_bpl);
    std::memcpy(dst.data(), src, n);
    if (h.cupsColorSpace == CUPS_CSPACE_W ||
        h.cupsColorSpace == CUPS_CSPACE_SW) {
      for (unsigned i = 0; i < n; ++i) {
        dst[i] = static_cast<uint8_t>(~dst[i]);
      }
    }
    return;
  }

  const unsigned bytes_pp = std::max(1u, h.cupsBitsPerPixel / 8);
  for (unsigned x = 0; x < width; ++x) {
    if (pixel_is_black(h, src + x * bytes_pp)) {
      dst[x / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
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
    params.resolution = pjl_resolution(header);
    params.copies = static_cast<int>(std::max(1u, header.NumCopies));
    params.economode = option_is_on(options, "TonerSaveMode");
    params.papersize = pjl_paper(header);
    params.mediatype = pjl_media(header.MediaType, header.cupsMediaType);

    raw_line.resize(header.cupsBytesPerLine);
    packed.resize((header.cupsWidth + 7) / 8);
    unsigned row = 0;
    auto next_line = [&](std::vector<uint8_t>& buf) {
      if (interrupted || row >= header.cupsHeight) {
        return false;
      }
      if (cupsRasterReadPixels(ras, raw_line.data(), header.cupsBytesPerLine) !=
          header.cupsBytesPerLine) {
        return false;
      }
      pack_row(header, raw_line.data(), packed);
      buf = packed;
      ++row;
      return true;
    };

    job.encode_page(params, static_cast<int>(header.cupsHeight),
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
