// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SISTERHL2030_JOB_H
#define SISTERHL2030_JOB_H

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "encoder/page_params.h"

namespace sisterhl2030 {

using NextLineFn = std::function<bool(std::vector<uint8_t>&)>;

class Job {
 public:
  Job(FILE* out, std::string job_name);
  ~Job();

  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;

  void encode_page(const PageParams& params, int height, int bytes_per_line,
                   const NextLineFn& next_line);

  int pages() const { return pages_; }

 private:
  void begin_job();
  void end_job();
  void write_page_header();

  FILE* out_;
  std::string job_name_;
  PageParams page_params_;
  int pages_ = 0;
  bool started_ = false;
};

}  // namespace sisterhl2030

#endif
