#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
CIELAB colour distances that come out bit-for-bit the same in Python and in
the editor's C++ (editor/src/colour.cpp), on any machine.

numpy's float32 to_lab (tools/gen_views.py) gives slightly different bits
depending on the array length, the CPU and the maths library, so a tool
written in another language can't reproduce the colour choices exactly.
Here every step is an IEEE double add, subtract, multiply or divide done
element by element in a fixed order (the powers and roots are fixed
Newton iterations), and the errors that get summed are integers.

Pipeline: lab() turns 0..255 sRGB into CIELAB (D65 white), dist2() gives
the squared Lab distance and err() scales it to an integer error. The colour
pickers in the view builders sum these errors to choose palette entries.
Run as a script, it prints test vectors (input colour and the exact hex bits
of L, a, b) for the editor's self-test to compare against.

Licence: GPL-2.0-or-later.
"""
import numpy as np

# linear sRGB -> CIE XYZ matrix (rows X, Y, Z) and the D65 reference white
M = ((0.4124, 0.3576, 0.1805), (0.2126, 0.7152, 0.0722), (0.0193, 0.1192, 0.9505))
WHITE = (0.95047, 1.0, 1.08883)
ERR_SCALE = 64.0           # squared Lab distance -> integer error: floor(d * 64)


def _root(a, n, iters):
    """a ** (1/n) for a in (0, 1.2], Newton from 1.0 with a fixed number of steps.

    a is a float64 array, n the root (an integer >= 2), iters the number of
    Newton steps y = ((n-1) y + a / y^(n-1)) / n. Returns an array like a.
    """
    y = np.ones_like(a)
    for _ in range(iters):
        p = y
        for _ in range(n - 2):
            p = p * y                       # y ** (n - 1)
        y = ((n - 1) * y + a / p) / n
    return y


def _linear(c):
    """sRGB 0..1 -> linear: ((c + 0.055) / 1.055) ** 2.4, x ** 2.4 = x * x * fifth root of (x * x).

    c is an array of channel values 0..1. Values at or below 0.04045 use the
    linear segment c / 12.92. The inner np.where feeds 1.0 to the root for
    those, so the Newton iteration never sees out-of-range input.
    """
    x = (c + 0.055) / 1.055
    x2 = x * x
    return np.where(c > 0.04045, x2 * _root(np.where(c > 0.04045, x2, 1.0), 5, 40), c / 12.92)


def lab(rgb):
    """[..., 3] colours 0..255 (may be half-integers) -> [..., 3] float64 L, a, b.

    Steps: scale to 0..1, linearise, multiply by M and divide by the white
    point, apply the CIELAB f() (cube root above 0.008856, else the linear
    7.787 x + 16/116 part), then L = 116 fY - 16, a = 500 (fX - fY),
    b = 200 (fY - fZ).
    """
    c = np.asarray(rgb, np.float64) / 255.0
    lin = _linear(c)
    r, g, b = lin[..., 0], lin[..., 1], lin[..., 2]
    f = []
    for i in range(3):
        x = ((r * M[i][0] + g * M[i][1]) + b * M[i][2]) / WHITE[i]
        big = x > 0.008856              # (6/29)^3: where f() switches to the cube root
        f.append(np.where(big, _root(np.where(big, x, 1.0), 3, 40), x * 7.787 + 16.0 / 116.0))
    return np.stack([f[1] * 116.0 - 16.0, (f[0] - f[1]) * 500.0, (f[1] - f[2]) * 200.0], -1)


def dist2(a, b):
    """Squared distance of broadcast Lab arrays a and b ([..., 3]), summed in a fixed order.

    Returns an array of the broadcast shape without the last axis.
    """
    d0 = a[..., 0] - b[..., 0]
    d1 = a[..., 1] - b[..., 1]
    d2 = a[..., 2] - b[..., 2]
    return (d0 * d0 + d1 * d1) + d2 * d2


def err(d):
    """Squared Lab distance d -> int64 error floor(d * ERR_SCALE)."""
    return np.floor(d * ERR_SCALE).astype(np.int64)


if __name__ == "__main__":
    # test vectors for the editor's self-test
    import sys
    v = np.array([[0, 0, 0], [255, 255, 255], [66, 0, 0], [42.5, 127.5, 212.5], [8, 4, 252]], np.float64)
    for row, l in zip(v, lab(v)):
        print(row.tolist(), [float.hex(float(x)) for x in l])
    sys.exit(0)
