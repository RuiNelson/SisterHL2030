// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Colour → toner, engine darkness model, and the two halftone screens
// (Atkinson error diffusion and AM45 clustered-dot). Public constants are
// the calibrated numbers; change them with the calibration sheet, not by
// adding per-mode fudge factors.

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

// sRGB 8-bit RGB → toner 0..255.
//
// Rec.709 luminance in linear light, then the laser curve -- and then a floor
// that keeps saturated colour legible. The luminance part is the standard
// colorimetric answer and it is exactly right for "how bright is this?":
// pure yellow really does reflect 93 % as much light as white. It is the
// WRONG question for a printer with one black toner, because it maps yellow,
// cyan and light greens onto bare paper and a chart that was readable in
// colour comes out blank.
//
// So a fully saturated colour is not allowed to print lighter than
// `kChromaFloor`, and the floor scales with saturation, which means neutrals
// are untouched: greys, text and the neutral parts of photographs come out
// bit for bit as they did before, and only actual colour is lifted. A
// deliberate departure from colorimetry, in the direction of keeping colours
// off the paper -- which is what a mono print is for.
//
// Reached only when the raster arrives as sRGB. That depends on the queue's
// PPD carrying a `*ColorModel RGB` entry, which in turn depends on
// `ppm_color` at the time the queue was generated -- see sister_app.cpp.
// Confirm with:
//   cupsfilter -p /etc/cups/ppd/<queue>.ppd -m application/vnd.cups-raster \
//       file.pdf | ...   (cupsColorSpace 19 = sRGB, 18 = ColorSync already
//                         reduced it to grey and this function never runs)
uint8_t rgb_to_toner(uint8_t r, uint8_t g, uint8_t b);

// Minimum toner coverage for a fully saturated colour, 0..1, reached as
// `kChromaFloor * saturation^kChromaKnee`. Setting the floor to 0 restores
// pure colorimetric luminance.
//
// Both are measured from Normal prints. The floor: of the six primary ramps,
// red (33 %), blue (53 %) and magenta (29 %) were judged right while green
// (15 %), cyan (11 %) and yellow (3 %) were called too light, so it sits at
// the bottom of the range that was accepted -- lifting exactly the three that
// failed and leaving the three that passed alone.
//
// The knee: on the white-to-green ramp the density that used to appear only
// at full strength was wanted by about 8.5 % of the way along. An exponent
// below 1 bends the curve up steeply out of white to put it there. That is a
// strong choice and it is not free -- at 2 % saturation a barely-tinted
// near-white already carries 9 % coverage, so pale tints in photographs go
// greyer than luminance would make them. Raise the knee toward 1 to soften
// it (1.0 makes the floor simply proportional to saturation).
constexpr float kChromaFloor = 0.29f;
constexpr float kChromaKnee = 0.3042f;

// DeviceGray / sGray (0 = black, 255 = white) → toner 0..255. Equal-RGB
// path through rgb_to_toner, so a page of greys matches the sRGB path.
uint8_t device_gray_to_toner(uint8_t gray);

// DeviceK (0 = white, 255 = black) → toner 0..255. Skips the sRGB
// round-trip; only the laser curve is applied.
uint8_t device_k_to_toner(uint8_t k);

// Double width by pixel replication: the 600 dpi raster resampled onto
// HQ1200's 1200x600 addressable grid, so the halftone runs at the device's
// own pitch instead of being computed at 600 and stretched afterwards.
// Replication and not interpolation on purpose -- there is no extra detail
// in the source to recover, and interpolating would put a half-pixel ramp on
// every glyph edge for nothing. `resample_to_grid` is the usual way in.
void nn_upsample_2x_x(std::vector<uint8_t>& toner, unsigned& width,
                      unsigned height);

// ---------------------------------------------------------------------------
// Engine dot transfer
// ---------------------------------------------------------------------------
//
// The addressable grid the halftone is computed on. HQ1200's bitmap is twice
// the 600 dpi page in BOTH axes (docs/protocol.md, `paperinf`), but the engine
// only resolves 1200 along the scan line -- Brother's own PPD calls the mode
// `1200x600dpi`. So the halftone is computed at 1200x600 and each dithered row
// is emitted twice to fill the doubled bitmap.
struct DeviceGrid {
  int dpi_x = 600;  // samples per inch along the scan line
  int dpi_y = 600;  // samples per inch down the page
};

// Addressable grid for `pjl_resolution` (PageParams::resolution):
// 300 → 300×300, 600 → 600×600, 1200 (RAS1200MODE) → 1200×600.
DeviceGrid device_grid(int pjl_resolution);

