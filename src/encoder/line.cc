// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Mode-1030 line codec. Grammar recovered from libbrcomplpr2.so
// (SendData_1030 empty-line = 0xFF) and documented in docs/protocol.md.
// Edit packing matches the published brlaser description (Peter De Wachter,
// GPL-2+), used as a cross-check.

#include "encoder/line.h"

#include <algorithm>
#include <cassert>

namespace sisterhl2030 {
namespace {

// Extra length that does not fit in the opcode nibble/bits. Values below
// 255 are one byte; 255 or more is `value/255` bytes of 0xFF plus the
// remainder, matching SendData_1030.
void write_overflow(int value, std::vector<uint8_t>* out) {
  if (value < 0) {
    return;
  }
  if (value < 255) {
    out->push_back(static_cast<uint8_t>(value));
    return;
  }
  out->insert(out->end(), static_cast<size_t>(value / 255), 255);
  out->push_back(static_cast<uint8_t>(value % 255));
}

// Substitute edit: copy `[first, last)` into the output at `offset` bytes
// past the last decoded byte. Opcode packs offset (4 bits, max 15) and
// count-minus-one (3 bits, max 7); the rest is overflow plus the payload.
template <typename Iterator>
void write_substitute(int offset, Iterator first, Iterator last,
                      std::vector<uint8_t>* out) {
  const int count = static_cast<int>(std::distance(first, last)) - 1;
  const int offset_low = std::min(offset, 15);
  const int count_low = std::min(count, 7);
  out->push_back(static_cast<uint8_t>((offset_low << 3) | count_low));
  write_overflow(offset - 15, out);
  write_overflow(count - 7, out);
  out->insert(out->end(), first, last);
}

// Repeat edit: write `count` copies of `value` at `offset`. Opcode high bit
// is set; offset uses 2 bits (max 3) and count-minus-two uses 5 bits (max 31).
void write_repeat(int offset, int count, uint8_t value,
                  std::vector<uint8_t>* out) {
  count -= 2;
  const int offset_low = std::min(offset, 3);
  const int count_low = std::min(count, 31);
  out->push_back(static_cast<uint8_t>(0x80 | (offset_low << 5) | count_low));
  write_overflow(offset - 3, out);
  write_overflow(count - 31, out);
  out->push_back(value);
}

// True if every byte is 0 (a white packed scanline).
bool all_zeros(const std::vector<uint8_t>& buf) {
  return std::none_of(buf.begin(), buf.end(), [](uint8_t b) { return b != 0; });
}

// Advance both iterators over the matching prefix. Returns how many bytes
// were skipped (the edit offset).
template <typename It1, typename It2>
int skip_matching(It1* first1, It1 last1, It2* first2) {
  auto mismatch = std::mismatch(*first1, last1, *first2);
  const int skipped = static_cast<int>(std::distance(*first1, mismatch.first));
  *first1 = mismatch.first;
  *first2 = mismatch.second;
  return skipped;
}

// Run of identical bytes starting at `first`. Zero on an empty range.
template <typename Iterator>
int repeat_length(Iterator first, Iterator last) {
  if (first == last) {
    return 0;
  }
  const uint8_t k = *first;
  auto it = std::find_if(std::next(first), last,
                         [k](uint8_t x) { return x != k; });
  return static_cast<int>(std::distance(first, it));
}

// How many bytes to emit as a substitute rather than a repeat: stops at a
// two-byte match with the reference, or at a three-byte run of the same
// value (which a repeat edit packs more tightly).
template <typename It1, typename It2>
int substitute_length(It1 first1, It1 last1, It2 first2) {
  if (first1 == last1) {
    return 0;
  }
  It1 it1 = first1;
  It2 it2 = first2;
  It1 next1 = std::next(first1);
  It2 next2 = std::next(first2);
  It1 prev1 = first1;
  while (next1 != last1) {
    if (*it1 == *it2 && *next1 == *next2) {
      return static_cast<int>(std::distance(first1, it1));
    }
    if (*it1 == *next1 && *it1 == *prev1) {
      return static_cast<int>(std::distance(first1, prev1));
    }
    prev1 = it1;
    it1 = next1;
    it2 = next2;
    ++next1;
    ++next2;
  }
  return static_cast<int>(std::distance(first1, last1));
}

}  // namespace

std::vector<uint8_t> encode_line(const std::vector<uint8_t>& line) {
  if (all_zeros(line)) {
    return {0xFF};
  }
  std::vector<uint8_t> out;
  out.reserve(line.size() + 16);
  out.push_back(1);
  write_substitute(0, line.begin(), line.end(), &out);
  return out;
}

std::vector<uint8_t> encode_line(const std::vector<uint8_t>& line,
                                 const std::vector<uint8_t>& reference) {
  assert(line.size() == reference.size());
  if (all_zeros(line)) {
    return {0xFF};
  }

  std::vector<uint8_t> out;
  out.reserve(line.size() + 16);
  out.push_back(0);

  // First byte of a delta line is the edit count; 0xFF is reserved for
  // an empty line, so at most 254 edits. The remainder is one substitute.
  constexpr uint8_t kMaxEdits = 254;
  int num_edits = 0;

  auto line_it = line.begin();
  auto line_end =
      std::mismatch(line.rbegin(), line.rend(), reference.rbegin()).first.base();
  auto ref_it = reference.begin();

  while (true) {
    const int offset = skip_matching(&line_it, line_end, &ref_it);
    if (line_it == line_end) {
      break;
    }
    if (++num_edits == kMaxEdits) {
      write_substitute(offset, line_it, line_end, &out);
      break;
    }
    const int sub = substitute_length(line_it, line_end, ref_it);
    if (sub > 0) {
      write_substitute(offset, line_it, std::next(line_it, sub), &out);
      line_it += sub;
      ref_it += sub;
    } else {
      const int run = repeat_length(line_it, line_end);
      assert(run >= 2);
      write_repeat(offset, run, *line_it, &out);
      line_it += run;
      ref_it += run;
    }
  }

  out[0] = static_cast<uint8_t>(num_edits);
  return out;
}

}  // namespace sisterhl2030
