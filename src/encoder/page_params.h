// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SISTERHL2030_PAGE_PARAMS_H
#define SISTERHL2030_PAGE_PARAMS_H

#include <string>

namespace sisterhl2030 {

struct PageParams {
  int resolution = 600;  // 300, 600, or 1200 (HQ1200)
  int copies = 1;
  bool economode = false;
  std::string papersize = "A4";
  std::string mediatype = "REGULAR";
  std::string sourcetray = "TRAY1";
};

inline bool operator==(const PageParams& a, const PageParams& b) {
  return a.resolution == b.resolution && a.copies == b.copies &&
         a.economode == b.economode && a.papersize == b.papersize &&
         a.mediatype == b.mediatype && a.sourcetray == b.sourcetray;
}

}  // namespace sisterhl2030

#endif