// Resample a square `src_dpi` contone page onto `grid`, in place. Box-average
// going down, pixel-replicate going up, each axis independently. Call this
// BEFORE the halftone so the dither runs at the pitch the engine actually
// addresses -- halftoning first and rescaling the 1-bit result afterwards
// throws away exactly the precision the finer grid was for.
void resample_to_grid(std::vector<uint8_t>& toner, unsigned& width,
                      unsigned& height, int src_dpi, const DeviceGrid& grid);

// The engine's unprintable margins, in hundredths of a millimetre. Brother's
// own PPD declares the same box for every cassette size -- 18 pt left/right
// and 12 pt top/bottom, symmetric on both axes (LinuxDrivers/
// cupswrapperHL2030-2.0.1, `*ImageableArea`) -- and its driver never puts
// more than that on the wire: captures/official-hello-letter-600.bin is
// 6400 lines for Letter, which is 768 pt to the line.
constexpr int kEngineMarginLeftRight = 635;  // 18 pt
constexpr int kEngineMarginTopBottom = 423;  // 12 pt

// Crop a full-sheet page down to that imageable box, in place and centred.
// `sheet_w`/`sheet_h` are the media size in hundredths of a millimetre.
//
// Sister advertises 0.01 mm margins so the print dialog cannot decide to
// scale-to-fit (see `driver_cb`), and the price is that CUPS rasterises the
// WHOLE sheet: 5100x6600 for Letter against an engine that images 4800x6400,
// 4960x7015 for A4 against 4658x6817. Those extra bytes overrun the band
// decoder's line buffer and scanlines come back blank -- so the crop happens
// here instead, before the halftone, and the wire carries only what the
// engine can paint.
//
// A page already at or inside the box is left alone, so this is a no-op if
// the raster ever starts arriving correctly sized.
void crop_to_imageable(std::vector<uint8_t>& toner, unsigned& width,
                       unsigned& height, int sheet_w, int sheet_h,
                       const DeviceGrid& grid);

// Physical size of one device pixel, in micrometres.
inline float pixel_um(int dpi) { return 25400.0f / static_cast<float>(dpi); }

// How this engine turns a requested area coverage into toner on paper.
//
// Electrophotography does not reproduce the pattern it is handed. The latent
// image is the laser spot convolved with the pattern and then developed
// against a threshold, and the practical consequence for a halftone is that
// EVERYTHING WITHIN `suppression_um` OF A TONER/PAPER BOUNDARY RESOLVES
// TOWARD WHICHEVER PHASE SURROUNDS IT. An isolated black dot is all boundary,
// so it never reaches threshold and does not develop -- highlights die. An
// isolated white hole is all boundary too, so its neighbours' toner closes it
// -- shadows fill. Both push the same way: what lands on paper has MORE
// contrast than what was asked for.
//
// That is measured, not assumed. Printed at Normal on the HL-2030, an 11 %
// patch that laid 23 % of its pixels down came back looking like 5 %, while
// an 80 % patch that laid 82 % down came back solid. A single suppression
// length reproduces both, and also -- with nothing further fitted -- the
// crush point, the invisible 5 % patch, the 90 %/100 % patches being
// indistinguishable, and all six colour ramps.
//
// How much of the pattern is inside that band is set by its BOUNDARY LENGTH,
// which is why the effect depends on resolution and on the screen: a
// dispersed 600 dpi screen is nearly all boundary, a 300 dpi one much less,
// and a clustered AM dot has the least of all. So one length predicts every
// mode instead of each being measured separately. It enters as a gain on the
// pattern's log-odds -- the standard shape for a threshold process, monotone
// for any gain, and fixed at 0, 0.5 and 1.
//
// ECONOMODE IS NOT PART OF THIS MODEL, deliberately. It thins the toner layer
// on the printer's side of the wire, and the driver's job is to ask for the
// coverage it wants, not to chase what the engine then does with the request.
// Compensating for it here would mean asking for MORE coverage precisely in
// the modes chosen to use less -- fighting the setting instead of honouring
// it. So the model is purely geometric: grid and screen in, tone curve out,
// identical whether or not the page will be printed with ECONOMODE on. The
// PJL flag is still sent (see job.cc); it just does not feed back into the
// halftone.
//
// `density` is not the engine at all -- it is intent. It is a gamma on the
// requested coverage, and the one knob for "the page should be darker or
// lighter overall" once the shape above is right. Keep it out of the physics.
//
// The numbers live per screen in ScreenCalibration below, not here -- build
// one of these with `engine_transfer()`.
struct DotTransfer {
  float pixel_w_um = 42.333f;
  float pixel_h_um = 42.333f;
  float suppression_um = 0.0f;
  float density = 1.0f;  // >1 darker, <1 lighter; 1 = reproduce what was asked
  // Ink limit: the transfer table never asks the screen for more than this
  // fraction of full black, however dark the page requests. 1.0 (the
  // default) leaves solid black solid; below that, even a nominal 255
  // dithers at the capped level instead of printing flat. Unlike `density`,
  // which bends the whole tone curve, this only clips the top of it.
  float max_toner = 1.0f;
  bool clustered = false;  // AM45 rather than a dispersed (FM) screen
  unsigned cell = 4;       // AM45 dot radius in device pixels
  // Whether to invert the screen's own flat-field response as well as the
  // engine's erosion. Worth doing for a screen that clips its own tone scale
  // (Atkinson), pointless for one that is already linear (AM45).
  bool linearize = false;
};

