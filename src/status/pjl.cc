// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

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

std::string upper_ascii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return s;
}

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

  const std::string d = upper_ascii(display);
  if (d.find("TONER") != std::string::npos) {
    if (d.find("END") != std::string::npos || d.find("EMPTY") != std::string::npos ||
        d.find("ESGOT") != std::string::npos || d.find("VAZIO") != std::string::npos ||
        d.find("LIFE") != std::string::npos) {
      return TonerState::empty;
    }
    if (d.find("LOW") != std::string::npos || d.find("BAIXO") != std::string::npos ||
        d.find("POUCO") != std::string::npos) {
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

const char* toner_state_label_pt(TonerState s) {
  switch (s) {
    case TonerState::ok:
      return "OK";
    case TonerState::low:
      return "baixo";
    case TonerState::empty:
      return "vazio";
    case TonerState::unknown:
    default:
      return "desconhecido";
  }
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

std::string printer_supply_description() {
  return "Toner preto (TN-2000),Tambor (DR-2000)";
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
  os << "STATE: -toner-low,-toner-empty,-opc-life-almost-over,-opc-life-over,"
        "-marker-waste-full-report,-marker-waste-almost-full-report,"
        "-marker-supply-low-warning\n";
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
  return os.str();
}

}  // namespace sisterhl2030
