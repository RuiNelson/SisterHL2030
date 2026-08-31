// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// One mode-1030 band (a run of packed scanlines). Brother flushes when the
// band reaches 128 lines or 16 384 payload bytes. See docs/protocol.md.

#ifndef SISTERHL2030_BLOCK_H
#define SISTERHL2030_BLOCK_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sisterhl2030 {

// Accumulates packed lines and writes them as one `<nbytes>w` band.
class Block {
 public:
  // True when no lines have been added since the last flush (or construction).
  bool empty() const { return lines_.empty(); }

  // True if a packed line of `size` bytes can join this band without
  // exceeding kMaxBlockSize. The first line of a band always fits.
  bool line_fits(unsigned size) const {
    return line_bytes_ + size <= kMaxBlockSize;
  }

  // Number of packed lines currently buffered.
  unsigned line_count() const { return static_cast<unsigned>(lines_.size()); }

  // Append a packed line. Caller must have checked line_fits() unless this
  // is the first line of a new band.
  void add_line(std::vector<uint8_t>&& line) {
    line_bytes_ += line.size();
    lines_.push_back(std::move(line));
  }

  // Write the band header and payload, then reset. A no-op on an empty band.
  // Header is ASCII `<nbytes>w` followed by a zero byte and the line count.
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
  // Maximum packed-line payload per band, in bytes. Recovered from
  // SendData_1030: flush when adding the next line would exceed this.
  static constexpr unsigned kMaxBlockSize = 16384;

  std::vector<std::vector<uint8_t>> lines_;
  unsigned line_bytes_ = 0;  // sum of packed line sizes, excluding the header
};

}  // namespace sisterhl2030

#endif
