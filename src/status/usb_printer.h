// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// IOKit USB printer-class bulk I/O for the HL-2030. The PAPPL app talks
// to the same device through papplDeviceRead/Write; this path is for
// sister-status and for exercising PJL with no PAPPL in the way.

#ifndef SISTERHL2030_STATUS_USB_PRINTER_H
#define SISTERHL2030_STATUS_USB_PRINTER_H

#include <cstdint>
#include <string>

namespace sisterhl2030 {

// Brother USB vendor ID.
constexpr uint16_t kBrotherVid = 0x04f9;

// HL-2030 product ID (the whole 2030 series on this VID).
constexpr uint16_t kHl2030Pid = 0x0027;

// Open the matching printer-class device, write `commands_crlf` as one PJL
// command, and collect the reply. `want_serial` empty means the first
// 04f9:0027 on the bus. Returns false on open/write failure or an empty
// reply; `error` then holds a short English reason.
bool pjl_query(const std::string& want_serial, const std::string& commands_crlf,
               std::string* response, std::string* error);

// INFO STATUS + PAGECOUNT + DRUMLIFE + ECHO in one transaction. Drains the
// IN pipe first so a leftover reply from the last open is not parsed as
// this one. Stops as soon as pjl_response_complete() is true.
bool pjl_query_supplies(const std::string& want_serial, std::string* response,
                        std::string* error);

}  // namespace sisterhl2030

#endif
