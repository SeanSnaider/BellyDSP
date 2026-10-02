"""
Polyphase IIR halfband filters for zero-latency oversampling (BUILD_PLAN "Boost and Overdrive",
Oversampling): design, response, and a check of the design against its specification.

A halfband low-pass split into two parallel allpass branches (the polyphase form; Valenzuela and
Constantinides 1983; Laurent de Soras, HIIR):

    H(z) = ( A0(z^2) + z^-1 A1(z^2) ) / 2

Each branch is a cascade of first-order allpass sections in z^2,
    A(z^2) = prod_k (a_k + z^-2) / (1 + a_k z^-2),
with the coefficients alternating between the branches. Only allpasses, so it's cheap (one multiply
per coefficient per sample), it's IIR (no fixed latency: the delay is the filter's own group delay,
a few samples near DC and growing toward the band edge), and the two branches' outputs sum to the
low half of the band and differ to the high half.

The coefficients come from an elliptic design: for N coefficients (filter order 2N + 1) and a
transition band from fs/4 - tbw to fs/4 + tbw (tbw as a fraction of fs, relative to fs/4 at the
oversampled rate... here `transition` is the normalized transition width, 0 < transition < 0.5),
the elliptic modulus k comes from the transition, the nome q from k, and each coefficient from
Jacobi theta function series (the same formulas as HIIR's PolyphaseIir2Designer).

Usage:
  uv run --with numpy python prototypes/halfband.py          # the design table
  uv run --with numpy python prototypes/halfband.py --golden tests/fixtures/halfband_coefficients.csv
"""

import argparse
import math

import numpy as np


def nome_from_transition (transition):
    """Elliptic modulus and nome for a normalized transition width."""
    k = math.tan ((1.0 - 2.0 * transition) * math.pi / 4.0) ** 2
    kk = math.sqrt (math.sqrt (1.0 - k * k))
    e = 0.5 * (1.0 - kk) / (1.0 + kk)
    e2 = e * e
    e4 = e2 * e2
    q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)))
    return k, q


def coefficient (index, k, q, order):
    """One allpass coefficient from the theta-function series (HIIR's compute_coef)."""
    c = index + 1
    num = 0.0
    i = 0
    while True:
        term = ((-1) ** i) * q ** (i * (i + 1)) * math.sin ((2 * i + 1) * c * math.pi / order)
        num += term
        i += 1
        if abs (term) < 1e-100 or i > 100:
            break
    den = 0.0
    i = 1
    while True:
        term = ((-1) ** i) * q ** (i * i) * math.cos (2 * i * c * math.pi / order)
        den += term
        i += 1
        if abs (term) < 1e-100 or i > 100:
            break
    den = 1.0 + 2.0 * den
    wi = 2.0 * q ** 0.25 * num / den
    ww = wi * wi
    x = math.sqrt ((1.0 - ww * k) * (1.0 - ww / k)) / (1.0 + ww)
    return (1.0 - x) / (1.0 + x)


def design (nbr_coefs, transition):
    k, q = nome_from_transition (transition)
    order = 2 * nbr_coefs + 1
    return [coefficient (i, k, q, order) for i in range (nbr_coefs)]


def response (coefs, freqs):
    """|H| at normalized frequencies (fraction of the oversampled rate), via the polyphase form."""
    z = np.exp (1j * 2 * np.pi * np.asarray (freqs))
    z2 = z * z
    a0 = np.ones_like (z)
    a1 = np.ones_like (z)
    for i, a in enumerate (coefs):
        section = (a + 1 / z2) / (1 + a / z2)
        if i % 2 == 0:
            a0 = a0 * section
        else:
            a1 = a1 * section
    return 0.5 * (a0 + a1 / z)


def attenuation (coefs, transition):
    """Worst stopband level (dB) above fs/4 + transition/2... and passband ripple below fs/4 - transition/2."""
    stop = np.linspace (0.25 + transition / 2, 0.5, 4000)
    passband = np.linspace (0.0, 0.25 - transition / 2, 4000)
    return 20 * np.log10 (np.max (np.abs (response (coefs, stop)))), 20 * np.log10 (np.min (np.abs (response (coefs, passband))))


def table():
    print ("coefs  transition (of the oversampled rate)  stopband   passband dip   group delay at 0 Hz (samples)")
    for n, tw in [(4, 0.1), (6, 0.06), (8, 0.04), (8, 0.02), (10, 0.02), (12, 0.01)]:
        c = design (n, tw)
        stop, pas = attenuation (c, tw)
        # Group delay at DC: numerical derivative of the phase.
        f = np.array ([1e-6, 2e-6])
        ph = np.unwrap (np.angle (response (c, f)))
        gd = -(ph[1] - ph[0]) / (2 * np.pi * (f[1] - f[0]))
        print (f"{n:5d}  {tw:8.3f}                              {stop:8.1f} dB  {pas:10.5f} dB   {gd:7.2f}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument ("--golden")
    args = parser.parse_args()
    if args.golden:
        with open (args.golden, "w") as f:
            f.write ("# Polyphase halfband coefficients (prototypes/halfband.py): nbr_coefs, transition, then the coefficients\n")
            # (8, 0.04), (4, 0.25), and (3, 0.375) are the oversampler's three 2x stages (src/dsp/Oversampler.h):
            # 48<->96 kHz, 96<->192 kHz, 192<->384 kHz. The later stages only have to pass 0-24 kHz, so their
            # transition bands are far wider and a few coefficients reach more than 100 dB.
            for n, tw in [(4, 0.1), (8, 0.04), (12, 0.01), (4, 0.25), (3, 0.375)]:
                f.write (f"{n},{tw}," + ",".join (f"{c:.17g}" for c in design (n, tw)) + "\n")
    else:
        table()
