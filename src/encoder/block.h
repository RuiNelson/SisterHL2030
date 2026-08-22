// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SISTERHL2030_BLOCK_H
#define SISTERHL2030_BLOCK_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sisterhl2030 {

// One mode-1030 band. Brother flushes at 128 lines or 16384 payload bytes.
class Block {
 public:
  bool empty() const { return lines_.empty(); }

  bool line_fits(unsigned size) const {
    return line_bytes_ + size <= kMaxBlockSize;
  }

  unsigned line_count() const { return static_cast<unsigned>(lines_.size()); }

  void add_line(std::vector<uint8_t>&& line) {
    line_bytes_ += line.size();
    lines_.push_back(std::move(line));
  }

  void flush(FILE* out) {
    if (empty()) {
      return;
    }
    const unsigned nbytes = line_bytes_ + 2;
    std::string header = std::to_string(nbytes) + "w";
    fwrite(header.data(), 1, header.size(), out);
    fputc(0, out);
    fputc(static_cast<int>(lines_.size()), out);
    for (const auto& line : lines_) {
      fwrite(line.data(), 1, line.size(), out);
    }
    lines_.clear();
    line_bytes_ = 0;
  }

 private:
  static constexpr unsigned kMaxBlockSize = 16384;
  std::vector<std::vector<uint8_t>> lines_;
  unsigned line_bytes_ = 0;
};

}  // namespace sisterhl2030

#endif
