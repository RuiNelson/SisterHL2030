// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

#include "encoder/job.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "encoder/block.h"
#include "encoder/line.h"

namespace sisterhl2030 {
namespace {

constexpr int kLinesPerBand = 128;

void sanitize_job_name(std::string* name) {
  std::replace_if(
      name->begin(), name->end(),
      [](unsigned char c) { return c < 32 || c >= 127 || c == '"' || c == '\\'; },
      ' ');
  if (name->empty()) {
    *name = "SisterHL2030";
  }
  if (name->size() > 79) {
    name->resize(79);
  }
}

void pjl(FILE* out, const char* line) { fputs(line, out); }

bool is_envelope(const PageParams& p) {
  return p.mediatype == "ENVELOPES" || p.mediatype == "ENVTHICK" ||
         p.mediatype == "ENVTHIN";
}

bool wants_manual(const PageParams& p) {
  if (p.sourcetray == "MANUAL" || p.sourcetray == "MPTRAY") {
    return true;
  }
  // Thick stock, labels and envelopes only feed from the manual slot
  // on this printer. Sending them at tray 1 jams or does nothing.
  return p.mediatype == "THICK" || p.mediatype == "THICK2" || is_envelope(p);
}

void write_tray(FILE* out, const PageParams& p) {
  // Brother PCL paper source (HL-2070N column of the TRG, same family):
  //   1 / 1001 = tray 1 (fixed). macOS rastertobrother2030 emits 1h1001H.
  //   2 = manual feed slot.
  //   3 = envelope from the manual slot.
  if (wants_manual(p)) {
    pjl(out, is_envelope(p) ? "\033&l3H" : "\033&l2H");
  } else {
    pjl(out, "\033&l1h1001H");
  }
}

}  // namespace

Job::Job(FILE* out, std::string job_name)
    : out_(out), job_name_(std::move(job_name)) {
  sanitize_job_name(&job_name_);
}

Job::~Job() {
  if (started_) {
    end_job();
  }
}

void Job::begin_job() {
  // The official macOS rastertobrother2030 starts the job at the first
  // page header (UEL + PJL). No 128-byte NUL prefix, no JOB NAME.
  started_ = true;
}

void Job::end_job() {
  pjl(out_, "\033%-12345X");
}

void Job::write_page_header() {
  pjl(out_, "\033%-12345X@PJL\n");
  if (page_params_.resolution == 1200) {
    pjl(out_, "@PJL SET RAS1200MODE = TRUE\n");
    pjl(out_, "@PJL SET RESOLUTION = 600\n");
  } else {
    pjl(out_, "@PJL SET RAS1200MODE = OFF\n");
    fprintf(out_, "@PJL SET RESOLUTION = %d\n", page_params_.resolution);
  }
  fprintf(out_, "@PJL SET ECONOMODE = %s\n",
          page_params_.economode ? "ON" : "OFF");
  fprintf(out_, "@PJL SET MEDIATYPE = %s\n", page_params_.mediatype.c_str());
  pjl(out_, "@PJL SET ORIENTATION = PORTRAIT\n");
  fprintf(out_, "@PJL SET PAPER = %s\n", page_params_.papersize.c_str());
  pjl(out_, "@PJL SET PAGEPROTECT = AUTO\n");
  pjl(out_, "@PJL ENTER LANGUAGE = PCL\n");
  pjl(out_, "\033E");
  write_tray(out_, page_params_);
  fprintf(out_, "\033&l%dX", std::max(1, page_params_.copies));
}

void Job::encode_page(const PageParams& params, int height, int bytes_per_line,
                      const NextLineFn& next_line) {
  if (!started_) {
    begin_job();
  }
  ++pages_;

  if (pages_ == 1 || !(page_params_ == params)) {
    page_params_ = params;
    write_page_header();
  }

  std::vector<uint8_t> line(static_cast<size_t>(bytes_per_line));
  std::vector<uint8_t> reference(static_cast<size_t>(bytes_per_line));
  Block block;

  if (!next_line(line)) {
    return;
  }
  block.add_line(encode_line(line));
  reference.swap(line);

  pjl(out_, "\033*b1030m");

  for (int i = 1; i < height && next_line(line); ++i) {
    std::vector<uint8_t> encoded;
    const bool new_band = (i % kLinesPerBand) == 0;
    if (new_band) {
      block.flush(out_);
      encoded = encode_line(line);
    } else {
      encoded = encode_line(line, reference);
      if (!block.line_fits(static_cast<unsigned>(encoded.size()))) {
        block.flush(out_);
        encoded = encode_line(line);
      }
    }
    block.add_line(std::move(encoded));
    reference.swap(line);
  }

  block.flush(out_);
  pjl(out_, "1030M");
  fputc('\f', out_);
  fflush(out_);
}

}  // namespace sisterhl2030
