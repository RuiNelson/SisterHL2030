// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstring>
#include <string>

#include "status/pjl.h"

namespace {

int failures = 0;

// Count a failed assertion. The process still runs the rest of the suite.
void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++failures;
  }
}

}  // namespace

int main() {
  using sisterhl2030::parse_pjl_status;
  using sisterhl2030::TonerState;

  const std::string ready =
      "@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY=\"READY           \"\r\n"
      "ONLINE=TRUE\r\n\x0c"
      "@PJL INFO PAGECOUNT\r\nPAGECOUNT=3616\r\n\x0c"
      "@PJL INFO DRUMLIFE\r\nDRUMLIFE=3616\r\n\x0c";
  auto st = parse_pjl_status(ready);
  expect(st.have_status && st.code == 10001, "ready CODE");
  expect(st.display == "READY", "trim DISPLAY");
  expect(st.online, "ONLINE");
  expect(st.pagecount == 3616 && st.drumlife == 3616, "counters");
  expect(st.toner == TonerState::ok && st.toner_percent == 100, "toner OK -> 100%");
  expect(st.drum_percent == 70, "3616/12000 remaining rounds to 70%");
  expect(!st.toner_low && !st.drum_low, "not low");

  const std::string sleep =
      "@PJL INFO STATUS\r\nCODE=40000\r\nDISPLAY=\"SLEEP           \"\r\n"
      "ONLINE=TRUE\r\n\x0c";
  st = parse_pjl_status(sleep);
  expect(st.toner == TonerState::ok, "sleep is not toner empty");
  expect(st.display == "SLEEP", "sleep display");

  st = parse_pjl_status("@PJL INFO STATUS\r\nCODE=10006\r\nDISPLAY=\"TONER LOW       \"\r\n\x0c");
  expect(st.toner == TonerState::low && st.toner_percent == 15 && st.toner_low,
         "toner low");

  st = parse_pjl_status("@PJL INFO STATUS\r\nCODE=40010\r\nDISPLAY=\"                \"\r\n\x0c");
  expect(st.toner == TonerState::empty && st.toner_percent == 0 && st.toner_empty,
         "toner empty");

  st = parse_pjl_status(
      "@PJL INFO STATUS\r\nCODE=40021\r\nDISPLAY=\"12 COVER OPEN   \"\r\n"
      "ONLINE=FALSE\r\n\x0c");
  expect(st.cover_open && !st.media_jam && !st.media_empty, "cover open CODE");
  expect(st.toner == TonerState::ok, "cover open is not toner empty");

  st = parse_pjl_status(
      "@PJL INFO STATUS\r\nCODE=40022\r\nDISPLAY=\"PAPER JAM       \"\r\n"
      "ONLINE=FALSE\r\n\x0c");
  expect(st.media_jam && !st.cover_open, "paper jam CODE");

  st = parse_pjl_status(
      "@PJL INFO STATUS\r\nCODE=41100\r\nDISPLAY=\"NO PAPER        \"\r\n"
      "ONLINE=FALSE\r\n\x0c");
  expect(st.media_empty && !st.media_jam, "tray empty 41xxx");

  st = parse_pjl_status(
      "@PJL INFO STATUS\r\nCODE=30000\r\nDISPLAY=\"COVER OPEN      \"\r\n\x0c");
  expect(st.cover_open, "unmapped code, display says cover");
  st = parse_pjl_status(
      "@PJL INFO STATUS\r\nCODE=30000\r\nDISPLAY=\"NO PAPER FED MANUAL\"\r\n\x0c");
  expect(st.media_needed && !st.media_empty, "manual slot waiting for a sheet");

  st = parse_pjl_status(ready);
  expect(!st.cover_open && !st.media_jam && !st.media_empty && !st.media_needed,
         "ready has no engine intervention");

  st = parse_pjl_status(
      "@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY=\"READY           \"\r\n\x0c"
      "@PJL INFO DRUMLIFE\r\nDRUMLIFE=12000\r\n\x0c");
  expect(st.drum_percent == 0 && st.drum_empty, "drum life end");

  st = parse_pjl_status("@PJL INFO DRUMLIFE\r\nDRUMLIFE=11400\r\n\x0c");
  expect(st.drum_percent == 5 && st.drum_low, "drum low at 5%");

  // Unmapped CODE: fall back to the English DISPLAY wording.
  st = parse_pjl_status(
      "@PJL INFO STATUS\r\nCODE=30000\r\nDISPLAY=\"TONER LOW       \"\r\n\x0c");
  expect(st.toner == TonerState::low, "unmapped code, display says low");
  st = parse_pjl_status(
      "@PJL INFO STATUS\r\nCODE=30000\r\nDISPLAY=\"TONER EMPTY     \"\r\n\x0c");
  expect(st.toner == TonerState::empty, "unmapped code, display says empty");

  // What status_cb hands papplPrinterSetSupplies, which PAPPL turns into
  // marker-levels and marker-names on its own.
  const sisterhl2030::PrinterStatus ready_st = parse_pjl_status(ready);
  expect(ready_st.toner_percent == 100 && ready_st.drum_percent == 70,
         "levels for the Supply Levels panel");
  expect(!ready_st.cover_open, "ready clears cover-open");
  expect(parse_pjl_status(
             "@PJL INFO STATUS\r\nCODE=40022\r\nDISPLAY=\"PAPER JAM       \"\r\n\x0c")
             .media_jam,
         "jam reports media-jam");
  const sisterhl2030::PrinterStatus empty_st = parse_pjl_status("");
  expect(empty_st.toner_percent == sisterhl2030::kLevelUnknown &&
             empty_st.drum_percent == sisterhl2030::kLevelUnknown,
         "unknown levels stay unknown, not 0");
  expect(std::strstr(sisterhl2030::toner_description(), "TN-2000") != nullptr,
         "TN-2000 name");
  expect(std::strstr(sisterhl2030::drum_description(), "DR-2000") != nullptr,
         "DR-2000 name");
  expect(sisterhl2030::drum_remaining_percent(0) == 100, "new drum");
  expect(sisterhl2030::drum_remaining_percent(6000) == 50, "half drum");
  expect(sisterhl2030::drum_remaining_percent(20000) == 0, "over-life drum");

  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("ok");
  return 0;
}
