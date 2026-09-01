// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// PAPPL printer application for the Brother HL-2030.
//
// PAPPL owns the USB device, the IPP attributes and the web UI, so this
// replaces the ippeveprinter façade, its printer-attrs.conf, the CUPS usb
// backend and the device-uri file. The raster encoder in src/encoder is
// reused unchanged: PAPPL pushes one line at a time, we buffer the page (as
// the dither needs it anyway) and hand it to Job::encode_page.

#include <pappl/pappl.h>

#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <malloc/malloc.h>
#endif

#include "encoder/halftone.h"
#include "encoder/job.h"
#include "status/pjl.h"
#include "version.h"

namespace {

// PAPPL driver name (`-m` / Create-Printer). Reverse-DNS is not required
// here; the LaunchDaemon label is the one that uses com.ruinelson.
constexpr const char* kDriverName = "sister-hl2030";
// make-and-model string, and the human name in `pappl_pr_driver_t`.
constexpr const char* kDriverDesc = "Brother HL-2030 series";
// IEEE 1284 device ID. Must match the hardware so auto-add can find it.
constexpr const char* kDeviceId = "MFG:Brother;MDL:HL-2030 series;CMD:PJL,HBP;CLS:PRINTER;";
// Queue artwork. PAPPL serves the same file for all three icon sizes.
constexpr const char* kIconPath = "/Library/Printers/SisterHL2030/icon.png";
// What AirPrint clients show. The IPP printer name cannot hold spaces, so the
// friendly name is set separately -- the façade advertised the same string.
constexpr const char* kDnsSdName = "Brother HL-2030";
// A job with this name prints nothing. CUPS only copies marker-levels off the
// printer while a job runs through its backend, so without a way to run an
// empty one the macOS Supply Levels panel stays empty until the first real
// print. Submitting this costs no paper.
constexpr const char* kStatusJobName = ".sister-status";
// The screen this build prints with, and the darkness numbers calibrated for
// it. Constexpr, so the other screen folds out of the binary.
constexpr sisterhl2030::ScreenCalibration kScreen = sisterhl2030::active_screen();

// How long one status transaction may hold the USB device. It runs between
// jobs -- the print dialog asks for printer attributes immediately before it
// submits -- so every millisecond here is a millisecond the next job spends
// waiting for the device. The IOKit path in status/usb_printer.cc budgets
// ~700 ms per PJL command; these match it. Worst case is the sum plus one
// read timeout of overshoot, as the deadline is only tested between reads.
constexpr long kStatusReplyMs = 2500;
constexpr long kStatusDrainMs = 600;

// Per-job state. PAPPL hands it back to every raster callback.
struct JobState {
  bool status_only = false;  // named kStatusJobName: report status, print nothing
  FILE* stream = nullptr;    // funopen(3) wrapper around the PAPPL device
  std::unique_ptr<sisterhl2030::Job> job;
  sisterhl2030::PageParams params;
  std::vector<uint8_t> toner;  // width*height, 0 = paper white, 255 = black
  unsigned width = 0;
  unsigned height = 0;
  unsigned lines_seen = 0;  // rwriteline calls; compared to height in the log
  int raster_dpi = 600;     // dpi of the raster PAPPL handed over
  int sheet_w = 0;          // media size, hundredths of a millimetre
  int sheet_h = 0;          // ... used to crop to the engine's imageable box
};

// funopen(3) write callback: PAPPL device as a FILE* the encoder can fwrite.
int device_write(void* cookie, const char* buffer, int bytes) {
  auto* device = static_cast<pappl_device_t*>(cookie);
  const ssize_t written =
      papplDeviceWrite(device, buffer, static_cast<size_t>(bytes));
  return written < 0 ? -1 : static_cast<int>(written);
}

// PWG media size name → PJL PAPER. Unrecognised sizes become A4.
std::string pjl_paper(const char* pwg_size) {
  const std::string s = pwg_size ? pwg_size : "";
  if (s.find("na_letter") == 0) return "LETTER";
  if (s.find("na_legal") == 0) return "LEGAL";
  if (s.find("na_executive") == 0) return "EXECUTIVE";
  if (s.find("na_folio") == 0) return "FOLIO";
  if (s.find("na_number-10") == 0) return "COM10";
  if (s.find("iso_dl") == 0) return "DL";
  if (s.find("iso_a5") == 0) return "A5";
  if (s.find("iso_a6") == 0) return "A6";
  if (s.find("iso_b5") == 0 || s.find("jis_b5") == 0) return "B5";
  if (s.find("iso_b6") == 0) return "B6";
  return "A4";
}

// IPP media-source → PageParams::sourcetray (TRAY1 or MANUAL).
std::string pjl_tray(const char* source) {
  const std::string s = source ? source : "";
  if (s == "manual" || s == "by-pass-tray" || s == "manual-feed") {
    return "MANUAL";
  }
  return "TRAY1";
}

// IPP media-type → PJL MEDIATYPE. Subset of the official vocabulary;
// see docs/protocol.md. Unknown types become REGULAR.
std::string pjl_mediatype(const char* pwg_type) {
  const std::string t = pwg_type ? pwg_type : "";
  if (t == "envelope") return "ENVELOPES";
  if (t == "cardstock") return "THICK";
  if (t == "transparency") return "TRANSPARENCY";
  if (t == "labels") return "THICK";
  // Letterhead is deliberately *not* THICK. Job::wants_manual treats THICK as
  // manual-only (see "Paper source (PCL)" in docs/protocol.md), which is right
  // for cardstock and labels -- those cannot feed from the cassette -- but
  // letterhead is ordinary-weight paper, and mapping it to THICK made choosing
  // "Letterhead" in the print dialog silently park the job waiting for a
  // hand-fed sheet. It feeds from tray 1 exactly like plain stationery.
  if (t == "stationery-letterhead") return "REGULAR";
  return "REGULAR";  // "stationery", "auto", "other"
}

// Horizontal dpi of the raster PAPPL actually handed over. Prefers the
// CUPS header; falls back to the job's printer-resolution, then 600.
int raster_dpi_of(const pappl_pr_options_t* options) {
  if (options->header.HWResolution[0] > 0) {
    return static_cast<int>(options->header.HWResolution[0]);
  }
  if (options->printer_resolution[0] > 0) {
    return options->printer_resolution[0];
  }
  return 600;
}

// Quality maps the same way as the CUPS filter: see docs/protocol.md.
// raster_dpi is the bitmap the client actually sent. PJL RESOLUTION must
// match it: a 300 dpi page with RESOLUTION=600 prints at half size.
void quality_to_params(ipp_quality_t quality, int raster_dpi,
                       sisterhl2030::PageParams* params) {
  switch (quality) {
    case IPP_QUALITY_DRAFT:
      params->resolution = 300;
      params->economode = true;
      break;
    case IPP_QUALITY_HIGH:
      params->resolution = 1200;  // RAS1200MODE, raster stays 600 dpi
      params->economode = false;
      break;
    default:
      params->resolution = 600;
      params->economode = true;
      break;
  }
  if (raster_dpi > 0 && raster_dpi < 450 && params->resolution >= 600) {
    params->resolution = 300;
  }
}

// Convert one PAPPL raster line to toner 0..255. Both paths go through the
// same laser curve, and `rgb_to_toner` is built so a neutral pixel produces
// exactly what `device_gray_to_toner` produces -- a page of greys prints
// identically whether it arrives as sRGB or as sGray. (There used to be an
// extra 0.85 exponent on the sRGB path, calibrated to reproduce output from
// before the tone chain was modelled. With sRGB now the normal path it would
// only darken every neutral away from the calibration.)
void line_to_toner(const cups_page_header2_t& header, const unsigned char* line,
                   unsigned width, uint8_t* row) {
  const unsigned bpp = header.cupsBitsPerPixel;
  const cups_cspace_t cs =
      static_cast<cups_cspace_t>(header.cupsColorSpace);

  if (bpp == 1) {
    // DeviceW/sGray 1-bit is 1 = white; DeviceK is 1 = black.
    const bool invert =
        cs == CUPS_CSPACE_W || cs == CUPS_CSPACE_SW || cs == CUPS_CSPACE_WHITE;
    for (unsigned x = 0; x < width; ++x) {
      const bool on =
          (line[x / 8] & static_cast<uint8_t>(0x80 >> (x % 8))) != 0;
      row[x] = (on != invert) ? 255 : 0;
    }
    return;
  }

  if (bpp >= 24) {
    const unsigned stride = bpp / 8;
    for (unsigned x = 0; x < width; ++x) {
      const unsigned char* px = line + static_cast<size_t>(x) * stride;
      row[x] = sisterhl2030::rgb_to_toner(px[0], px[1], px[2]);
    }
    return;
  }

  const unsigned stride = bpp >= 8 ? bpp / 8 : 1;
  if (cs == CUPS_CSPACE_K) {
    for (unsigned x = 0; x < width; ++x) {
      row[x] = sisterhl2030::device_k_to_toner(line[x * stride]);
    }
  } else {
    for (unsigned x = 0; x < width; ++x) {
      row[x] = sisterhl2030::device_gray_to_toner(line[x * stride]);
    }
  }
}

// Raw passthrough: the file is already a HL-2030 job stream, as produced by
// sister-rawtobr. PAPPL requires this whenever a driver advertises a raw
// format, and rejects the driver data outright without it.
bool printfile(pappl_job_t* job, pappl_pr_options_t* options,
               pappl_device_t* device) {
  (void)options;
  const char* name = papplJobGetName(job);
  if (name && std::strcmp(name, kStatusJobName) == 0) {
    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
                "Status-only job: nothing will be printed.");
    return true;
  }
  const char* filename = papplJobGetFilename(job);
  if (!filename) {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "No job file to print.");
    return false;
  }

  const int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to open '%s'.", filename);
    return false;
  }

  papplJobSetImpressions(job, 1);

  char buffer[65536];
  ssize_t got;
  bool ok = true;
  while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
    if (papplDeviceWrite(device, buffer, static_cast<size_t>(got)) < 0) {
      papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Write to the printer failed.");
      ok = false;
      break;
    }
  }
  if (got < 0) {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Read from '%s' failed.", filename);
    ok = false;
  }

  close(fd);
  papplDeviceFlush(device);
  if (ok) {
    papplJobSetImpressionsCompleted(job, 1);
  }
  return ok;
}

