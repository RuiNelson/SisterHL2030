// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Mode-1030 packed-line codec. Grammar recovered from libbrcomplpr2.so
// (SendData_1030 empty-line = 0xFF) and documented in docs/protocol.md.

#ifndef SISTERHL2030_LINE_H
#define SISTERHL2030_LINE_H

#include <cstdint>
#include <vector>

namespace sisterhl2030 {

// Pack `line` with no previous-line reference (absolute / substitute of the
// whole row). An all-zero line is the single byte 0xFF. Otherwise the result
// starts with 0x01 followed by one substitute edit covering the row.
std::vector<uint8_t> encode_line(const std::vector<uint8_t>& line);

// Pack `line` as a delta against `reference` (the previous packed row, same
// length). An all-zero line is still 0xFF. Otherwise the first byte is the
// edit count, then substitute and/or repeat edits. Falls back to a single
// absolute substitute of the remainder if the edit budget (254) is exhausted.
std::vector<uint8_t> encode_line(const std::vector<uint8_t>& line,
                                 const std::vector<uint8_t>& reference);

}  // namespace sisterhl2030

#endif
