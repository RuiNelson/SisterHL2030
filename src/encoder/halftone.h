// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SISTERHL2030_HALFTONE_H
#define SISTERHL2030_HALFTONE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sisterhl2030 {

// Atkinson error diffusion with serpentine scanning.
// `toner` is width*height samples, 0 = paper white, 255 = solid black.
// On return each sample is 0 or 255.
void atkinson(uint8_t* toner, unsigned width, unsigned height);

// Clustered-dot ordered dither (AM halftoning), screen angle 45°.
// `toner` is width*height samples, 0 = paper white, 255 = solid black.
// On return each sample is 0 or 255. `cell` is the screen's dot radius in
// device pixels: the growing round dot is built from a 2*cell*cell-pixel
// rotated cell, so the centre-to-centre spacing is cell*sqrt(2) pixels
// (screen ruling ~= dpi / (cell*sqrt(2)) lpi). Larger cell = coarser screen.
// The default, cell=5, is tuned for this driver's 600 dpi normal/high-quality
// output: ~85 lpi, the classic laser "fine screen" ruling, giving 50 gray
// levels -- enough steps to keep gradients smooth while the dot stays coarse
// enough that this engine's toner and fuser reproduce it cleanly. Draft
// quality halftones its already-downsampled 300 dpi buffer through the same
// default, landing near 42 lpi; that coarser dot is expected at draft.
void clustered_dot_45(uint8_t* toner, unsigned width, unsigned height,
                      unsigned cell = 5);

// Pack one 0/255 toner row to 1-bit MSB-first (1 = black).
void pack_toner_row(const uint8_t* toner_row, unsigned width, uint8_t* packed);

// Same packing, each pixel emitted twice (HQ1200 nearest-neighbour of 1-bit).
// `packed` must hold (2*width+7)/8 bytes.
void pack_toner_row_2x(const uint8_t* toner_row, unsigned width, uint8_t* packed);

// sRGB 8-bit RGB → toner 0..255 (perceptual Rec.709 luma, highlight-preserving laser curve).
uint8_t rgb_to_toner(uint8_t r, uint8_t g, uint8_t b);

// DeviceGray (0 = black, 255 = white) → toner.
uint8_t device_gray_to_toner(uint8_t gray);

// DeviceK (0 = white, 255 = black) → toner.
uint8_t device_k_to_toner(uint8_t k);

// Halve width and height with a 2×2 box filter (draft 600→300). No-op if
// either dimension is < 2. `toner` is resized to the new width*height.
void box_downsample_2x(std::vector<uint8_t>& toner, unsigned& width,
                       unsigned& height);

// Double width and height by nearest-neighbour (Fine 600→1200). HQ1200
// rasters are twice the 600 dpi paperinf size; a 600 dpi bitmap with
// RAS1200MODE prints at half size.
void nn_upsample_2x(std::vector<uint8_t>& toner, unsigned& width,
                    unsigned& height);

// Multiply each toner sample by `gain` (1 = unchanged, >1 darker, <1 lighter).
void scale_coverage(uint8_t* toner, size_t n, float gain);

}  // namespace sisterhl2030

#endif
