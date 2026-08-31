// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Job envelope: UEL + PJL page header + mode-1030 bands + form feed.
// Matches the official macOS rastertobrother2030 shape (no 128-byte NUL
// prefix, no @PJL JOB NAME). See docs/protocol.md.

#ifndef SISTERHL2030_JOB_H
#define SISTERHL2030_JOB_H

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "encoder/page_params.h"

namespace sisterhl2030 {

// Pulls the next packed scanline into `line`. Returns false when the page
// is exhausted. `line` is already sized to bytes_per_line by the encoder.
using NextLineFn = std::function<bool(std::vector<uint8_t>&)>;

// One print job on `out`. The first encode_page() writes the opening UEL;
// the destructor writes the closing UEL.
class Job {
 public:
  // `job_name` is sanitised (ASCII, no quotes, max 79 chars) and kept for
  // diagnostics; it is not written to the wire (the macOS path has no
  // @PJL JOB NAME).
  Job(FILE* out, std::string job_name);
  ~Job();

  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;

  // Encode one page of `height` packed rows, each `bytes_per_line` bytes,
  // pulling rows from `next_line`. Writes a new PJL header when this is
  // the first page or when `params` differ from the previous page.
  void encode_page(const PageParams& params, int height, int bytes_per_line,
                   const NextLineFn& next_line);

  // Pages accepted by encode_page() so far, including empty ones.
  int pages() const { return pages_; }

 private:
  // Marks the job open. The opening UEL is emitted with the first page
  // header, matching rastertobrother2030.
  void begin_job();

  // Writes the closing UEL. Called from the destructor if begin_job ran.
  void end_job();

  // PJL SET block + PCL reset, paper source and copy count for the current
  // page_params_. HQ1200 sends RAS1200MODE = TRUE and RESOLUTION = 600.
  void write_page_header();

  FILE* out_;
  std::string job_name_;
  PageParams page_params_;
  int pages_ = 0;
  bool started_ = false;
};

}  // namespace sisterhl2030

#endif
