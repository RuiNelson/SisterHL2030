// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// PJL supply-query construction and INFO STATUS / PAGECOUNT / DRUMLIFE
// parsing. Transport is status/usb_printer.cc (IOKit) or sister_app.cpp
// (PAPPL device).

#include "status/pjl.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace sisterhl2030 {
namespace {

// ASCII uppercase, in place. DISPLAY matching is English-only and last resort.
std::string upper_ascii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return s;
}

// Strip leading spaces/tabs and trailing whitespace including CR/LF.
std::string trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                        s.back() == '\n')) {
    s.pop_back();
  }
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
    ++i;
  }
  return s.substr(i);
}

// First `key=` integer in `text` whose preceding character is not
// alphanumeric or `_`, so CODE= does not steal PAGECOUNT=.
bool extract_int_field(const std::string& text, const char* key, int* out) {
  const std::string prefix = std::string(key) + "=";
  size_t pos = 0;
  while ((pos = text.find(prefix, pos)) != std::string::npos) {
    if (pos > 0) {
      const unsigned char prev = static_cast<unsigned char>(text[pos - 1]);
      if (std::isalnum(prev) || prev == '_') {
        pos += prefix.size();
        continue;
      }
    }
    const char* p = text.c_str() + pos + prefix.size();
    char* end = nullptr;
    const long v = std::strtol(p, &end, 10);
    if (end != p) {
      *out = static_cast<int>(v);
      return true;
    }
    break;
  }
  return false;
}

// First `key="…"` value, trimmed. Used for DISPLAY.
bool extract_quoted(const std::string& text, const char* key, std::string* out) {
  const std::string prefix = std::string(key) + "=\"";
  const size_t pos = text.find(prefix);
  if (pos == std::string::npos) {
    return false;
  }
  const size_t start = pos + prefix.size();
  const size_t end = text.find('"', start);
  if (end == std::string::npos) {
    return false;
  }
  *out = trim(text.substr(start, end - start));
  return true;
}

// Toner from PJL CODE, with English DISPLAY as a last-resort fallback for
// unmapped codes. Ready/sleep/warming and operator-intervention codes that
// do not mention toner default to OK.
TonerState toner_from_code_and_display(int code, const std::string& display) {
  switch (code) {
    case 10006:  // Toner Low (PJL informational)
    case 40038:  // Low Toner, press Go
      return TonerState::low;
    case 40010:  // Install toner cartridge / no contact
      return TonerState::empty;
    default:
      break;
  }

  // Fallback for codes we do not map. DISPLAY follows the printer's own
  // front-panel language setting, so this only fires on an English panel;
  // CODE is the reliable signal.
  const std::string d = upper_ascii(display);
  if (d.find("TONER") != std::string::npos) {
    if (d.find("END") != std::string::npos || d.find("EMPTY") != std::string::npos ||
        d.find("LIFE") != std::string::npos) {
      return TonerState::empty;
    }
    if (d.find("LOW") != std::string::npos) {
      return TonerState::low;
    }
  }

  // Ready / sleep / warming / processing: sensor has not flagged low/empty.
  if (code == 10001 || code == 10002 || code == 10003 || code == 10023 ||
      code == 40000) {
    return TonerState::ok;
  }
  // Any other decoded STATUS still means we talked to the printer; default OK
  // unless the code is an operator-intervention we do not map.
  if (code > 0 && code < 40000) {
    return TonerState::ok;
  }
  if (code >= 40000 && code < 50000) {
    // Cover open / jam / etc. Toner not reported empty.
    return TonerState::ok;
  }
  return TonerState::unknown;
}

// Cover / jam / empty-tray / manual-wait flags from CODE, then DISPLAY.
void classify_engine(int code, const std::string& display, PrinterStatus* st) {
  switch (code) {
    case 40021:  // Brother TRG example: DISPLAY="12 COVER OPEN"
      st->cover_open = true;
      return;
    case 40022:  // HP/Brother 40xxx table: paper jam
      st->media_jam = true;
      return;
    default:
      break;
  }
  // 11xxx = paper-source status (empty, switching). 41xxx = empty with
  // nowhere else to pull from. Both suspend the job until someone loads
  // paper. Sleep (40000) is in the 40xxx range and is not empty.
  if ((code >= 11000 && code <= 11999) || (code >= 41000 && code <= 41999)) {
    st->media_empty = true;
    return;
  }

  // Same English-panel last resort as toner. CODE is the reliable signal.
  const std::string d = upper_ascii(display);
  if (d.find("COVER") != std::string::npos) {
    st->cover_open = true;
    return;
  }
  if (d.find("JAM") != std::string::npos) {
    st->media_jam = true;
    return;
  }
  if (d.find("NO PAPER") != std::string::npos ||
      d.find("PAPER EMPTY") != std::string::npos ||
      d.find("LOAD PAPER") != std::string::npos ||
      d.find("TRAY EMPTY") != std::string::npos) {
    if (d.find("MANUAL") != std::string::npos) {
      st->media_needed = true;
    } else {
      st->media_empty = true;
    }
  }
}

// Three-state sensor → percent: OK=100, low=kTonerLowPercent, empty=0.
int toner_percent_for(TonerState s) {
  switch (s) {
    case TonerState::ok:
      return 100;
    case TonerState::low:
      return kTonerLowPercent;
    case TonerState::empty:
      return 0;
    case TonerState::unknown:
    default:
      return kLevelUnknown;
  }
}

}  // namespace