// PAPPL: job is starting. Allocates JobState and opens the encoder stream.
// A job named kStatusJobName prints nothing.
bool rstartjob(pappl_job_t* job, pappl_pr_options_t* options,
               pappl_device_t* device) {
  (void)options;
  auto* state = new JobState();

  const char* name = papplJobGetName(job);
  if (name && std::strcmp(name, kStatusJobName) == 0) {
    state->status_only = true;
    papplJobSetData(job, state);
    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
                "Status-only job: nothing will be printed.");
    return true;
  }

  state->stream = funopen(device, nullptr, device_write, nullptr, nullptr);
  if (!state->stream) {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to open the device stream.");
    delete state;
    return false;
  }
  state->job = std::make_unique<sisterhl2030::Job>(
      state->stream, papplJobGetName(job) ? papplJobGetName(job) : "SisterHL2030");
  papplJobSetData(job, state);
  return true;
}

// PAPPL: page is starting. Sizes the toner buffer and resolves PageParams
// from quality, media and the raster dpi.
bool rstartpage(pappl_job_t* job, pappl_pr_options_t* options,
                pappl_device_t* device, unsigned page) {
  (void)device;
  (void)page;
  auto* state = static_cast<JobState*>(papplJobGetData(job));
  if (!state) return false;
  if (state->status_only) return true;

  state->width = options->header.cupsWidth;
  state->height = options->header.cupsHeight;
  if (state->width == 0 || state->height == 0) {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Empty raster page.");
    return false;
  }

  state->raster_dpi = raster_dpi_of(options);
  quality_to_params(options->print_quality, state->raster_dpi, &state->params);
  state->params.copies = options->copies > 0 ? options->copies : 1;
  state->params.papersize = pjl_paper(options->media.size_name);
  state->params.mediatype = pjl_mediatype(options->media.type);
  state->params.sourcetray = pjl_tray(options->media.source);
  state->sheet_w = options->media.size_width;
  state->sheet_h = options->media.size_length;

  // 0 = paper white; rwriteline fills in the real toner values.
  state->lines_seen = 0;
  papplLogJob(job, PAPPL_LOGLEVEL_INFO,
              "Raster page %ux%u, %u bytes/line, %u bits/pixel, "
              "quality=%d, %d dpi.",
              options->header.cupsWidth, options->header.cupsHeight,
              options->header.cupsBytesPerLine,
              options->header.cupsBitsPerPixel,
              static_cast<int>(options->print_quality), state->raster_dpi);
  state->toner.assign(static_cast<size_t>(state->width) * state->height, 0);
  return true;
}

