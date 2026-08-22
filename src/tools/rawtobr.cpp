// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// sister-rawtobr: binary PBM (P4) on stdin → HL-2030 job on stdout.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "encoder/job.h"

namespace {

void usage() {
  std::cerr
      << "usage: sister-rawtobr [--resolution 300|600|1200] [--paper A4|LETTER|...]\n"
      << "                      [--media REGULAR|THIN|THICK|...] [--tray TRAY1|AUTO|...]\n"
      << "                      [--econo] [--copies N] [file.pbm]\n"
      << "Reads a binary PBM (P4) and writes a Brother HL-2030 job.\n";
}

int skip_ws_and_comments(FILE* in) {
  int c;
  while ((c = fgetc(in)) != EOF) {
    if (c == '#') {
      while ((c = fgetc(in)) != EOF && c != '\n') {
      }
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      continue;
    }
    ungetc(c, in);
    return 0;
  }
  return -1;
}

bool read_pbm(FILE* in, int* width, int* height, std::vector<uint8_t>* pixels) {
  char magic[3] = {};
  if (fread(magic, 1, 2, in) != 2 || magic[0] != 'P' || magic[1] != '4') {
    return false;
  }
  if (skip_ws_and_comments(in) != 0) {
    return false;
  }
  if (fscanf(in, "%d", width) != 1) {
    return false;
  }
  if (skip_ws_and_comments(in) != 0) {
    return false;
  }
  if (fscanf(in, "%d", height) != 1) {
    return false;
  }
  int c = fgetc(in);
  if (c != '\n' && c != ' ' && c != '\t' && c != '\r') {
    ungetc(c, in);
  }
  if (*width <= 0 || *height <= 0 || *width > 20000 || *height > 30000) {
    return false;
  }
  const int bpl = (*width + 7) / 8;
  pixels->assign(static_cast<size_t>(bpl) * static_cast<size_t>(*height), 0);
  const size_t n = pixels->size();
  return fread(pixels->data(), 1, n, in) == n;
}

}  // namespace

int main(int argc, char** argv) {
  sisterhl2030::PageParams params;
  const char* path = nullptr;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << flag << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
      usage();
      return 0;
    }
    if (std::strcmp(a, "--resolution") == 0) {
      params.resolution = std::atoi(need(a));
    } else if (std::strcmp(a, "--paper") == 0) {
      params.papersize = need(a);
    } else if (std::strcmp(a, "--media") == 0) {
      params.mediatype = need(a);
    } else if (std::strcmp(a, "--tray") == 0) {
      params.sourcetray = need(a);
    } else if (std::strcmp(a, "--econo") == 0) {
      params.economode = true;
    } else if (std::strcmp(a, "--copies") == 0) {
      params.copies = std::atoi(need(a));
    } else if (a[0] != '-') {
      path = a;
    } else {
      std::cerr << "unknown option: " << a << "\n";
      usage();
      return 2;
    }
  }

  if (params.resolution != 300 && params.resolution != 600 &&
      params.resolution != 1200) {
    std::cerr << "resolution must be 300, 600, or 1200\n";
    return 2;
  }

  FILE* in = stdin;
  if (path) {
    in = std::fopen(path, "rb");
    if (!in) {
      std::perror(path);
      return 1;
    }
  }

  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;
  if (!read_pbm(in, &width, &height, &pixels)) {
    std::cerr << "failed to read binary PBM (P4)\n";
    return 1;
  }
  if (in != stdin) {
    std::fclose(in);
  }

  const int bpl = (width + 7) / 8;
  size_t row = 0;
  auto next_line = [&](std::vector<uint8_t>& buf) {
    if (row >= static_cast<size_t>(height)) {
      return false;
    }
    const uint8_t* src = pixels.data() + row * static_cast<size_t>(bpl);
    std::memcpy(buf.data(), src, static_cast<size_t>(bpl));
    ++row;
    return true;
  };

  sisterhl2030::Job job(stdout, path ? path : "SisterHL2030");
  job.encode_page(params, height, bpl, next_line);
  if (std::ferror(stdout)) {
    std::cerr << "write failed\n";
    return 1;
  }
  return 0;
}
