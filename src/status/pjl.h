// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Parse HL-2030 PJL status readback. Toner is a three-state sensor
// (OK / low / empty); drum remaining is estimated from DRUMLIFE against
// 12 000 rated pages. Map supply state from CODE; DISPLAY follows the
// printer's front-panel language and is a last-resort fallback.

#ifndef SISTERHL2030_STATUS_PJL_H
#define SISTERHL2030_STATUS_PJL_H

#include <string>

namespace sisterhl2030 {

// ---------------------------------------------------------------------------
// Supply-level constants
// ---------------------------------------------------------------------------

// Rated life of the DR-2000 drum, in pages. Remaining percent is
// (kDrumRatedPages - DRUMLIFE) / kDrumRatedPages.
constexpr int kDrumRatedPages = 12000;

// Percent reported for TonerState::low. The sensor is not a gauge: OK,
// low and empty become 100, this, and 0.
constexpr int kTonerLowPercent = 15;

// Percent at or below which the drum is flagged low (and the floor used
// when CODE=40130 reports "drum low" but DRUMLIFE still looks healthy).
constexpr int kDrumLowPercent = 10;

// Sentinel for "no reading" in toner_percent / drum_percent. Distinct from
// 0 (empty) and from PAPPL's -1 (unknown at the IPP layer).
constexpr int kLevelUnknown = -2;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

enum class TonerState { unknown, ok, low, empty };

// Decoded INFO STATUS + PAGECOUNT + DRUMLIFE. Flags default false / 0 /
// unknown so a partial reply is still usable.
struct PrinterStatus {
  bool have_status = false;  // saw a CODE= field
  int code = 0;              // PJL INFO STATUS CODE
  std::string display;       // trimmed 16-char PJL DISPLAY
  bool online = false;       // ONLINE=TRUE

  bool have_pagecount = false;
  int pagecount = 0;  // engine page counter

  bool have_drumlife = false;
  int drumlife = 0;  // pages the drum has already run

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

// ---------------------------------------------------------------------------
// Query construction
// ---------------------------------------------------------------------------

// One PJL command wrapped in UELs, ready to write to the printer.
std::string pjl_command(const std::string& command);

// INFO STATUS + PAGECOUNT + DRUMLIFE + a trailing ECHO, as one payload.
// ECHO exists to shake the lagged DRUMLIFE reply loose; it is not the
// completion sentinel — pjl_response_complete() does not wait for it.
std::string pjl_supply_query();

// True once a reply carries CODE= and DRUMLIFE=, i.e. everything
// pjl_supply_query() asked for that we actually parse.
bool pjl_response_complete(const std::string& text);

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

// PJL INFO response text (one or more @PJL INFO … blocks, form-feed
// separated) → toner, drum, and engine-intervention flags.
PrinterStatus parse_pjl_status(const std::string& text);

// Drum remaining as a percent of kDrumRatedPages. Returns kLevelUnknown
// for a negative drumlife.
int drum_remaining_percent(int drumlife);

// Short English label for logs and JSON: "OK", "low", "empty", "unknown".
const char* toner_state_label(TonerState s);

// ---------------------------------------------------------------------------
// IPP / CUPS presentation
// ---------------------------------------------------------------------------

// RFC 8011 printer-supply octet string for toner (index 1) and drum (index 2).
std::string printer_supply_octet(const PrinterStatus& st);

// "Black toner (TN-2000),Drum (DR-2000)" — the marker-names pair.
std::string printer_supply_description();

// Consumable names, so every surface calls them the same thing.
const char* toner_description();
const char* drum_description();

// ippeveprinter stderr lines (ATTR: / STATE:), newline-terminated. Kept
// for the retired façade's tests and for sister-status --ipp.
std::string ippeve_attr_lines(const PrinterStatus& st);

// ---------------------------------------------------------------------------
// URI helpers
// ---------------------------------------------------------------------------

// Serial number from a usb://…?serial= URI. Empty if the query is absent.
std::string serial_from_device_uri(const std::string& uri);

}  // namespace sisterhl2030

#endif