// PAPPL: one raster row. Converted to toner 0..255 and stored at row `y`.
bool rwriteline(pappl_job_t* job, pappl_pr_options_t* options,
                pappl_device_t* device, unsigned y, const unsigned char* line) {
  (void)options;
  (void)device;
  auto* state = static_cast<JobState*>(papplJobGetData(job));
  if (!state || state->status_only || y >= state->height) return true;

  ++state->lines_seen;
  uint8_t* row = state->toner.data() + static_cast<size_t>(y) * state->width;
  line_to_toner(options->header, line, state->width, row);
  return true;
}

// PAPPL: page is complete. Resample → transfer LUT → dither → pack → encode.
bool rendpage(pappl_job_t* job, pappl_pr_options_t* options,
              pappl_device_t* device, unsigned page) {
  (void)options;
  (void)device;
  (void)page;
  auto* state = static_cast<JobState*>(papplJobGetData(job));
  if (!state) return false;
  if (state->status_only) return true;

  unsigned width = state->width;
  unsigned height = state->height;

  // Put the contone page on the grid the engine actually addresses BEFORE
  // dithering: 300x300 for draft, 600x600 for normal, 1200x600 for HQ1200.
  // Dithering at some other pitch and rescaling the 1-bit result afterwards
  // discards the very placement precision the mode exists for -- HQ1200 in
  // particular used to dither at 600 and pixel-double, which produced 600 dpi
  // dots wearing a 1200 dpi bitmap.
  const sisterhl2030::DeviceGrid grid =
      sisterhl2030::device_grid(state->params.resolution);
  sisterhl2030::resample_to_grid(state->toner, width, height,
                                 state->raster_dpi, grid);
  if (width != state->width || height != state->height) {
    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
                "Resampled %ux%u at %d dpi to the %dx%d dpi device grid, "
                "%ux%u.", state->width, state->height, state->raster_dpi,
                grid.dpi_x, grid.dpi_y, width, height);
  }

  // Then down to what the engine can actually paint. Sister advertises 0.01 mm
  // margins so the dialog cannot scale-to-fit (driver_cb), so CUPS rasterises
  // the whole sheet -- and the sheet is bigger than the imageable box by 38
  // bytes on every line and 200 lines on every page. See crop_to_imageable in
  // encoder/halftone.h for why sending those to the band decoder is what puts
  // blank scanlines on paper.
  const unsigned sheet_pixels_w = width;
  const unsigned sheet_pixels_h = height;
  sisterhl2030::crop_to_imageable(state->toner, width, height, state->sheet_w,
                                  state->sheet_h, grid);
  if (width != sheet_pixels_w || height != sheet_pixels_h) {
    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
                "Cropped the %ux%u sheet to the engine's %ux%u imageable "
                "area (%d bytes/line, was %d).", sheet_pixels_w,
                sheet_pixels_h, width, height, static_cast<int>((width + 7) / 8),
                static_cast<int>((sheet_pixels_w + 7) / 8));
  }

  // Darkness is a model, not a set of measured per-mode multipliers: one
  // physical erosion length per screen drives every mode, and ECONOMODE is no
  // part of it. See "Engine dot transfer" and "Per-screen calibration" in
  // encoder/halftone.h.
  const sisterhl2030::DotTransfer dt =
      sisterhl2030::engine_transfer(kScreen, grid);
  const std::vector<uint8_t> transfer = sisterhl2030::dot_transfer_lut(dt);
  papplLogJob(job, PAPPL_LOGLEVEL_INFO,
              "%s dot transfer at %dx%d dpi, contrast gain %d/100, "
              "density %d/100, ink limit %d%%: 25%%/50%%/75%% coverage asks "
              "for %d/%d/%d of 255.",
              kScreen.name, grid.dpi_x, grid.dpi_y,
              static_cast<int>(std::lround(sisterhl2030::contrast_gain(dt) * 100.0f)),
              static_cast<int>(std::lround(dt.density * 100.0f)),
              static_cast<int>(std::lround(dt.max_toner * 100.0f)),
              transfer[64], transfer[128], transfer[191]);
  sisterhl2030::apply_transfer(state->toner.data(), state->toner.size(),
                               transfer);

  // How much tone did we actually receive? If PAPPL already reduced the page
  // to black and white there is nothing for error diffusion to spread.
  size_t midtones = 0;
  for (uint8_t v : state->toner) {
    if (v != 0 && v != 255) ++midtones;
  }
  // papplLogJob implements its own printf subset -- %zu is not in it and
  // crashes the server, so keep every conversion to %d/%u/%s.
  papplLogJob(job, PAPPL_LOGLEVEL_INFO,
              "Page %ux%u: %u of %u samples are mid-tone (%s via %s).", width,
              height, static_cast<unsigned>(midtones),
              static_cast<unsigned>(state->toner.size()),
              midtones ? "dithering" : "already 1-bit", kScreen.name);
  // Against the raster PAPPL announced, not `height`: by here the page has
  // been put on the device grid and cropped to the imageable box, so neither
  // number is the count of rows the client was meant to send.
  papplLogJob(job, PAPPL_LOGLEVEL_INFO,
              "Received %u of %u raster lines.", state->lines_seen,
              state->height);

  if (kScreen.clustered) {
    sisterhl2030::clustered_dot_45(state->toner.data(), width, height,
                                   kScreen.cell);
  } else {
    sisterhl2030::atkinson(state->toner.data(), width, height);
  }

  // The bitmap the printer wants is square at PageParams::resolution: at 1200
  // it is twice the 600 dpi page on BOTH axes (docs/protocol.md, `paperinf`),
  // while the engine only resolves 1200 across the scan. So the dither above
  // ran at 1200x600 and each of its rows now goes out twice -- the horizontal
  // half of the mode is real halftone detail, the vertical half is the
  // replication the format demands.
  const unsigned repeat =
      grid.dpi_y > 0
          ? static_cast<unsigned>(std::max(1, state->params.resolution / grid.dpi_y))
          : 1u;
  const unsigned out_w = width;
  const unsigned out_h = height * repeat;
  if (repeat > 1) {
    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
                "HQ1200: %ux%u dithered rows encoded as %ux%u.", width, height,
                out_w, out_h);
  }

  const unsigned bpl = (out_w + 7) / 8;
  std::vector<uint8_t> packed(bpl, 0);
  unsigned src_row = 0;
  unsigned dup = 0;
  auto next_line = [&](std::vector<uint8_t>& out) {
    if (src_row >= height) return false;
    if (dup == 0) {
      sisterhl2030::pack_toner_row(
          state->toner.data() + static_cast<size_t>(src_row) * width, width,
          packed.data());
    }
    out.assign(packed.begin(), packed.end());
    if (++dup >= repeat) {
      dup = 0;
      ++src_row;
    }
    return true;
  };

  state->job->encode_page(state->params, static_cast<int>(out_h),
                          static_cast<int>(bpl), next_line);
  fflush(state->stream);
  // Drop the page buffer now: HQ1200's 1200x600 A4 toner is ~70 MB, and the
  // next rstartpage reallocates. Leaving it until rendjob keeps that RAM
  // for the rest of the job and for idle if the client stalls.
  state->toner.clear();
  state->toner.shrink_to_fit();
  return true;
}

