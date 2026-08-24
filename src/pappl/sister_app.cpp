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
#include <unistd.h>

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

constexpr const char* kDriverName = "sister-hl2030";
constexpr const char* kDriverDesc = "Brother HL-2030 series";
constexpr const char* kDeviceId = "MFG:Brother;MDL:HL-2030 series;CMD:PJL,HBP;CLS:PRINTER;";
constexpr const char* kIconPath = "/Library/Printers/SisterHL2030/icon.png";
// What AirPrint clients show. The IPP printer name cannot hold spaces, so the
// friendly name is set separately -- the façade advertised the same string.
constexpr const char* kDnsSdName = "Brother HL-2030";
// A job with this name prints nothing. CUPS only copies marker-levels off the
// printer while a job runs through its backend, so without a way to run an
// empty one the macOS Supply Levels panel stays empty until the first real
// print. Submitting this costs no paper.
constexpr const char* kStatusJobName = ".sister-status";
#if defined(SISTERHL2030_HALFTONE_AM45)
constexpr const char* kHalftoneScreenName = "AM45";
#else
constexpr const char* kHalftoneScreenName = "Atkinson";
#endif

// Per-job state. PAPPL hands it back to every raster callback.
struct JobState {
  bool status_only = false;  // named kStatusJobName: report status, print nothing
  FILE* stream = nullptr;
  std::unique_ptr<sisterhl2030::Job> job;
  sisterhl2030::PageParams params;
  std::vector<uint8_t> toner;  // width*height, 0 = paper white, 255 = black
  unsigned width = 0;
  unsigned height = 0;
  unsigned lines_seen = 0;
  int raster_dpi = 600;
};

// The old CUPS path never converted colour itself: macOS handed the filter a
// grey raster (CUPS_CSPACE_W) and the laser curve was tuned against those
// values. macOS's PDF rasteriser lifts tone as it goes -- for the flag it
// emits grey 186/144 where ColorSync's own transform gives 130/87 -- so a
// colorimetrically correct conversion lands lighter than the prints this
// driver has always produced.
//
// PAPPL has no equivalent stage, so match the old output by measurement
// rather than by theory. Solving for the exponent that reproduces the old
// coverage gives 0.841 on test_fixtures/flag.png and 0.869 on the photo
// fixture; 0.85 splits them. Re-derive with Scripts/decode_job.py if the
// pipeline changes: the targets are 40.2% and 40.5% black coverage.
constexpr float kToneCalibration = 0.85f;

const std::array<uint8_t, 256>& tone_curve() {
  static const std::array<uint8_t, 256> lut = [] {
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i) {
      const float v = std::pow(i / 255.0f, kToneCalibration);
      t[static_cast<size_t>(i)] =
          static_cast<uint8_t>(std::lround(v * 255.0f));
    }
    return t;
  }();
  return lut;
}

// Adapt the PAPPL device to the FILE* the encoder writes to.
int device_write(void* cookie, const char* buffer, int bytes) {
  auto* device = static_cast<pappl_device_t*>(cookie);
  const ssize_t written =
      papplDeviceWrite(device, buffer, static_cast<size_t>(bytes));
  return written < 0 ? -1 : static_cast<int>(written);
}

std::string pjl_paper(const char* pwg_size) {
  const std::string s = pwg_size ? pwg_size : "";
  if (s.find("na_letter") == 0) return "LETTER";
  if (s.find("na_legal") == 0) return "LEGAL";
  if (s.find("na_number-10") == 0) return "COM10";
  if (s.find("iso_dl") == 0) return "DL";
  return "A4";
}

std::string pjl_mediatype(const char* pwg_type) {
  const std::string t = pwg_type ? pwg_type : "";
  if (t == "envelope") return "ENVELOPES";
  if (t == "cardstock") return "THICK";
  if (t == "transparency") return "TRANSPARENCY";
  if (t == "labels") return "THICK";
  if (t == "stationery-letterhead") return "THICK";
  return "REGULAR";  // "stationery", "auto", "other"
}

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

