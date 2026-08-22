// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SISTERHL2030_HALFTONE_H
#define SISTERHL2030_HALFTONE_H

#include <cstdint>
#include <vector>

namespace sisterhl2030 {

// Floyd–Steinberg error diffusion with serpentine scanning.
// `toner` is width*height samples, 0 = paper white, 255 = solid black.
// On return each sample is 0 or 255.
void floyd_steinberg(uint8_t* toner, unsigned width, unsigned height);

// Pack one 0/255 toner row to 1-bit MSB-first (1 = black).
void pack_toner_row(const uint8_t* toner_row, unsigned width, uint8_t* packed);

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

}  // namespace sisterhl2030

#endif