// PAPPL: job is complete. Writes the closing UEL, flushes, frees JobState.
bool rendjob(pappl_job_t* job, pappl_pr_options_t* options,
             pappl_device_t* device) {
  (void)options;
  auto* state = static_cast<JobState*>(papplJobGetData(job));
  if (!state) return false;

  if (state->status_only) {
    papplJobSetData(job, nullptr);
    delete state;
    return true;
  }

  state->job.reset();  // writes the closing UEL
  if (state->stream) {
    fflush(state->stream);
    fclose(state->stream);
  }
  papplDeviceFlush(device);
  papplJobSetData(job, nullptr);
  delete state;
#ifdef __APPLE__
  // After a page the malloc zone still holds the 8-bit toner arena.
  // Hand it back so idle RSS is idle RSS.
  malloc_zone_pressure_relief(nullptr, 0);
#endif
  return true;
}

// True when `name` is the friendly Bonjour name we assert, or one of the
// variants PAPPL derived from it after a name collision: dnssd.c renames to
// "<name> (<hostname|serial|uuid>)" first and to "<name> (2)", "<name> (3)"
// after that, so every derived form is kDnsSdName followed by " (".
// Recognising those is what lets the name be asserted from a callback that
// runs once a second without fighting PAPPL's collision handling.
bool dns_sd_name_is_ours(const char* name) {
  if (std::strcmp(name, kDnsSdName) == 0) return true;
  const size_t len = std::strlen(kDnsSdName);
  return std::strncmp(name, kDnsSdName, len) == 0 &&
         std::strncmp(name + len, " (", 2) == 0;
}

