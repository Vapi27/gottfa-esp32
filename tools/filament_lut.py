#!/usr/bin/env python3
"""Generate the incandescent filament lookup table used by arenaled.cpp.

    python3 tools/filament_lut.py        # prints the C table, paste into arenaled.cpp

A bulb switching on does not only get brighter, it changes colour: the filament
climbs from dull red through amber to warm white as it heats. Fading an LED's
brightness holds its hue at every level, and that is what reads as digital
rather than as a playfield.

Two knobs, and the second one is the interesting one:

  T_HOT    filament temperature at full drive. 2700 K is a #47. Lower reads as an
           older, tireder machine; higher as a modern bulb.

  W_TOP    how much of the output the WHITE die carries at full drive. This is
           deliberately not the colorimetric answer. Splitting so the white die
           only takes the neutral part of the spectrum is what a meter would do,
           and on the bench it looked orange — a 2700 K source measures orange,
           but the eye adapts to the dominant illuminant and reads a real bulb as
           warm white. Pushing the white die to ~85 % at full keeps the physical
           trajectory and lands where the eye expects it. Drop it towards 0.5 for
           a deliberately amber, vintage look.
"""
import math

N       = 33      # table entries
T_COLD  = 800.0   # K — visibly dull red, the bottom of the useful range
T_HOT   = 2700.0  # K — #47 pinball bulb at full drive
W_TOP   = 0.85    # share carried by the white die at full (see above)
W_KNEE  = 0.35    # below this normalised temperature the white die stays off


def planck(nm, T):
    l = nm * 1e-9
    return (3.7418e-16 / l ** 5) / (math.exp(1.4388e-2 / (l * T)) - 1)


def _g(x, m, s1, s2):
    s = s1 if x < m else s2
    return math.exp(-0.5 * ((x - m) / s) ** 2)


def cie(nm):
    """CIE 1931 colour matching functions, multi-lobe Gaussian fit (Wyman et al.)."""
    return (1.056 * _g(nm, 599.8, 37.9, 31.0) + 0.362 * _g(nm, 442.0, 16.0, 26.7)
            - 0.065 * _g(nm, 501.1, 20.4, 26.2),
            0.821 * _g(nm, 568.8, 46.9, 40.5) + 0.286 * _g(nm, 530.9, 16.3, 31.1),
            1.217 * _g(nm, 437.0, 11.8, 36.0) + 0.681 * _g(nm, 459.0, 26.0, 13.8))


def rgb_of(T):
    X = Y = Z = 0.0
    for nm in range(380, 781, 5):
        p = planck(nm, T)
        cx, cy, cz = cie(nm)
        X += p * cx
        Y += p * cy
        Z += p * cz
    X /= Y
    Z /= Y
    Y = 1.0
    r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z
    g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z
    b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z
    m = max(r, g, b, 1e-9)
    return [max(0.0, min(1.0, c / m)) for c in (r, g, b)]


rows = []
for i in range(N):
    t = i / (N - 1)
    T = T_COLD + t * (T_HOT - T_COLD)
    r, g, _ = rgb_of(T)
    lum = (T / T_HOT) ** 4                       # Stefan-Boltzmann
    wf = 0.0 if t <= W_KNEE else W_TOP * ((t - W_KNEE) / (1 - W_KNEE)) ** 0.8
    rows.append([lum * r * (1 - wf), lum * g * (1 - wf), 0.0, lum * wf])

peak = max(max(row) for row in rows)             # full drive saturates one channel
rows = [[min(255, round(c / peak * 255)) for c in row] for row in rows]

print("static const uint8_t FILAMENT[%d][4] = {   // R,G,B,W, filament %d K -> %d K"
      % (N, T_COLD, T_HOT))
for i in range(0, N, 4):
    print("  " + " ".join("{%3d,%3d,%3d,%3d}," % tuple(rows[j])
                          for j in range(i, min(i + 4, N))))
print("};")
