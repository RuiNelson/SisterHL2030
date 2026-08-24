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
// rotated cell, so gray levels = 2*cell*cell and the centre-to-centre
// spacing is cell*sqrt(2) pixels (screen ruling ~= dpi / (cell*sqrt(2)) lpi).
// Smaller cell trades fewer gray levels for a finer, sharper screen -- at a
// fixed output dpi those two move in lockstep, there is no free lunch.
//
// The default, cell=4, favours graphic precision over smooth tone: ~106 lpi
// at this driver's 600 dpi normal/high-quality output, 32 gray levels. That
// is a deliberate trade for a document/line-art printer, not a photo one --
// a coarser, higher-level screen (try cell=5: ~85 lpi, 50 levels) reads
// smoother on continuous-tone photos but blurs fine lines and small text
// into its coarser grid; a finer one (cell=3: ~141 lpi, 18 levels) trades
// further the other way. Draft quality halftones its already-downsampled
// 300 dpi buffer through the same default, landing near 53 lpi at the same
// 32 levels -- coarser dots are expected at draft, gray-level count is not
// resolution-dependent.
void clustered_dot_45(uint8_t* toner, unsigned width, unsigned height,
                      unsigned cell = 4);

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
