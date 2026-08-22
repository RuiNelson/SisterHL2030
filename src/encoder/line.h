// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SISTERHL2030_LINE_H
#define SISTERHL2030_LINE_H

#include <cstdint>
#include <vector>

namespace sisterhl2030 {

// Mode-1030 packed line. See docs/protocol.md.
std::vector<uint8_t> encode_line(const std::vector<uint8_t>& line);
std::vector<uint8_t> encode_line(const std::vector<uint8_t>& line,
                                 const std::vector<uint8_t>& reference);

}  // namespace sisterhl2030

#endif
