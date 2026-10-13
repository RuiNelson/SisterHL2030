// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
// A tiny PWG page for exercising the client's raster-stream copy path.
#include <cups/raster.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cstring>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  int fd = open(argv[1], O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) return 1;
  cups_raster_t* raster = cupsRasterOpen(fd, CUPS_RASTER_WRITE_PWG);
  if (!raster) { close(fd); return 1; }
  cups_page_header2_t header{};
  std::strcpy(header.MediaClass, "PwgRaster");
  std::strcpy(header.cupsPageSizeName, "iso_a6_105x148mm");
  header.HWResolution[0] = header.HWResolution[1] = 300;
  header.PageSize[0] = 298;
  header.PageSize[1] = 420;
  header.cupsPageSize[0] = 105.0f * 72.0f / 25.4f;
  header.cupsPageSize[1] = 148.0f * 72.0f / 25.4f;
  header.cupsWidth = header.cupsHeight = 64;
  header.cupsBitsPerColor = header.cupsBitsPerPixel = 8;
  header.cupsBytesPerLine = 64;
  header.cupsNumColors = 1;
  header.cupsColorOrder = CUPS_ORDER_CHUNKED;
  header.cupsColorSpace = CUPS_CSPACE_SW;
  header.NumCopies = 1;
  bool ok = cupsRasterWriteHeader2(raster, &header) != 0;
  std::array<unsigned char, 64> row;
  row.fill(128);
  for (unsigned y = 0; ok && y < header.cupsHeight; ++y)
    ok = cupsRasterWritePixels(raster, row.data(), row.size()) == row.size();
  cupsRasterClose(raster);
  close(fd);
  return ok ? 0 : 1;
}
