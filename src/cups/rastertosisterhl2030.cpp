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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "encoder/halftone.h"
#include "encoder/job.h"

namespace {

// CUPS sends SIGTERM when the job is cancelled. Checked between pages.
volatile sig_atomic_t interrupted = 0;

void on_sigterm(int) { interrupted = 1; }

// The screen this build prints with, and the darkness numbers calibrated for
// it. Constexpr, so the other screen folds out of the binary.
constexpr sisterhl2030::ScreenCalibration kScreen = sisterhl2030::active_screen();

// ASCII uppercase. Option tokens from CUPS arrive in mixed case.
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

// CUPS MediaType string or cupsMediaType enum → PJL MEDIATYPE. The string
// wins when present; the numeric fallback is the cupsMediaType table.
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

// cupsPageSizeName, then PageSize in points, then A4. Names are uppercased
// so "Letter" and "LETTER" both match.
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
  if (name == "FOLIO") {
    return "FOLIO";
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

// Raster HWResolution → 300 / 600 / 1200. Thresholds sit halfway between
// the advertised dpi values so a slightly-off header still classifies.
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

// Quality: draft=300 dpi, normal=600 dpi, high=HQ1200.
// Draft and Normal also set ECONOMODE; Best (HQ1200) uses full toner.
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

// Short name for the CUPS log line: 300dpi / 600dpi / HQ1200.
const char* mode_name(int dpi) {
  if (dpi >= 900) {
    return "HQ1200";
  }
  if (dpi >= 450) {
    return "600dpi";
  }
  return "300dpi";
}

// First byte of a sample. 16-bit raster is treated as high-byte-only.
uint8_t sample8(const unsigned char* p, int bpc) {
  if (bpc >= 16) {
    return p[0];  // high byte of 16-bit big-endian-ish sample
  }
  return p[0];
}

// One CUPS pixel → toner 0..255, dispatching on cupsColorSpace.
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

// Convert one 8/16/24-bit raster row into a toner row of cupsWidth samples.
void row_to_toner(const cups_page_header2_t& h, const unsigned char* src,
                  std::vector<uint8_t>& toner) {
  const unsigned width = h.cupsWidth;
  toner.resize(width);
  const unsigned bytes_pp = std::max(1u, h.cupsBitsPerPixel / 8);
  for (unsigned x = 0; x < width; ++x) {
    toner[x] = toner_at(h, src + x * bytes_pp);
  }
}

// Copy a 1-bit CUPS row into packed MSB-first 1=black. DeviceW/sGray is
// inverted (CUPS 1 = white).
void pack_1bit_row(const cups_page_header2_t& h, const unsigned char* src,
                   std::vector<uint8_t>& dst) {
  const unsigned width = h.cupsWidth;
  const unsigned out_bpl = (width + 7) / 8;
  dst.assign(out_bpl, 0);
  const unsigned n = std::min(h.cupsBytesPerLine, out_bpl);
  std::memcpy(dst.data(), src, n);
  // The same three white-is-one spaces the PAPPL app inverts in
  // `line_to_toner`; the two paths have to agree on what a set bit means.
  if (h.cupsColorSpace == CUPS_CSPACE_W || h.cupsColorSpace == CUPS_CSPACE_SW ||
      h.cupsColorSpace == CUPS_CSPACE_WHITE) {
    for (unsigned i = 0; i < n; ++i) {
      dst[i] = static_cast<uint8_t>(~dst[i]);
    }
  }
  // The tail of the last byte is padding, not pixels, and inverting it turned
  // white padding into up to seven black dots printed past the right edge of
  // every line. The engine's imageable widths are exactly the widths that are
  // not a multiple of 8 -- A4 is 4661 px at 600 dpi -- so this is the normal
  // case, not a corner one. Carry bits for x < width only, which is the
  // convention `pack_toner_row` packs to.
  const unsigned tail = width % 8;
  if (tail != 0 && out_bpl != 0) {
    dst[out_bpl - 1] =
        static_cast<uint8_t>(dst[out_bpl - 1] & (0xFF << (8 - tail)));
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
    {
      std::string tray = upper(option_value(options, "InputSlot"));
      if (tray.empty()) {
        tray = upper(option_value(options, "media-source"));
      }
      if (tray == "MANUAL" || tray == "MPTRAY" || tray == "BY-PASS-TRAY" ||
          tray == "BYPASS") {
        params.sourcetray = "MANUAL";
      }
    }

    const bool already_1bit =
        header.cupsBitsPerPixel == 1 && header.cupsNumColors <= 1;
    // Same device grid and same darkness model as the PAPPL app: this filter
    // is the reference path for comparing encoder output, so it has to make
    // the same bits.
    const sisterhl2030::DeviceGrid grid =
        sisterhl2030::device_grid(params.resolution);
    const int raster_dpi = pjl_resolution(header);
    const bool resampling =
        raster_dpi != grid.dpi_x || raster_dpi != grid.dpi_y;
    std::fprintf(stderr,
                 "INFO: page %ux%u %ubit/%u colorspace=%u 1bit=%s "
                 "mode=%s ECONOMODE=%s\n",
                 header.cupsWidth, header.cupsHeight, header.cupsBitsPerPixel,
                 header.cupsNumColors, header.cupsColorSpace,
                 already_1bit ? "passthrough" : kScreen.name,
                 mode_name(params.resolution),
                 params.economode ? "ON" : "OFF");

    raw_line.resize(header.cupsBytesPerLine);

    unsigned out_w = header.cupsWidth;
    unsigned out_h = header.cupsHeight;
    std::vector<uint8_t> page_bits;

    // Every page goes through the toner buffer, 1-bit rasters included. A
    // 1-bit page that needs no resample could be packed straight across, but
    // then there is nowhere to crop it to the imageable area, and that crop is
    // not optional (see below). Going the long way costs nothing and changes
    // nothing: the row becomes toner 0/255, no resample runs, neither the
    // dither nor the re-threshold runs, and `pack_toner_row`'s >= 128 test
    // maps 255 back to 1 and 0 back to 0.
    {
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
      if (resampling) {
        sisterhl2030::resample_to_grid(toner, out_w, out_h, raster_dpi, grid);
        std::fprintf(stderr, "INFO: resampled to the %dx%d dpi grid, %ux%u\n",
                     grid.dpi_x, grid.dpi_y, out_w, out_h);
      }
      // Then down to what the engine can actually paint, in the same place
      // `rendpage` does it. The driver claims 0.01 mm margins so the print
      // dialog cannot scale-to-fit, and the price is that CUPS rasterises the
      // whole sheet; the surplus overruns the band decoder's line buffer and
      // comes back as blank scanlines. See crop_to_imageable in
      // encoder/halftone.h.
      {
        // The crop wants hundredths of a millimetre and the header carries
        // points. cupsPageSize is the precise one -- A4 is 595.276 pt, which
        // is the 21000 the PAPPL app passes, while rounding PageSize's whole
        // 595 pt would crop two pixels away from it.
        const double pt_w = header.cupsPageSize[0] != 0.0f
                                ? static_cast<double>(header.cupsPageSize[0])
                                : static_cast<double>(header.PageSize[0]);
        const double pt_h = header.cupsPageSize[1] != 0.0f
                                ? static_cast<double>(header.cupsPageSize[1])
                                : static_cast<double>(header.PageSize[1]);
        const int sheet_w = static_cast<int>(std::lround(pt_w * 2540.0 / 72.0));
        const int sheet_h = static_cast<int>(std::lround(pt_h * 2540.0 / 72.0));
        const unsigned sheet_pixels_w = out_w;
        const unsigned sheet_pixels_h = out_h;
        sisterhl2030::crop_to_imageable(toner, out_w, out_h, sheet_w, sheet_h,
                                        grid);
        if (out_w != sheet_pixels_w || out_h != sheet_pixels_h) {
          std::fprintf(stderr,
                       "INFO: cropped the %ux%u sheet to the engine's "
                       "imageable area, %ux%u (%u bytes/line, was %u)\n",
                       sheet_pixels_w, sheet_pixels_h, out_w, out_h,
                       (out_w + 7) / 8, (sheet_pixels_w + 7) / 8);
        }
      }
      if (!already_1bit || resampling) {
        if (!already_1bit) {
          const double mean =
              static_cast<double>(toner_sum) /
              (static_cast<double>(header.cupsWidth) * header.cupsHeight);
          std::fprintf(stderr, "INFO: mean toner before dither %.1f / 255\n",
                       mean);
          const sisterhl2030::DotTransfer dt =
              sisterhl2030::engine_transfer(kScreen, grid);
          const std::vector<uint8_t> transfer =
              sisterhl2030::dot_transfer_lut(dt);
          std::fprintf(stderr,
                       "INFO: %s dot transfer %dx%d dpi gain %.2f density "
                       "%.2f ink limit %.0f%%, 25/50/75%% ask for %u/%u/%u\n",
                       kScreen.name, grid.dpi_x, grid.dpi_y,
                       sisterhl2030::contrast_gain(dt), dt.density,
                       dt.max_toner * 100.0f,
                       transfer[64], transfer[128], transfer[191]);
          sisterhl2030::apply_transfer(toner.data(), toner.size(), transfer);
          if (kScreen.clustered) {
            sisterhl2030::clustered_dot_45(toner.data(), out_w, out_h,
                                           kScreen.cell);
          } else {
            sisterhl2030::atkinson(toner.data(), out_w, out_h);
          }
        } else {
          // 1-bit resampled onto another grid: the box average produced
          // 0..255 again, so re-threshold.
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

    // HQ1200's bitmap is twice the 600 dpi page vertically as well, but the
    // engine only resolves 1200 across the scan, so each dithered row of the
    // 1200x600 grid goes out `repeat` times.
    const unsigned repeat =
        grid.dpi_y > 0
            ? static_cast<unsigned>(std::max(1, params.resolution / grid.dpi_y))
            : 1u;
    unsigned row = 0;
    unsigned dup = 0;
    auto next_line = [&](std::vector<uint8_t>& buf) {
      if (row >= out_h) {
        return false;
      }
      buf.assign(page_bits.data() + static_cast<size_t>(row) * packed.size(),
                 page_bits.data() + static_cast<size_t>(row + 1) * packed.size());
      if (++dup >= repeat) {
        dup = 0;
        ++row;
      }
      return true;
    };

    job.encode_page(params, static_cast<int>(out_h * repeat),
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