// Read toner and drum over PJL and hand them to PAPPL. PAPPL calls this when
// something asks for printer status, so it replaces the whole no-op-job dance
// the ippeveprinter façade needed to get supply data in.
bool status_cb(pappl_printer_t* printer) {
  // Set the friendly Bonjour name once per printer. status_cb runs about once
  // a second; re-asserting the name every tick would fight PAPPL's own DNS-SD
  // collision handling (dnssd.c), which renames the service if another device
  // on the LAN already advertises "Brother HL-2030" -- forcing our name
  // straight back would retrigger that same collision forever. driver_cb
  // can't do this instead: it only gets a pappl_system_t*, not the
  // pappl_printer_t* that papplPrinterSetDNSSDName needs, and runs before the
  // printer exists.
  //
  // The guard is the printer's own name, not a `static bool`: a function-level
  // static is per-process, and make_system() enables
  // PAPPL_SOPTIONS_MULTI_QUEUE, so only the first printer to be polled ever
  // got its name or its job limits and any second one silently kept PAPPL's
  // defaults. Reading the name back is per-printer by construction and needs
  // no bookkeeping to key on the pappl_printer_t*. dns_sd_name_is_ours()
  // accepts the collision-renamed forms too, so a printer PAPPL has already
  // renamed is left alone -- and once a printer is configured the steady-state
  // poll makes no papplPrinterSet* call at all. The job limits ride along in
  // the same block: PAPPL persists them beside the name in its state file, so
  // a printer whose name is already ours has them already.
  char dns_sd[128] = "";
  papplPrinterGetDNSSDName(printer, dns_sd, sizeof(dns_sd));
  if (!dns_sd_name_is_ours(dns_sd)) {
    papplPrinterSetDNSSDName(printer, kDnsSdName);
    // One USB laser: no parallel jobs, and do not keep a hundred completed
    // IPP jobs in RAM after they have finished.
    papplPrinterSetMaxActiveJobs(printer, 1);
    papplPrinterSetMaxCompletedJobs(printer, 16);
    papplPrinterSetMaxPreservedJobs(printer, 0);
  }

  if (papplPrinterGetState(printer) == IPP_PSTATE_PROCESSING) {
    return true;  // mid-job: the device is busy, keep the last reading
  }

  pappl_device_t* device = papplPrinterOpenDevice(printer);
  if (!device) {
    return true;  // in use elsewhere; not an error worth failing status over
  }

  // Everything below runs against one wall clock, because the device is held
  // for exactly as long as this takes and a job submitted meanwhile waits in
  // start_job's "Waiting for device to become available" loop.
  timeval start;
  gettimeofday(&start, nullptr);
  auto elapsed_ms = [&start]() {
    timeval now;
    gettimeofday(&now, nullptr);
    return (now.tv_sec - start.tv_sec) * 1000 +
           (now.tv_usec - start.tv_usec) / 1000;
  };

  char buf[2048];

  // Clear the IN pipe first. The HL-2030's PJL readback lags a command
  // behind (see collect_in in status/usb_printer.cc), which is why
  // pjl_supply_query() ends with an ECHO whose only job is to shake the
  // DRUMLIFE reply loose; the ECHO's own reply is surplus and
  // pjl_response_complete() stops before it. Nothing else ever reads this
  // device -- the print path only writes, and PAPPL clears no pipe on open --
  // so without this every poll left a block behind for the next one to
  // mistake for a fresh answer. Draining on the way in also picks up whatever
  // the engine emitted during the last print job. pjl_query_supplies() does
  // the same thing on the IOKit path.
  unsigned stale = 0;
  while (elapsed_ms() < kStatusDrainMs) {
    const ssize_t got = papplDeviceRead(device, buf, sizeof(buf));
    if (got <= 0) break;
    stale += static_cast<unsigned>(got);
  }

  const std::string query = sisterhl2030::pjl_supply_query();
  papplDeviceWrite(device, query.data(), query.size());
  papplDeviceFlush(device);

  // Bound the wait on the clock, not on a count of reads. papplDeviceRead is
  // a blocking bulk transfer, so counting iterations was really counting read
  // timeouts: 25 of them held the device for over four minutes and stalled
  // the job the print dialog had just submitted.
  std::string reply;
  while (elapsed_ms() < kStatusReplyMs &&
         !sisterhl2030::pjl_response_complete(reply)) {
    const ssize_t got = papplDeviceRead(device, buf, sizeof(buf));
    if (got > 0) {
      reply.append(buf, static_cast<size_t>(got));
    } else {
      // A read that comes back empty has already spent its timeout; this only
      // keeps a hard error from spinning until the deadline.
      usleep(20000);
    }
  }

  const long held_ms = elapsed_ms();
  papplPrinterCloseDevice(printer);

  // Whatever is still in the pipe stays there -- the next poll's drain takes
  // it, and the print path never reads, so it bothers nobody in between.
  // Draining here too would cost a second empty read (half a second of
  // holding the device) to do what the drain above already does.
  //
  // A non-zero `stale` means the last transaction did leave something behind:
  // that is the drain earning its keep, and the number to watch if status
  // readings ever look like they belong to the poll before.
  papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
                  "PJL status held the device %d ms: %u stale bytes dropped, "
                  "%u read.",
                  static_cast<int>(held_ms), stale,
                  static_cast<unsigned>(reply.size()));

  if (reply.empty()) {
    papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG, "No PJL status reply.");
    return true;
  }

  const sisterhl2030::PrinterStatus st = sisterhl2030::parse_pjl_status(reply);

  // The toner sensor is three-state, not a gauge: OK/low/empty become
  // 100/15/0. Drum is DRUMLIFE against its rated 12 000 pages.
  pappl_supply_t supplies[2];
  std::memset(supplies, 0, sizeof(supplies));
  supplies[0].color = PAPPL_SUPPLY_COLOR_BLACK;
  supplies[0].type = PAPPL_SUPPLY_TYPE_TONER;
  supplies[0].is_consumed = true;
  supplies[0].level = st.toner_percent < 0 ? -1 : st.toner_percent;
  papplCopyString(supplies[0].description, sisterhl2030::toner_description(),
                  sizeof(supplies[0].description));
  supplies[1].color = PAPPL_SUPPLY_COLOR_NO_COLOR;
  supplies[1].type = PAPPL_SUPPLY_TYPE_OPC;
  supplies[1].is_consumed = true;
  supplies[1].level = st.drum_percent < 0 ? -1 : st.drum_percent;
  papplCopyString(supplies[1].description, sisterhl2030::drum_description(),
                  sizeof(supplies[1].description));
  papplPrinterSetSupplies(printer, 2, supplies);

  unsigned add = PAPPL_PREASON_NONE;
  if (st.toner_empty) {
    add |= PAPPL_PREASON_TONER_EMPTY;
  } else if (st.toner_low) {
    add |= PAPPL_PREASON_TONER_LOW;
  }
  if (st.drum_empty) {
    add |= PAPPL_PREASON_MARKER_SUPPLY_EMPTY;
  } else if (st.drum_low) {
    add |= PAPPL_PREASON_MARKER_SUPPLY_LOW;
  }
  if (st.cover_open) {
    add |= PAPPL_PREASON_COVER_OPEN;
  }
  if (st.media_jam) {
    add |= PAPPL_PREASON_MEDIA_JAM;
  }
  if (st.media_empty) {
    add |= PAPPL_PREASON_MEDIA_EMPTY;
  }
  if (st.media_needed) {
    add |= PAPPL_PREASON_MEDIA_NEEDED;
  }
  const unsigned clear = PAPPL_PREASON_TONER_LOW | PAPPL_PREASON_TONER_EMPTY |
                         PAPPL_PREASON_MARKER_SUPPLY_LOW |
                         PAPPL_PREASON_MARKER_SUPPLY_EMPTY |
                         PAPPL_PREASON_COVER_OPEN | PAPPL_PREASON_MEDIA_JAM |
                         PAPPL_PREASON_MEDIA_EMPTY | PAPPL_PREASON_MEDIA_NEEDED;
  papplPrinterSetReasons(printer, static_cast<pappl_preason_t>(add),
                         static_cast<pappl_preason_t>(clear & ~add));

  papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
                  "CODE=%d toner %d%%, drum %d%% (page count %d)%s%s%s%s.",
                  st.code, supplies[0].level, supplies[1].level, st.pagecount,
                  st.cover_open ? " cover-open" : "",
                  st.media_jam ? " media-jam" : "",
                  st.media_empty ? " media-empty" : "",
                  st.media_needed ? " media-needed" : "");
  return true;
}

