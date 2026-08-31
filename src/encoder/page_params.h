// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Per-page PJL/PCL settings the encoder emits in the page header.
// Quality mapping (draft / normal / high → resolution + ECONOMODE) lives
// in the filter and in sister_app.cpp; this struct is the already-resolved
// result. See docs/protocol.md.

#ifndef SISTERHL2030_PAGE_PARAMS_H
#define SISTERHL2030_PAGE_PARAMS_H

#include <string>

namespace sisterhl2030 {

// Settings written once per page (or when they change mid-job).
struct PageParams {
  // PJL RESOLUTION in dpi: 300 (draft), 600 (normal), or 1200 (HQ1200).
  // 1200 is the caller's name for RAS1200MODE; the PJL RESOLUTION field
  // itself is still sent as 600. Must match the bitmap the encoder is
  // given — a 300 dpi raster with RESOLUTION = 600 prints at half size.
  int resolution = 600;

  // PCL copy count (`ESC & l <n> X`). Clamped to at least 1 at emit time.
  int copies = 1;

  // PJL ECONOMODE. Draft and normal set this; HQ1200 does not. The flag
  // is sent to the engine and is not an input to the darkness model.
  bool economode = false;

  // PJL PAPER name: A4, LETTER, LEGAL, … Unknown sizes are sent as A4.
  std::string papersize = "A4";

  // PJL MEDIATYPE: REGULAR, THIN, THICK, THICK2, TRANSPARENCY, ENVELOPES,
  // ENVTHICK, ENVTHIN, RECYCLED. Thick stock and envelopes also select
  // the manual slot even if sourcetray is TRAY1.
  std::string mediatype = "REGULAR";

  // Paper source: TRAY1 (cassette) or MANUAL / MPTRAY (manual slot).
  std::string sourcetray = "TRAY1";
};

inline bool operator==(const PageParams& a, const PageParams& b) {
  return a.resolution == b.resolution && a.copies == b.copies &&
         a.economode == b.economode && a.papersize == b.papersize &&
         a.mediatype == b.mediatype && a.sourcetray == b.sourcetray;
}

}  // namespace sisterhl2030

#endif