// Boundary length of the halftone pattern per unit area, in 1/um, when the
// pattern covers fraction `c` (0..1) of the paper. This is the whole
// geometric content of the model.
float boundary_density(const DotTransfer& dt, float c);

// Flat-field response of the SCREEN itself, measured rather than assumed:
// `[i]` is the fraction of pixels (0..255) that come out black when a field
// of constant value `i` is dithered. An ordered screen is very nearly the
// identity here -- it blackens exactly as many pixels per cell as asked. A
// dispersed error-diffusion screen is not: Atkinson throws away two eighths
// of its error, so a flat field below about 13 % produces no dots at all and
// one above about 87 % goes fully solid, with roughly 1.45x contrast in
// between. That is Atkinson's signature crispness, and also a real loss of
// highlight and shadow -- folding it into the model means the requested tone
// survives while the scattered-dot texture stays exactly as it was.
//
// `cell` is meaningful only for the clustered screen; a dispersed one has no
// cell and ignores it, so any `cell` gives the same table there.
//
// Computed once per thread per screen by actually dithering flat patches, so
// it can never drift away from what `atkinson()` and `clustered_dot_45()` do.
const std::vector<uint8_t>& screen_response(bool clustered, unsigned cell);

// The engine's contrast gain: 1 means it reproduces patterns faithfully,
// higher means it suppresses the minority phase harder. Derived from the
// suppression length and the pattern's boundary density, so it falls out of
// the grid and the screen rather than being a per-mode number.
float contrast_gain(const DotTransfer& dt);

// Forward model of the ENGINE alone: the coverage of a pattern handed to the
// drum in, the coverage that develops out.
float printed_coverage(const DotTransfer& dt, float pattern_coverage);

// The whole chain: nominal coverage in, coverage on paper out. This is
// `printed_coverage` composed with `screen_response`.
float paper_coverage(const DotTransfer& dt, float nominal);

// Inverse of `paper_coverage`, tabulated: `lut[i]` is the nominal coverage
// to ask the dither for when `i/255` is the coverage wanted on paper. Built by
// sampling the forward model and inverting it, under a running maximum so a
// non-monotonic forward curve (possible when the erosion approaches the pixel
// pitch, e.g. a dispersed screen at 1200 dpi) still yields a usable table.
//
// Both ends are anchored rather than inverted: 0 stays 0 and 255 stays 255.
// A dispersed screen saturates well before 255, so a pure inversion would map
// full black to whatever first went solid on a flat field -- the same result
// on a flat field, but on text and edges it would hand error diffusion a
// value to dither instead of a solid. `max_toner` below 1 overrides that
// anchor on purpose: it caps the whole table, 255 included, at that fraction
// of black so the darkest the screen ever asks for is the ink limit rather
// than a flat solid.
std::vector<uint8_t> dot_transfer_lut(const DotTransfer& dt);

// Apply such a table to a toner buffer, in place.
void apply_transfer(uint8_t* toner, size_t n, const std::vector<uint8_t>& lut);

// ---------------------------------------------------------------------------
// Per-screen calibration
// ---------------------------------------------------------------------------
//
// Two independent sets of numbers, deliberately. The screens lay ink down by
// different geometry and were calibrated by different means, so re-measuring
// one must never move the other: editing the Atkinson numbers below cannot
// change a single bit of AM45 output, and the reverse.