// A4 first: this printer is sold with A4 trays in the region it targets.
const char* const kMedia[] = {
    "iso_a4_210x297mm",
    "na_letter_8.5x11in",
    "na_legal_8.5x14in",
    "na_executive_7.25x10.5in",
    "na_folio_8.5x13in",
    "iso_a5_148x210mm",
    "iso_a6_105x148mm",
    "iso_b5_176x250mm",
    "jis_b5_182x257mm",
    "iso_b6_125x176mm",
    "na_number-10_4.125x9.5in",
    "iso_dl_110x220mm",
};

// PAPPL driver callback: fill pappl_pr_driver_data_t for kDriverName.
bool driver_cb(pappl_system_t* system, const char* driver_name,
               const char* device_uri, const char* device_id,
               pappl_pr_driver_data_t* driver_data, ipp_t** driver_attrs,
               void* data) {
  (void)device_uri;
  (void)device_id;
  (void)data;

  if (!driver_name || !driver_data || !driver_attrs) {
    papplLog(system, PAPPL_LOGLEVEL_ERROR, "Driver callback is missing data.");
    return false;
  }
  if (std::strcmp(driver_name, kDriverName) != 0) {
    papplLog(system, PAPPL_LOGLEVEL_ERROR, "Unsupported driver '%s'.",
             driver_name);
    return false;
  }

  papplCopyString(driver_data->make_and_model, kDriverDesc,
                  sizeof(driver_data->make_and_model));
  driver_data->kind = PAPPL_KIND_DOCUMENT;
  driver_data->ppm = 18;
  // Mono paper, but this field is what decides whether the driver ever sees
  // colour at all. PAPPL derives `color-supported` from `ppm_color > 0`
  // (printer-driver.c); Apple's ipp2ppd reads that boolean when the queue is
  // created to decide whether to write a `*ColorModel RGB` entry; and that
  // entry is what makes CUPS rasterise to sRGB instead of an 8-bit sGray that
  // ColorSync has already flattened by luminance. With it unset, saturated
  // yellow and cyan reach this driver as very nearly white and `rgb_to_toner`
  // is never even called.
  //
  // Set, deliberately, so the colour-to-grey decision is ours (see
  // `kChromaFloor`). The cost is a Color/Grayscale control in the print
  // dialog of a printer that only has black toner. The alternatives were
  // tried and do not work: a custom gray ICC cannot carry chroma at all, and
  // a device-link profile is silently ignored by cgpdftoraster.
  driver_data->ppm_color = 18;

  driver_data->rstartjob_cb = rstartjob;
  driver_data->rstartpage_cb = rstartpage;
  driver_data->rwriteline_cb = rwriteline;
  driver_data->rendpage_cb = rendpage;
  driver_data->rendjob_cb = rendjob;
  driver_data->printfile_cb = printfile;
  driver_data->status_cb = status_cb;
  driver_data->has_supplies = true;
  driver_data->format = "application/octet-stream";

  // 8-bit only. black_1 makes PAPPL (and some clients) threshold with a
  // clustered-dot screen, and Atkinson then has nothing to spread.
  driver_data->raster_types =
      PAPPL_PWG_RASTER_TYPE_SGRAY_8 | PAPPL_PWG_RASTER_TYPE_SRGB_8;

  // The printer is mono, but PAPPL only hands over sRGB when the colour mode
  // is "color" -- and sRGB is the only form in which colour survives the trip
  // intact. We reduce it to toner ourselves, so the paper is still mono.
  driver_data->color_supported =
      PAPPL_COLOR_MODE_COLOR | PAPPL_COLOR_MODE_MONOCHROME;
  driver_data->color_default = PAPPL_COLOR_MODE_COLOR;
  driver_data->sides_supported = PAPPL_SIDES_ONE_SIDED;
  driver_data->sides_default = PAPPL_SIDES_ONE_SIDED;
  driver_data->orient_default = IPP_ORIENT_NONE;
  driver_data->quality_default = IPP_QUALITY_NORMAL;
  // Print at 100%: the whole point of the 0.01 mm margins below is that the
  // dialog must not decide to scale-to-fit.
  driver_data->scaling_default = PAPPL_SCALING_NONE;

  // The queue artwork, when installed. PAPPL serves it for all three sizes.
  if (access(kIconPath, R_OK) == 0) {
    for (auto& icon : driver_data->icons) {
      papplCopyString(icon.filename, kIconPath, sizeof(icon.filename));
    }
  }

  // Advertise both so Apple's ipp2ppd maps Quality Draft → 300 dpi and
  // Normal/High → 600 dpi. CUPS's generic "everywhere" PPD maps them
  // wrongly (Normal → 300); the installer therefore uses ipp2ppd.
  driver_data->num_resolution = 2;
  driver_data->x_resolution[0] = driver_data->y_resolution[0] = 300;
  driver_data->x_resolution[1] = driver_data->y_resolution[1] = 600;
  driver_data->x_default = driver_data->y_default = 600;

  // 0.01 mm, not 0: a zero margin reads as borderless, and the macOS print
  // dialog then scale-to-fits at about 96% instead of printing at 100%.
  // The laser clips a few millimetres at the physical edge regardless.
  driver_data->left_right = 1;
  driver_data->bottom_top = 1;

  driver_data->num_media = static_cast<int>(sizeof(kMedia) / sizeof(kMedia[0]));
  std::memcpy(const_cast<char**>(driver_data->media), kMedia, sizeof(kMedia));

  driver_data->num_source = 2;
  driver_data->source[0] = "main";
  driver_data->source[1] = "manual";

  driver_data->num_type = 6;
  driver_data->type[0] = "stationery";
  driver_data->type[1] = "stationery-letterhead";
  driver_data->type[2] = "envelope";
  driver_data->type[3] = "cardstock";
  driver_data->type[4] = "labels";
  driver_data->type[5] = "transparency";

  papplCopyString(driver_data->media_default.size_name, "iso_a4_210x297mm",
                  sizeof(driver_data->media_default.size_name));
  driver_data->media_default.size_width = 21000;
  driver_data->media_default.size_length = 29700;
  driver_data->media_default.left_margin = driver_data->left_right;
  driver_data->media_default.right_margin = driver_data->left_right;
  driver_data->media_default.top_margin = driver_data->bottom_top;
  driver_data->media_default.bottom_margin = driver_data->bottom_top;
  papplCopyString(driver_data->media_default.source, "main",
                  sizeof(driver_data->media_default.source));
  papplCopyString(driver_data->media_default.type, "stationery",
                  sizeof(driver_data->media_default.type));

  driver_data->media_ready[0] = driver_data->media_default;

  return true;
}

