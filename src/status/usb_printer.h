// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SISTERHL2030_STATUS_USB_PRINTER_H
#define SISTERHL2030_STATUS_USB_PRINTER_H

#include <cstdint>
#include <string>

namespace sisterhl2030 {

constexpr uint16_t kBrotherVid = 0x04f9;
constexpr uint16_t kHl2030Pid = 0x0027;

// Bidirectional USB printer-class (04f9:0027). want_serial empty = first match.
bool pjl_query(const std::string& want_serial, const std::string& commands_crlf,
               std::string* response, std::string* error);

// INFO STATUS + PAGECOUNT + DRUMLIFE in one transaction.
bool pjl_query_supplies(const std::string& want_serial, std::string* response,
                        std::string* error);

}  // namespace sisterhl2030

#endif