// Convert one PAPPL raster line to toner 0..255. 8-bit sGray from macOS is
// ColorSync-linearised already, so it uses the laser curve as-is. PAPPL's
// own sRGB raster (JPEG/PNG submitted to the app) does not, and gets the
// extra 0.85 calibration so the two paths land on the same coverage.
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
    const std::array<uint8_t, 256>& tone = tone_curve();
    for (unsigned x = 0; x < width; ++x) {
      const unsigned char* px = line + static_cast<size_t>(x) * stride;
      row[x] = tone[sisterhl2030::rgb_to_toner(px[0], px[1], px[2])];
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

  // Draft is 300 dpi. If the client already sent 300 dpi (Apple's ipp2ppd
  // mapping), leave it; if it sent 600, box-filter down.
  if (state->params.resolution == 300 && state->raster_dpi >= 450) {
    sisterhl2030::box_downsample_2x(state->toner, width, height);
  }

#if defined(SISTERHL2030_HALFTONE_AM45)
  // The 1.28/1.18 dot-gain compensation below (see the #else) was measured
  // against Atkinson's failure mode on this engine: isolated single-pixel
  // dots that go missing more often at higher dpi. A clustered-dot screen
  // groups every dot into a solid multi-pixel blob for exactly this reason,
  // so it should not lose coverage the same way -- but that is a hypothesis,
  // not a measurement, and reusing Atkinson's numbers here would stack an
  // unverified resolution-dependent correction on top of an unverified
  // algorithm. No compensation until AM45 has its own hardware measurement.
  const float gain = 1.0f;
#else
  // Draft at 300 dpi is the reference look. 600 and 1200 have less dot
  // gain, so they need more digital coverage to match. Fine at 1.28 was
  // a hair too heavy once the page was full size (ECONOMODE is already
  // off); 1.18 sits between that and the washed-out 0.82.
  float gain = 1.0f;
  if (state->params.resolution == 600) {
    gain = 1.28f;
  } else if (state->params.resolution == 1200) {
    gain = 1.18f;
  }
#endif
  sisterhl2030::scale_coverage(state->toner.data(), state->toner.size(), gain);

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
              midtones ? "dithering" : "already 1-bit", kHalftoneScreenName);
  papplLogJob(job, PAPPL_LOGLEVEL_INFO,
              "Received %u of %u raster lines.", state->lines_seen,
              height);

#if defined(SISTERHL2030_HALFTONE_AM45)
  sisterhl2030::clustered_dot_45(state->toner.data(), width, height);
#else
  sisterhl2030::atkinson(state->toner.data(), width, height);
#endif

  // HQ1200's pixel grid is twice 600 dpi. Dither at 600 first: a 1200 dpi A4
  // toner buffer is ~140 million 8-bit samples, and Atkinson's serpentine
  // error diffusion never finishes at that size. Pixel-double the 1-bit rows
  // as they encode so RAS1200MODE still prints at full size without a 4x
  // 8-bit page -- this also keeps the screen ruling identical to Normal
  // quality's dither, just rendered with finer engine dots underneath it.
  const bool x2 = state->params.resolution == 1200 && state->raster_dpi < 900;
  if (x2) {
    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
                "HQ1200 encoding as %ux%u after dither.", width * 2, height * 2);
  }

  const unsigned out_w = x2 ? width * 2 : width;
  const unsigned out_h = x2 ? height * 2 : height;
  const unsigned bpl = (out_w + 7) / 8;
  std::vector<uint8_t> packed(bpl, 0);
  unsigned src_row = 0;
  unsigned dup = 0;
  auto next_line = [&](std::vector<uint8_t>& out) {
    if (src_row >= height) return false;
    if (!x2) {
      sisterhl2030::pack_toner_row(
          state->toner.data() + static_cast<size_t>(src_row) * width, width,
          packed.data());
      out.assign(packed.begin(), packed.end());
      ++src_row;
      return true;
    }
    if (dup == 0) {
      sisterhl2030::pack_toner_row_2x(
          state->toner.data() + static_cast<size_t>(src_row) * width, width,
          packed.data());
    }
    out.assign(packed.begin(), packed.end());
    if (dup == 0) {
      dup = 1;
    } else {
      dup = 0;
      ++src_row;
    }
    return true;
  };

  state->job->encode_page(state->params, static_cast<int>(out_h),
                          static_cast<int>(bpl), next_line);
  fflush(state->stream);
  // Drop the page buffer now: HQ1200's upsampled toner is ~140 MB, and the
  // next rstartpage reallocates. Leaving it until rendjob keeps that RAM
  // for the rest of the job and for idle if the client stalls.
  state->toner.clear();
  state->toner.shrink_to_fit();
  return true;
}

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