// Lean system object for a single USB printer that sits idle most of the
// day. PAPPL's default is multi-queue + TLS, which mints a self-signed
// cert and keeps extra printer-list state even when nothing is printing.
pappl_system_t* make_system(int num_options, cups_option_t* options,
                            void* data) {
  (void)data;
  const char* directory = cupsGetOption("spool-directory", num_options, options);
  const char* logfile = cupsGetOption("log-file", num_options, options);
  const char* server_hostname =
      cupsGetOption("server-hostname", num_options, options);
  const char* value = cupsGetOption("log-level", num_options, options);
  pappl_loglevel_t loglevel = PAPPL_LOGLEVEL_INFO;
  if (value) {
    if (!std::strcmp(value, "fatal")) {
      loglevel = PAPPL_LOGLEVEL_FATAL;
    } else if (!std::strcmp(value, "error")) {
      loglevel = PAPPL_LOGLEVEL_ERROR;
    } else if (!std::strcmp(value, "warn")) {
      loglevel = PAPPL_LOGLEVEL_WARN;
    } else if (!std::strcmp(value, "info")) {
      loglevel = PAPPL_LOGLEVEL_INFO;
    } else if (!std::strcmp(value, "debug")) {
      loglevel = PAPPL_LOGLEVEL_DEBUG;
    }
  }
  int port = 0;
  if ((value = cupsGetOption("server-port", num_options, options)) != nullptr) {
    port = static_cast<int>(std::strtol(value, nullptr, 10));
  }

  // MULTI_QUEUE stays on: Create-Printer (the installer's `add`) is rejected
  // without it. NO_TLS skips the self-signed cert PAPPL would mint at boot.
  const pappl_soptions_t soptions = PAPPL_SOPTIONS_MULTI_QUEUE |
                                    PAPPL_SOPTIONS_WEB_INTERFACE |
                                    PAPPL_SOPTIONS_NO_TLS;
  pappl_system_t* system = papplSystemCreate(
      soptions, "sister-printer-app", port, "_print,_universal", directory,
      logfile, loglevel, nullptr, false);
  if (!system) {
    return nullptr;
  }
  if (server_hostname) {
    papplSystemSetHostName(system, server_hostname);
  }
  if (!cupsGetOption("private-server", num_options, options)) {
    papplSystemAddListeners(
        system, cupsGetOption("listen-hostname", num_options, options));
  }
  papplSystemSetMaxClients(system, 16);
  papplSystemSetMaxSubscriptions(system, 8);
  // Default is 1 GiB uncompressed. Legal @ 600 dpi sGray is ~40 MB; this
  // still accepts a photo submitted to the web UI without letting a huge
  // PNG pin a gigabyte.
  papplSystemSetMaxImageSize(system, 96u * 1024u * 1024u, 8192, 8192);
  return system;
}

}  // namespace

