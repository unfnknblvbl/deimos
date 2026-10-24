/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// Colour maths shared by the console builders. The CIELAB distance is a
// port of tools/colour_lab.py: the same IEEE double operations in the same
// order, so the editor picks exactly the colours the Python build does.
// Licence: GPL-2.0-or-later.
//
// Used by the builders (sms_build, md_build...), the editor's colour
// previews (app.cpp look_sms / look_md, ui_art.cpp) and the console views.
// Two groups of functions:
// - CIELAB: lab() converts an sRGB colour to L*a*b* (D65 white), dist2()
//   is the squared distance between two (CIE76 delta E squared). Builders
//   use it to pick the nearest console colour. lab_err() turns a distance
//   into an integer so comparisons and sums are exact.
// - Console colour formats: the Master System's 64 colours (2 bits per
//   channel) and the Mega Drive's 512 (3 bits per channel).
// CMakeLists.txt compiles this with -ffp-contract=off (no fused
// multiply-add), so the doubles come out the same on every machine.
#pragma once
#include "common.h"

#include <array>
#include <cmath>
#include <cstdint>

// a CIELAB colour: {L*, a*, b*}; L* is 0..100
typedef std::array<double, 3> Lab;

// colour components 0..255 (half-integers allowed)
// (a half-integer is the average of two colours, e.g. a 50/50 dither pair)
Lab lab(double r, double g, double b);
inline Lab lab(RGB c) { return lab(c.r, c.g, c.b); }
// the squared distance between two Lab colours (no square root)
double dist2(const Lab &a, const Lab &b);
// a distance as an integer in 1/64 steps, rounded down
inline int64_t lab_err(double d) { return (int64_t)std::floor(d * 64.0); }

// Master System: 2 bits per channel
int sms_level(int v);                        // 0..255 -> 0..3 (Python round(v / 255 * 3))
RGB sms_rgb(int index);                      // 0..63 (r*16 + g*4 + b) -> 0/85/170/255
u8 sms_cram(RGB c);                          // --BBGGRR
                                             // (the byte written to the console's colour RAM)
RGB sms_snap(RGB c);                         // each channel to the nearest of 0, 85, 170, 255

// Mega Drive: 3 bits per channel, the console's measured DAC levels
int md_level(int v);                         // 0..255 -> 0..7
                                             // (the nearest measured level; ties go to the lower)
u16 md_colour(RGB c);                        // ----BBB-GGG-RRR-
                                             // (the word written to the console's colour RAM)
RGB md_rgb(u16 cram);                        // what the console shows

// Python round() (ties to even) of a double
// (std::nearbyint rounds in the current rounding mode, which is
// round-to-nearest-even unless a program changes it)
inline double py_round(double x) { return std::nearbyint(x); }
