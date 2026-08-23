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
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "encoder/halftone.h"
#include "encoder/job.h"

namespace {

constexpr const char* kDriverName = "sister-hl2030";
constexpr const char* kDriverDesc = "Brother HL-2030 series";
constexpr const char* kDeviceId = "MFG:Brother;MDL:HL-2030 series;CMD:PJL,HBP;CLS:PRINTER;";

// Per-job state. PAPPL hands it back to every raster callback.
struct JobState {
  FILE* stream = nullptr;
  std::unique_ptr<sisterhl2030::Job> job;
  sisterhl2030::PageParams params;
  std::vector<uint8_t> toner;  // width*height, 0 = paper white, 255 = black
  unsigned width = 0;
  unsigned height = 0;
  unsigned lines_seen = 0;
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
  return "REGULAR";
}

// Quality maps the same way as the CUPS filter: see docs/protocol.md.
void quality_to_params(ipp_quality_t quality, sisterhl2030::PageParams* params) {
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
}

// Raw passthrough: the file is already a HL-2030 job stream, as produced by
// sister-rawtobr. PAPPL requires this whenever a driver advertises a raw
// format, and rejects the driver data outright without it.
bool printfile(pappl_job_t* job, pappl_pr_options_t* options,
               pappl_device_t* device) {
  (void)options;
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

  state->width = options->header.cupsWidth;
  state->height = options->header.cupsHeight;
  if (state->width == 0 || state->height == 0) {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Empty raster page.");
    return false;
  }

  quality_to_params(options->print_quality, &state->params);
  state->params.copies = options->copies > 0 ? options->copies : 1;
  state->params.papersize = pjl_paper(options->media.size_name);
  state->params.mediatype = pjl_mediatype(options->media.type);

  // 0 = paper white; rwriteline fills in the real toner values.
  state->lines_seen = 0;
  papplLogJob(job, PAPPL_LOGLEVEL_INFO,
              "Raster page %ux%u, %u bytes/line, %u bits/pixel.",
              options->header.cupsWidth, options->header.cupsHeight,
              options->header.cupsBytesPerLine,
              options->header.cupsBitsPerPixel);
  state->toner.assign(static_cast<size_t>(state->width) * state->height, 0);
  return true;
}

bool rwriteline(pappl_job_t* job, pappl_pr_options_t* options,
                pappl_device_t* device, unsigned y, const unsigned char* line) {
  (void)options;
  (void)device;
  auto* state = static_cast<JobState*>(papplJobGetData(job));
  if (!state || y >= state->height) return true;

  ++state->lines_seen;
  uint8_t* row = state->toner.data() + static_cast<size_t>(y) * state->width;

  // PAPPL 1.4 never converts RGB to grey: for an 8-bit grey raster it copies
  // bytes straight out of its 3-byte RGB buffer, so "grey" is really the red
  // channel and any saturated colour collapses to black or white. Ask for
  // sRGB instead and do the luma ourselves.
  const std::array<uint8_t, 256>& tone = tone_curve();
  if (options->header.cupsBitsPerPixel >= 24) {
    for (unsigned x = 0; x < state->width; ++x) {
      const unsigned char* px = line + static_cast<size_t>(x) * 3;
      row[x] = tone[sisterhl2030::rgb_to_toner(px[0], px[1], px[2])];
    }
  } else {
    for (unsigned x = 0; x < state->width; ++x) {
      row[x] = tone[sisterhl2030::device_gray_to_toner(line[x])];
    }
  }

  return true;
}

bool rendpage(pappl_job_t* job, pappl_pr_options_t* options,
              pappl_device_t* device, unsigned page) {
  (void)device;
  (void)page;
  auto* state = static_cast<JobState*>(papplJobGetData(job));
  if (!state) return false;

  unsigned width = state->width;
  unsigned height = state->height;

  // Draft prints at 300 dpi; the raster arrives at the advertised resolution.
  if (state->params.resolution == 300 &&
      options->printer_resolution[0] >= 450) {
    sisterhl2030::box_downsample_2x(state->toner, width, height);
  }

  // How much tone did we actually receive? If PAPPL already reduced the page
  // to black and white there is nothing for error diffusion to spread.
  size_t midtones = 0;
  for (uint8_t v : state->toner) {
    if (v != 0 && v != 255) ++midtones;
  }
  // papplLogJob implements its own printf subset -- %zu is not in it and
  // crashes the server, so keep every conversion to %d/%u/%s.
  papplLogJob(job, PAPPL_LOGLEVEL_INFO,
              "Page %ux%u: %u of %u samples are mid-tone (%s).", width, height,
              static_cast<unsigned>(midtones),
              static_cast<unsigned>(state->toner.size()),
              midtones ? "dithering" : "already 1-bit");
  papplLogJob(job, PAPPL_LOGLEVEL_INFO,
              "Received %u of %u raster lines.", state->lines_seen,
              height);

  sisterhl2030::atkinson(state->toner.data(), width, height);

  const unsigned bpl = (width + 7) / 8;
  std::vector<uint8_t> packed(static_cast<size_t>(bpl) * height, 0);
  for (unsigned y = 0; y < height; ++y) {
    sisterhl2030::pack_toner_row(
        state->toner.data() + static_cast<size_t>(y) * width, width,
        packed.data() + static_cast<size_t>(y) * bpl);
  }

  unsigned row = 0;
  auto next_line = [&](std::vector<uint8_t>& out) {
    if (row >= height) return false;
    const uint8_t* src = packed.data() + static_cast<size_t>(row) * bpl;
    out.assign(src, src + bpl);
    ++row;
    return true;
  };

  state->job->encode_page(state->params, static_cast<int>(height),
                          static_cast<int>(bpl), next_line);
  fflush(state->stream);
  return true;
}

bool rendjob(pappl_job_t* job, pappl_pr_options_t* options,
             pappl_device_t* device) {
  (void)options;
  auto* state = static_cast<JobState*>(papplJobGetData(job));
  if (!state) return false;

  state->job.reset();  // writes the closing UEL
  if (state->stream) {
    fflush(state->stream);
    fclose(state->stream);
  }
  papplDeviceFlush(device);
  papplJobSetData(job, nullptr);
  delete state;
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
  driver_data->format = "application/octet-stream";

  // Always take 8-bit gray and dither it ourselves: Atkinson here
  // beats handing the printer a pre-thresholded bitmap.
  driver_data->raster_types = PAPPL_PWG_RASTER_TYPE_BLACK_1 |
                              PAPPL_PWG_RASTER_TYPE_SGRAY_8 |
                              PAPPL_PWG_RASTER_TYPE_SRGB_8;

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

  driver_data->num_resolution = 2;
  driver_data->x_resolution[0] = driver_data->y_resolution[0] = 300;
  driver_data->x_resolution[1] = driver_data->y_resolution[1] = 600;
  driver_data->x_default = driver_data->y_default = 600;

  // 0.01 mm, not 0: a zero margin reads as borderless and the macOS print
  // dialog then scales to fit. Same reasoning as ipp/printer-attrs.conf.
  driver_data->left_right = 1;
  driver_data->bottom_top = 1;

  driver_data->num_media = static_cast<int>(sizeof(kMedia) / sizeof(kMedia[0]));
  std::memcpy(const_cast<char**>(driver_data->media), kMedia, sizeof(kMedia));

  driver_data->num_source = 2;
  driver_data->source[0] = "main";
  driver_data->source[1] = "manual";

  driver_data->num_type = 4;
  driver_data->type[0] = "stationery";
  driver_data->type[1] = "envelope";
  driver_data->type[2] = "cardstock";
  driver_data->type[3] = "transparency";

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

}  // namespace

int main(int argc, char* argv[]) {
  static pappl_pr_driver_t drivers[] = {
      {kDriverName, kDriverDesc, kDeviceId, nullptr},
  };

  return papplMainloop(argc, argv, "1.0", nullptr,
                       static_cast<int>(sizeof(drivers) / sizeof(drivers[0])),
                       drivers, nullptr, driver_cb, nullptr, nullptr, nullptr,
                       nullptr, nullptr);
}