int main(int argc, char* argv[]) {
  // sister-printer-app --version is what Scripts/Check Sister HL2030.sh
  // compares against the installed binary.
  if (argc >= 2 && (!std::strcmp(argv[1], "--version") ||
                    !std::strcmp(argv[1], "version"))) {
    std::puts(SISTER_VERSION_FULL);
    return 0;
  }

  static pappl_pr_driver_t drivers[] = {
      {kDriverName, kDriverDesc, kDeviceId, nullptr},
  };

  // The footer is not optional: PAPPL passes it to a string compare when it
  // renders any web page, and a null one segfaults the daemon.
  static const char* kFooter =
      "SisterHL2030 " SISTER_VERSION_FULL
      ". Copyright &copy; 2026 Rui Nelson. "
      "<a href=\"https://github.com/RuiNelson/SisterHL2030\">SisterHL2030</a> "
      "is free software under the GNU GPL v2 or later.";

  std::fprintf(stderr, "SisterHL2030 %s\n", SISTER_VERSION_FULL);
  return papplMainloop(argc, argv, SISTER_VERSION, kFooter,
                       static_cast<int>(sizeof(drivers) / sizeof(drivers[0])),
                       drivers, nullptr, driver_cb, nullptr, nullptr,
                       make_system, nullptr, nullptr);
}
