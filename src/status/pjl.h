// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Parse HL-2030 PJL status readback. Toner is a 3-state sensor (OK / low /
// empty); drum remaining is estimated from DRUMLIFE against 12 000 pages.

#ifndef SISTERHL2030_STATUS_PJL_H
#define SISTERHL2030_STATUS_PJL_H

#include <string>

namespace sisterhl2030 {

constexpr int kDrumRatedPages = 12000;
constexpr int kTonerLowPercent = 15;
constexpr int kDrumLowPercent = 10;
constexpr int kLevelUnknown = -2;

enum class TonerState { unknown, ok, low, empty };

struct PrinterStatus {
  bool have_status = false;
  int code = 0;
  std::string display;  // trimmed 16-char PJL DISPLAY
  bool online = false;

  bool have_pagecount = false;
  int pagecount = 0;

  bool have_drumlife = false;
  int drumlife = 0;

  TonerState toner = TonerState::unknown;
  int toner_percent = kLevelUnknown;  // 0..100 or kLevelUnknown
  int drum_percent = kLevelUnknown;   // 0..100 or kLevelUnknown

  bool toner_low = false;
  bool toner_empty = false;
  bool drum_low = false;
  bool drum_empty = false;

  // Engine intervention: cover, jam, empty tray. Independent of toner.
  bool cover_open = false;
  bool media_jam = false;
  bool media_empty = false;
  bool media_needed = false;  // waiting for a sheet in the manual slot
};

// One PJL query, wrapped in UELs, ready to write to the printer.
std::string pjl_command(const std::string& command);

// The supply queries, as one payload. ECHO closes the transaction.
std::string pjl_supply_query();

// True once a reply carries everything pjl_supply_query() asked for.
bool pjl_response_complete(const std::string& text);

// PJL INFO response text (one or more @PJL INFO … blocks, form-feed separated).
PrinterStatus parse_pjl_status(const std::string& text);

int drum_remaining_percent(int drumlife);

const char* toner_state_label(TonerState s);

std::string printer_supply_octet(const PrinterStatus& st);
std::string printer_supply_description();

// Consumable names, so every surface calls them the same thing.
const char* toner_description();
const char* drum_description();

// ippeveprinter stderr lines (ATTR: / STATE:), newline-terminated.
std::string ippeve_attr_lines(const PrinterStatus& st);

std::string serial_from_device_uri(const std::string& uri);

}  // namespace sisterhl2030

#endif
