// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// Colour maths (declared in colour.h): sRGB to CIELAB, exactly as
// tools/colour_lab.py computes it, and the Master System's and Mega Drive's
// colour formats. Every operation is written in the same order as the
// Python so the doubles match bit for bit; that is why the powers are done
// with an explicit Newton iteration rather than std::pow / std::cbrt.
#include "colour.h"

#include <cmath>
#include <cstdlib>

namespace {
// linear sRGB to CIE XYZ (D65), row i gives X, Y or Z
const double M[3][3] = {{0.4124, 0.3576, 0.1805}, {0.2126, 0.7152, 0.0722}, {0.0193, 0.1192, 0.9505}};
// the D65 reference white's X, Y, Z
const double WHITE[3] = {0.95047, 1.0, 1.08883};

// a ** (1/n): Newton from 1.0, 40 steps (tools/colour_lab.py _root)
// Solves y^n = a: y <- ((n-1) y + a / y^(n-1)) / n. n >= 2, a > 0.
double root(double a, int n)
{
    double y = 1.0;
    for (int it = 0; it < 40; it++) {
        double p = y;
        for (int k = 0; k < n - 2; k++) p = p * y;   // p = y^(n-1)
        y = ((double)(n - 1) * y + a / p) / (double)n;
    }
    return y;
}

// The sRGB transfer curve undone: a channel 0..1 to linear light 0..1.
// Above 0.04045 it is x^2.4, computed as x^2 * (x^2)^(1/5).
double linear(double c)
{
    if (!(c > 0.04045)) return c / 12.92;   // the straight part near black
    double x = (c + 0.055) / 1.055;
    double x2 = x * x;
    return x2 * root(x2, 5);
}
}  // namespace

// sRGB (0..255 per channel) to CIELAB.
Lab lab(double r8, double g8, double b8)
{
    double r = linear(r8 / 255.0), g = linear(g8 / 255.0), b = linear(b8 / 255.0);
    double f[3];   // f(X/Xn), f(Y/Yn), f(Z/Zn)
    for (int i = 0; i < 3; i++) {
        double x = ((r * M[i][0] + g * M[i][1]) + b * M[i][2]) / WHITE[i];
        // the cube root, or the straight line CIELAB uses near zero
        // (0.008856 = (6/29)^3; 7.787 = (29/6)^2 / 3)
        f[i] = x > 0.008856 ? root(x, 3) : x * 7.787 + 16.0 / 116.0;
    }
    return {f[1] * 116.0 - 16.0, (f[0] - f[1]) * 500.0, (f[1] - f[2]) * 200.0};   // L*, a*, b*
}

double dist2(const Lab &a, const Lab &b)
{
    double d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return (d0 * d0 + d1 * d1) + d2 * d2;   // (bracketed as the Python adds them)
}

int sms_level(int v) { return (int)py_round(v / 255.0 * 3.0); }

// a colour number 0..63 to RGB: red in bits 5-4, green 3-2, blue 1-0 of
// the number; each level 0..3 times 85
// (note: this numbering differs from the CRAM byte's, which has blue on top)
RGB sms_rgb(int i) { return {(u8)((i / 16) * 85), (u8)(((i / 4) % 4) * 85), (u8)((i % 4) * 85)}; }

// the Master System CRAM byte: blue in bits 5-4, green 3-2, red 1-0
u8 sms_cram(RGB c) { return (u8)((sms_level(c.b) << 4) | (sms_level(c.g) << 2) | sms_level(c.r)); }

RGB sms_snap(RGB c) { return {(u8)(sms_level(c.r) * 85), (u8)(sms_level(c.g) * 85), (u8)(sms_level(c.b) * 85)}; }

// the brightness (0..255) the Mega Drive outputs for each 3-bit level 0..7
// (measured, so not evenly spaced)
static const int MD_LEVELS[8] = {0, 52, 87, 116, 144, 172, 206, 255};

int md_level(int v)
{
    int best = 0;
    // strictly closer only, so on a tie the lower level wins
    for (int i = 1; i < 8; i++)
        if (std::abs(MD_LEVELS[i] - v) < std::abs(MD_LEVELS[best] - v)) best = i;
    return best;
}

// the Mega Drive CRAM word: blue in bits 11-9, green 7-5, red 3-1
u16 md_colour(RGB c) { return (u16)((md_level(c.b) << 9) | (md_level(c.g) << 5) | (md_level(c.r) << 1)); }

// a CRAM word back to the colour the console shows
RGB md_rgb(u16 v) { return {(u8)MD_LEVELS[(v >> 1) & 7], (u8)MD_LEVELS[(v >> 5) & 7], (u8)MD_LEVELS[(v >> 9) & 7]}; }