int drum_remaining_percent(int drumlife) {
  if (drumlife < 0) {
    return kLevelUnknown;
  }
  int remaining = kDrumRatedPages - drumlife;
  if (remaining < 0) {
    remaining = 0;
  }
  return (remaining * 100 + kDrumRatedPages / 2) / kDrumRatedPages;
}

const char* toner_state_label(TonerState s) {
  switch (s) {
    case TonerState::ok:
      return "OK";
    case TonerState::low:
      return "low";
    case TonerState::empty:
      return "empty";
    case TonerState::unknown:
    default:
      return "unknown";
  }
}

std::string pjl_command(const std::string& command) {
  return "\x1b%-12345X@PJL\r\n@PJL " + command + "\r\n\x1b%-12345X";
}

std::string pjl_supply_query() {
  std::string out;
  for (const char* cmd : {"INFO STATUS", "INFO PAGECOUNT", "INFO DRUMLIFE",
                          "ECHO SisterHL2030"}) {
    out += pjl_command(cmd);
  }
  return out;
}

bool pjl_response_complete(const std::string& text) {
  return text.find("CODE=") != std::string::npos &&
         text.find("DRUMLIFE=") != std::string::npos;
}

PrinterStatus parse_pjl_status(const std::string& text) {
  PrinterStatus st;
  st.have_status = extract_int_field(text, "CODE", &st.code);
  extract_quoted(text, "DISPLAY", &st.display);
  {
    const size_t pos = text.find("ONLINE=");
    if (pos != std::string::npos) {
      const std::string rest = upper_ascii(text.substr(pos + 7, 8));
      st.online = rest.find("TRUE") != std::string::npos;
    }
  }
  st.have_pagecount = extract_int_field(text, "PAGECOUNT", &st.pagecount);
  st.have_drumlife = extract_int_field(text, "DRUMLIFE", &st.drumlife);

  if (st.have_status) {
    st.toner = toner_from_code_and_display(st.code, st.display);
    classify_engine(st.code, st.display, &st);
  }
  st.toner_percent = toner_percent_for(st.toner);
  st.toner_low = st.toner == TonerState::low;
  st.toner_empty = st.toner == TonerState::empty;

  if (st.have_drumlife) {
    st.drum_percent = drum_remaining_percent(st.drumlife);
    if (st.code == 40129) {
      st.drum_percent = 0;
    } else if (st.code == 40130 && st.drum_percent > kDrumLowPercent) {
      st.drum_percent = kDrumLowPercent;
    }
  }
  st.drum_empty = st.drum_percent == 0 || st.code == 40129;
  st.drum_low = !st.drum_empty && (st.drum_percent >= 0) &&
                (st.drum_percent <= kDrumLowPercent || st.code == 40130);
  return st;
}

const char* toner_description() { return "Black toner (TN-2000)"; }

const char* drum_description() { return "Drum (DR-2000)"; }

std::string printer_supply_description() {
  return std::string(toner_description()) + "," + drum_description();
}

std::string printer_supply_octet(const PrinterStatus& st) {
  const int toner = st.toner_percent;
  const int drum = st.drum_percent;
  char buf[512];
  std::snprintf(
      buf, sizeof(buf),
      "index=1;class=supplyThatIsConsumed;type=toner;unit=percent;"
      "maxcapacity=100;level=%d;colorantname=black;,"
      "index=2;class=supplyThatIsConsumed;type=opc;unit=percent;"
      "maxcapacity=100;level=%d;colorantname=unknown;",
      toner, drum);
  return buf;
}

std::string serial_from_device_uri(const std::string& uri) {
  const size_t q = uri.find("serial=");
  if (q == std::string::npos) {
    return {};
  }
  std::string s = uri.substr(q + 7);
  const size_t cut = s.find_first_of("& \t\r\n");
  if (cut != std::string::npos) {
    s.resize(cut);
  }
  return s;
}

std::string ippeve_attr_lines(const PrinterStatus& st) {
  std::ostringstream os;
  os << "ATTR: printer-supply=" << printer_supply_octet(st) << "\n";
  // The Supply Levels panel reads marker-levels, so refresh it too. CUPS
  // picks these up off the queue's backend on the next job.
  os << "ATTR: marker-levels=" << st.toner_percent << "," << st.drum_percent
     << "\n";
  os << "STATE: -toner-low,-toner-empty,-opc-life-almost-over,-opc-life-over,"
        "-marker-waste-full-report,-marker-waste-almost-full-report,"
        "-marker-supply-low-warning,-cover-open,-media-jam,-media-empty,"
        "-media-needed\n";
  if (st.toner_empty) {
    os << "STATE: +toner-empty\n";
  } else if (st.toner_low) {
    os << "STATE: +toner-low\n";
  }
  if (st.drum_empty) {
    os << "STATE: +opc-life-over\n";
  } else if (st.drum_low) {
    os << "STATE: +opc-life-almost-over\n";
  }
  if (st.cover_open) {
    os << "STATE: +cover-open\n";
  }
  if (st.media_jam) {
    os << "STATE: +media-jam\n";
  }
  if (st.media_empty) {
    os << "STATE: +media-empty\n";
  }
  if (st.media_needed) {
    os << "STATE: +media-needed\n";
  }
  return os.str();
}

}  // namespace sisterhl2030