// Atkinson (dispersed / FM), the "Pencil style" build.
//
// `kAtkinsonSuppressionUm` is MEASURED, from a Normal-quality print of
// test_fixtures/calibration.pdf on the HL-2030: the 11 % patch laid 23 % of
// its pixels and came back looking like 5 %, which fixes the gain at 2.38 and
// the length at 58 um. Fitted to that one patch, it then reproduced the rest
// of the sheet -- the crush starting at 80 %, 90 % and 100 % being
// indistinguishable, the 5 % patch invisible, and all six colour ramps --
// with nothing else adjusted.
//
// It is the ONLY measured number here, and every mode's tone curve is derived
// from it geometrically. There is deliberately no ECONOMODE term to go with
// it: the one that used to sit here was never measured either, just the ratio
// of the two hand-tuned per-mode gains this model replaced. Rather than
// measure a compensation for ECONOMODE, the driver does not compensate at all
// -- see "ECONOMODE IS NOT PART OF THIS MODEL" above.
//
// To re-measure: print the sheet, read the highlight wedge. Adjust these
// numbers, not the modes.
constexpr float kAtkinsonSuppressionUm = 58.4f;
// Overall darkness, once the shape is right: >1 darker, <1 lighter. Starts
// neutral, so the page reproduces the coverage it was asked for. This is the
// knob for "too light" / "too dark" -- nothing else.
constexpr float kAtkinsonDensity = 1.0f;
// Atkinson clips its own tone scale hard at both ends (see `screen_response`),
// so its response is worth inverting along with the engine's.
constexpr bool kAtkinsonLinearize = true;
// Ink limit: Atkinson was printing darker than it needed to, so cap how much
// toner it is ever allowed to put down, independent of the darkness model
// above. Draft and Normal cap at 80 %, Fine at 90 % -- every mode still
// reaches a clearly dark black, just short of a flat solid.
//
// Keyed on the print MODE, not on the ECONOMODE flag those modes happen to
// set. The two agree today (draft and normal are the ECONOMODE-on modes), but
// they are different things: this is an intent knob about how much toner a
// quality level deserves, and it must not silently follow if the ECONOMODE
// mapping in the filter ever changes.
constexpr float kAtkinsonMaxTonerDraftNormal = 0.80f;
constexpr float kAtkinsonMaxTonerFine = 0.90f;

// AM45 (clustered dot at 45 degrees), the "Newspaper style" build.
//
// Calibrated the only way that really counts, by looking at prints: this
// screen was judged right as it is, so it asks for no correction at all and
// its transfer table is the identity. That is a legitimate calibration
// result, not a placeholder -- and it is consistent with the model, since a
// clustered blob has far less boundary per unit area than the same coverage
// scattered as single pixels, and an ordered screen reproduces the coverage
// it was asked for almost exactly.
//
// If AM45 ever does need correcting, measure it with the same sheet and set
// these; nothing about Atkinson is involved.
constexpr float kAm45SuppressionUm = 0.0f;
constexpr float kAm45Density = 1.0f;
constexpr bool kAm45Linearize = false;
constexpr unsigned kAm45Cell = 4;  // also the dot radius passed to the screen
// AM45 was judged right on paper as it prints today -- no ink cap.
constexpr float kAm45MaxTonerDraftNormal = 1.0f;
constexpr float kAm45MaxTonerFine = 1.0f;

struct ScreenCalibration {
  bool clustered;     // AM45 rather than Atkinson
  unsigned cell;      // AM45 dot radius in device pixels; unused for Atkinson
  float suppression_um;
  float density;
  bool linearize;
  // Ink limit (fraction of full black), per print mode -- see
  // `DotTransfer::max_toner` and `ink_limit()`.
  float max_toner_draft_normal;
  float max_toner_fine;
  const char* name;  // "Atkinson" or "AM45", for logs
};

constexpr ScreenCalibration kAtkinsonScreen = {
    false,
    0,
    kAtkinsonSuppressionUm,
    kAtkinsonDensity,
    kAtkinsonLinearize,
    kAtkinsonMaxTonerDraftNormal,
    kAtkinsonMaxTonerFine,
    "Atkinson"};
constexpr ScreenCalibration kAm45Screen = {
    true,
    kAm45Cell,
    kAm45SuppressionUm,
    kAm45Density,
    kAm45Linearize,
    kAm45MaxTonerDraftNormal,
    kAm45MaxTonerFine,
    "AM45"};

// Whichever screen this build prints with, chosen by SISTER_HALFTONE_SCREEN
// at configure time. Constexpr, so the branch on `.clustered` at a call site
// folds away and only one screen is actually reachable in the binary.
constexpr ScreenCalibration active_screen() {
#if defined(SISTERHL2030_HALFTONE_AM45)
  return kAm45Screen;
#else
  return kAtkinsonScreen;
#endif
}

// This screen's ink limit for the mode that `grid` belongs to. Fine is the
// 1200x600 grid -- the only one with non-square pixels, because it is the only
// mode where the engine resolves more across the scan than down it.
float ink_limit(const ScreenCalibration& screen, const DeviceGrid& grid);

// Fill in a DotTransfer for one screen on one device grid. There is no
// ECONOMODE argument on purpose: the flag does not enter the darkness model
// at all, so the same grid always yields the same tone curve.
DotTransfer engine_transfer(const ScreenCalibration& screen,
                            const DeviceGrid& grid);

}  // namespace sisterhl2030

#endif