// Read toner and drum over PJL and hand them to PAPPL. PAPPL calls this when
// something asks for printer status, so it replaces the whole no-op-job dance
// the ippeveprinter façade needed to get supply data in.
bool status_cb(pappl_printer_t* printer) {
  // Set the friendly Bonjour name once. status_cb runs about once a second;
  // re-asserting the name every tick would fight PAPPL's own DNS-SD collision
  // handling (dnssd.c), which renames the service if another device on the
  // LAN already advertises "Brother HL-2030" -- forcing our name straight
  // back would retrigger that same collision forever. driver_cb can't do
  // this instead: it only gets a pappl_system_t*, not the pappl_printer_t*
  // that papplPrinterSetDNSSDName needs, and runs before the printer exists.
  static bool dns_sd_name_set = false;
  if (!dns_sd_name_set) {
    char dns_sd[128] = "";
    papplPrinterGetDNSSDName(printer, dns_sd, sizeof(dns_sd));
    if (std::strcmp(dns_sd, kDnsSdName) != 0) {
      papplPrinterSetDNSSDName(printer, kDnsSdName);
    }
    // One USB laser: no parallel jobs, and do not keep a hundred completed
    // IPP jobs in RAM after they have finished.
    papplPrinterSetMaxActiveJobs(printer, 1);
    papplPrinterSetMaxCompletedJobs(printer, 16);
    papplPrinterSetMaxPreservedJobs(printer, 0);
    dns_sd_name_set = true;
  }

  if (papplPrinterGetState(printer) == IPP_PSTATE_PROCESSING) {
    return true;  // mid-job: the device is busy, keep the last reading
  }

  pappl_device_t* device = papplPrinterOpenDevice(printer);
  if (!device) {
    return true;  // in use elsewhere; not an error worth failing status over
  }

  const std::string query = sisterhl2030::pjl_supply_query();
  papplDeviceWrite(device, query.data(), query.size());
  papplDeviceFlush(device);

  std::string reply;
  char buf[2048];
  for (int i = 0; i < 25 && !sisterhl2030::pjl_response_complete(reply); ++i) {
    const ssize_t got = papplDeviceRead(device, buf, sizeof(buf));
    if (got > 0) {
      reply.append(buf, static_cast<size_t>(got));
    } else {
      usleep(100000);
    }
  }
  papplPrinterCloseDevice(printer);

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
  const unsigned clear = PAPPL_PREASON_TONER_LOW | PAPPL_PREASON_TONER_EMPTY |
                         PAPPL_PREASON_MARKER_SUPPLY_LOW |
                         PAPPL_PREASON_MARKER_SUPPLY_EMPTY;
  papplPrinterSetReasons(printer, static_cast<pappl_preason_t>(add),
                         static_cast<pappl_preason_t>(clear & ~add));

  papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
                  "Toner %d%%, drum %d%% (page count %d).", supplies[0].level,
                  supplies[1].level, st.pagecount);
  return true;
}

// A4 first: this printer is sold with A4 trays in the region it targets.
const char* const kMedia[] = {
    "iso_a4_210x297mm", "na_letter_8.5x11in", "na_legal_8.5x14in",
    "na_number-10_4.125x9.5in", "iso_dl_110x220mm",
};

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

  driver_data->num_source = 3;
  driver_data->source[0] = "main";
  driver_data->source[1] = "manual";
  driver_data->source[2] = "by-pass-tray";

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
