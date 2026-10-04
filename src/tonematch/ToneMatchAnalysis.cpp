// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "ToneMatchAnalysis.h"

#include "../dsp/AmpTone.h"
#include "../dsp/Svf.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ampsim::tonematch
{

namespace
{
constexpr double tiny = 1.0e-20;
constexpr double pi = juce::MathConstants<double>::pi;

double db (double p) { return 10.0 * std::log10 (std::max (p, tiny)); }

double binFrequency (int k) { return k * sampleRate / fftSize; }

/// The STFT's framing: frames of fftSize every hop, the end zero-padded to a whole frame
/// (prototypes/tone_match.py, frames_of).
int numFramesFor (int numSamples)
{
    const auto n = std::max (numSamples, fftSize);
    return 1 + (int) std::ceil ((double) (n - fftSize) / hop);
}

void frameAt (const float* x, int numSamples, int t, float* out)
{
    const auto start = t * hop;
    for (int i = 0; i < fftSize; ++i)
        out[i] = start + i < numSamples ? x[start + i] : 0.0f;
}

const std::array<double, fftSize>& hannWindow()
{
    // Periodic Hann, 0.5 - 0.5 cos(2 pi n / N): the STFT convention (its shifted copies at hop N/4 sum to a constant).
    static const auto w = [] {
        std::array<double, fftSize> v {};
        for (int n = 0; n < fftSize; ++n)
            v[(size_t) n] = 0.5 - 0.5 * std::cos (2.0 * pi * n / fftSize);
        return v;
    }();
    return w;
}

std::vector<int> bandsBetween (double lo, double hi)
{
    std::vector<int> out;
    for (int b = 0; b < numBands; ++b)
        if (bands().centre[(size_t) b] >= lo && bands().centre[(size_t) b] < hi)
            out.push_back (b);
    return out;
}

double mean (const std::vector<double>& v)
{
    return v.empty() ? 0.0 : std::accumulate (v.begin(), v.end(), 0.0) / (double) v.size();
}

double populationStd (const std::vector<double>& v)
{
    const auto m = mean (v);
    double s = 0.0;
    for (auto x : v)
        s += (x - m) * (x - m);
    return v.empty() ? 0.0 : std::sqrt (s / (double) v.size());
}

// Chroma: each bin from 60 Hz to 2.1 kHz belongs to its nearest pitch class.
constexpr double chromaLo = 60.0, chromaHi = 2100.0;
constexpr double chromaSilence = 1.0e-5; // -100 dB re. the loudest chroma value

int pitchClassOfBin (int k)
{
    // (round(12 log2(f / 440)) + 69) mod 12, as numpy rounds (half to even; never exactly half here).
    const auto semis = std::nearbyint (12.0 * std::log2 (binFrequency (k) / 440.0));
    return (((int) semis + 69) % 12 + 12) % 12;
}
} // namespace

// ---- Bands and weights ------------------------------------------------------------------------------

double perceptualWeight (double f)
{
    const auto f2 = f * f;
    const auto ra = (12194.0 * 12194.0 * f2 * f2)
                    / ((f2 + 20.6 * 20.6) * std::sqrt ((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0));
    const auto aDb = 20.0 * std::log10 (ra) + 2.0;
    return std::max (0.25, std::pow (10.0, aDb / 20.0));
}

const Bands& bands()
{
    static const Bands b = [] {
        Bands v;
        const auto half = coarseStep / 2.0;
        for (int i = 0; i < numBands; ++i)
        {
            const auto c = coarseLo * std::pow (2.0, i * coarseStep);
            const auto lo = c * std::pow (2.0, -half), hi = c * std::pow (2.0, half);
            int first = -1, last = -1;
            for (int k = 0; k < numBins; ++k)
            {
                const auto f = binFrequency (k);
                if (f >= lo && f < hi)
                {
                    if (first < 0)
                        first = k;
                    last = k + 1;
                }
            }
            if (first < 0)
            {
                first = (int) std::nearbyint (c * fftSize / sampleRate);
                last = first + 1;
            }
            v.centre[(size_t) i] = c;
            v.first[(size_t) i] = first;
            v.last[(size_t) i] = last;
            v.weight[(size_t) i] = perceptualWeight (c);
        }
        return v;
    }();
    return b;
}

std::vector<double> smoothBands (const std::vector<double>& r)
{
    auto out = r;
    for (size_t i = 1; i + 1 < r.size(); ++i)
        out[i] = 0.25 * r[i - 1] + 0.5 * r[i] + 0.25 * r[i + 1];
    return out;
}

std::vector<double> bandWeights (const std::vector<double>& ltas)
{
    const auto peak = *std::max_element (ltas.begin(), ltas.end());
    std::vector<double> w (ltas.size());
    for (size_t b = 0; b < ltas.size(); ++b)
    {
        const auto below = peak - ltas[b];
        const auto confidence = std::clamp ((ignoredDb - below) / (ignoredDb - confidentDb), minConfidence, 1.0);
        w[b] = bands().weight[b] * confidence;
    }
    return w;
}

double weightedMean (const std::vector<double>& v, const std::vector<double>& w)
{
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        num += v[i] * w[i];
        den += w[i];
    }
    return num / den;
}

double weightedRmsCentred (const std::vector<double>& r, const std::vector<double>& w)
{
    const auto m = weightedMean (r, w);
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < r.size(); ++i)
    {
        num += w[i] * (r[i] - m) * (r[i] - m);
        den += w[i];
    }
    return std::sqrt (num / den);
}

double percentile (std::vector<double> v, double p)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const auto pos = (double) (v.size() - 1) * p / 100.0;
    const auto lo = (size_t) std::floor (pos);
    const auto hi = std::min (lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (pos - (double) lo);
}

// ---- Analysis -------------------------------------------------------------------------------------------

Analysis Analysis::of (const float* x, int numSamples)
{
    Analysis a;
    const auto& bd = bands();
    const auto& window = hannWindow();
    const auto T = numFramesFor (numSamples);
    a.numFrames = T;

    // Pass 1: each frame's band powers and chroma, and its windowed power spectrum kept only as a running
    // sum later (pass 2), so a minute of audio never holds the whole spectrogram.
    juce::dsp::FFT fft (fftOrder);
    std::vector<float> frame ((size_t) fftSize), work ((size_t) fftSize * 2);
    std::vector<double> power ((size_t) numBins);
    std::vector<double> bandPower ((size_t) T * numBands);
    a.chroma.assign ((size_t) T * 12, 0.0);

    int chromaFirst = numBins, chromaLast = 0;
    for (int k = 0; k < numBins; ++k)
        if (binFrequency (k) >= chromaLo && binFrequency (k) < chromaHi)
        {
            chromaFirst = std::min (chromaFirst, k);
            chromaLast = k + 1;
        }
    std::vector<int> pitchClass ((size_t) numBins, 0);
    for (int k = chromaFirst; k < chromaLast; ++k)
        pitchClass[(size_t) k] = pitchClassOfBin (k);

    auto spectrum = [&] (int t) {
        frameAt (x, numSamples, t, frame.data());
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < fftSize; ++i)
            work[(size_t) i] = (float) (frame[(size_t) i] * window[(size_t) i]);
        fft.performRealOnlyForwardTransform (work.data(), true); // interleaved re, im for bins 0 .. N/2
        for (int k = 0; k < numBins; ++k)
        {
            const double re = work[(size_t) (2 * k)], im = work[(size_t) (2 * k + 1)];
            power[(size_t) k] = re * re + im * im;
        }
    };

    for (int t = 0; t < T; ++t)
    {
        spectrum (t);
        for (int b = 0; b < numBands; ++b)
        {
            double s = 0.0;
            for (int k = bd.first[(size_t) b]; k < bd.last[(size_t) b]; ++k)
                s += power[(size_t) k];
            bandPower[(size_t) t * numBands + (size_t) b] = s / (bd.last[(size_t) b] - bd.first[(size_t) b]);
        }
        auto* c = a.chroma.data() + (size_t) t * 12;
        for (int k = chromaFirst; k < chromaLast; ++k)
            c[pitchClass[(size_t) k]] += std::sqrt (power[(size_t) k]);
    }

    // Chroma (Fujishima 1999; Mueller, "Fundamentals of Music Processing", ch. 3): scaled by the loudest
    // value anywhere, log-compressed (log(1 + 100 c)), unit length per frame; a silent frame is all-equal.
    // A frame whose loudest class is 100 dB below the loudest anywhere is silence: its chroma would be
    // rounding noise (different in float and double FFTs), so it gets the all-equal vector too.
    const auto chromaMax = *std::max_element (a.chroma.begin(), a.chroma.end());
    for (int t = 0; t < T; ++t)
    {
        auto* c = a.chroma.data() + (size_t) t * 12;
        double norm = 0.0, loudest = 0.0;
        for (int pc = 0; pc < 12; ++pc)
        {
            const auto v = c[pc] / (chromaMax + tiny);
            loudest = std::max (loudest, v);
            c[pc] = std::log1p (100.0 * v);
            norm += c[pc] * c[pc];
        }
        norm = std::sqrt (norm);
        const auto silent = loudest < chromaSilence || norm <= 1.0e-9;
        for (int pc = 0; pc < 12; ++pc)
            c[pc] = silent ? 1.0 / std::sqrt (12.0) : c[pc] / norm;
    }

    a.frameDb.resize (bandPower.size());
    for (size_t i = 0; i < bandPower.size(); ++i)
        a.frameDb[i] = db (bandPower[i]);

    // Which frames hold playing: each band relative to its own median over the signal (so no filter
    // changes it), the power mean of that, and within 30 dB of its 95th percentile.
    std::vector<double> medians ((size_t) numBands);
    for (int b = 0; b < numBands; ++b)
    {
        std::vector<double> column ((size_t) T);
        for (int t = 0; t < T; ++t)
            column[(size_t) t] = a.frameDb[(size_t) t * numBands + (size_t) b];
        medians[(size_t) b] = percentile (column, 50.0);
    }
    std::vector<double> activityLevel ((size_t) T);
    for (int t = 0; t < T; ++t)
    {
        double s = 0.0;
        for (int b = 0; b < numBands; ++b)
            s += std::pow (10.0, (a.frameDb[(size_t) t * numBands + (size_t) b] - medians[(size_t) b]) / 10.0);
        activityLevel[(size_t) t] = 10.0 * std::log10 (s / numBands);
    }
    const auto threshold = percentile (activityLevel, 95.0) - activeRangeDb;
    a.active.resize ((size_t) T);
    int numActive = 0;
    for (int t = 0; t < T; ++t)
    {
        a.active[(size_t) t] = activityLevel[(size_t) t] > threshold ? 1 : 0;
        numActive += a.active[(size_t) t];
    }
    if (numActive < 4)
        return a;

    // Pass 2: the long-term spectrum per bin, over the playing frames.
    a.ltasBins.assign ((size_t) numBins, 0.0);
    for (int t = 0; t < T; ++t)
    {
        if (! a.active[(size_t) t])
            continue;
        spectrum (t);
        for (int k = 0; k < numBins; ++k)
            a.ltasBins[(size_t) k] += power[(size_t) k];
    }
    for (auto& v : a.ltasBins)
        v /= numActive;

    a.ltas.assign ((size_t) numBands, 0.0);
    for (int b = 0; b < numBands; ++b)
    {
        double s = 0.0;
        for (int t = 0; t < T; ++t)
            if (a.active[(size_t) t])
                s += bandPower[(size_t) t * numBands + (size_t) b];
        a.ltas[(size_t) b] = db (s / numActive);
    }
    a.weights = bandWeights (smoothBands (a.ltas));

    // The features (prototypes/tone_match.py, Analysis): on frames whitened by the long-term spectrum.
    a.level.resize ((size_t) T);
    std::vector<double> tilt;
    const auto low = bandsBetween (150.0, 800.0), high = bandsBetween (2000.0, 8000.0);
    for (int t = 0; t < T; ++t)
    {
        const auto* f = a.frame (t);
        double s = 0.0;
        for (int b = 0; b < numBands; ++b)
            s += std::pow (10.0, (f[b] - a.ltas[(size_t) b]) / 10.0);
        a.level[(size_t) t] = 10.0 * std::log10 (s / numBands);

        if (a.active[(size_t) t])
        {
            double h = 0.0, l = 0.0;
            for (auto b : high)
                h += f[b] - a.ltas[(size_t) b];
            for (auto b : low)
                l += f[b] - a.ltas[(size_t) b];
            tilt.push_back (h / (double) high.size() - l / (double) low.size());
        }
    }

    std::vector<double> activeLevel, flux, crest;
    for (int t = 0; t < T; ++t)
    {
        if (! a.active[(size_t) t])
            continue;
        activeLevel.push_back (a.level[(size_t) t]);
        if (t > 0 && a.active[(size_t) (t - 1)])
            flux.push_back (std::abs (a.level[(size_t) t] - a.level[(size_t) (t - 1)]));

        frameAt (x, numSamples, t, frame.data());
        double sumSquares = 0.0, peak = 0.0;
        for (auto v : frame)
        {
            sumSquares += (double) v * v;
            peak = std::max (peak, std::abs ((double) v));
        }
        const auto rms = std::sqrt (sumSquares / fftSize);
        crest.push_back (20.0 * std::log10 (peak / std::max (rms, 1.0e-12)));
    }

    a.features[0] = percentile (activeLevel, 90.0) - percentile (activeLevel, 10.0);
    a.features[1] = flux.empty() ? 0.0 : percentile (flux, 50.0);
    a.features[2] = populationStd (tilt);
    a.features[3] = percentile (crest, 50.0);
    a.ok = true;
    return a;
}

double nonlinearDistance (const std::array<double, numFeatures>& target, const std::array<double, numFeatures>& candidate)
{
    double d = 0.0;
    for (size_t i = 0; i < (size_t) numFeatures; ++i)
        d += std::abs (target[i] - candidate[i]) / featureScales[i];
    return d;
}

// ---- Same part: DTW ------------------------------------------------------------------------------------

Alignment align (const Analysis& target, const Analysis& candidate)
{
    const auto n = target.numFrames, m = candidate.numFrames;
    std::vector<double> cost ((size_t) n * (size_t) m), d ((size_t) n * (size_t) m);

    for (int i = 0; i < n; ++i)
        for (int j = 0; j < m; ++j)
        {
            double dot = 0.0;
            for (int pc = 0; pc < 12; ++pc)
                dot += target.chroma[(size_t) i * 12 + (size_t) pc] * candidate.chroma[(size_t) j * 12 + (size_t) pc];
            cost[(size_t) i * (size_t) m + (size_t) j] = 1.0 - dot; // cosine distance (both unit length)
        }

    // D[i, j] = cost[i, j] + min(D[i-1, j-1], D[i-1, j], D[i, j-1]), computed per row as the prototype
    // does: with A[j] = cost[i, j] + min(D[i-1, j-1], D[i-1, j]) and C the row's running sum of cost,
    // D[i, j] = C[j] + min over k <= j of (A[k] - C[k]).
    auto at = [m] (int i, int j) { return (size_t) i * (size_t) m + (size_t) j; };
    double run = 0.0;
    for (int j = 0; j < m; ++j)
        d[at (0, j)] = (run += cost[at (0, j)]);

    for (int i = 1; i < n; ++i)
    {
        double c = 0.0, best = std::numeric_limits<double>::infinity();
        for (int j = 0; j < m; ++j)
        {
            const auto prev = j > 0 ? std::min (d[at (i - 1, j)], d[at (i - 1, j - 1)]) : d[at (i - 1, j)];
            const auto a = cost[at (i, j)] + prev;
            c += cost[at (i, j)];
            best = std::min (best, a - c);
            d[at (i, j)] = c + best;
        }
    }

    Alignment out;
    int i = n - 1, j = m - 1;
    out.path.push_back ({ i, j });
    while (i > 0 || j > 0)
    {
        if (i == 0)
            --j;
        else if (j == 0)
            --i;
        else
        {
            const auto diag = d[at (i - 1, j - 1)], up = d[at (i - 1, j)], left = d[at (i, j - 1)];
            if (diag <= up && diag <= left)
            {
                --i;
                --j;
            }
            else if (up <= left)
                --i;
            else
                --j;
        }
        out.path.push_back ({ i, j });
    }
    std::reverse (out.path.begin(), out.path.end());
    out.meanCost = d[at (n - 1, m - 1)] / (double) out.path.size();
    return out;
}

AlignedDifference alignedDifference (const Analysis& target, const Analysis& candidate, const Alignment& alignment)
{
    AlignedDifference out;
    std::vector<double> sum ((size_t) numBands, 0.0), envelope;
    for (const auto& [i, j] : alignment.path)
    {
        if (! target.active[(size_t) i] || ! candidate.active[(size_t) j])
            continue;
        const auto* ft = target.frame (i);
        const auto* fc = candidate.frame (j);
        for (int b = 0; b < numBands; ++b)
            sum[(size_t) b] += ft[b] - fc[b];
        envelope.push_back (target.level[(size_t) i] - candidate.level[(size_t) j]);
    }
    out.pairs = (int) envelope.size();
    for (auto& v : sum)
        v /= std::max (1, out.pairs);
    out.residual = smoothBands (sum);
    out.envelope = populationStd (envelope);
    return out;
}

// ---- Linear blocks ---------------------------------------------------------------------------------------

std::array<double, numBands> toneDb (const std::array<double, 5>& gains)
{
    std::array<double, numBands> out {};
    for (size_t band = 0; band < AmpTone::numBands; ++band)
    {
        if (gains[band] == 0.0)
            continue;
        const auto& spec = AmpTone::bands[band];
        const auto c = Svf::design (spec.type, spec.frequency, spec.q, gains[band], sampleRate);
        for (int b = 0; b < numBands; ++b)
            out[(size_t) b] += 20.0 * std::log10 (std::abs (Svf::responseAt (c, bands().centre[(size_t) b], sampleRate)));
    }
    return out;
}

namespace
{
Svf::Type svfTypeOf (Equalizer::BandType t)
{
    switch (t)
    {
        case Equalizer::BandType::lowShelf:  return Svf::Type::lowShelf;
        case Equalizer::BandType::highShelf: return Svf::Type::highShelf;
        case Equalizer::BandType::notch:     return Svf::Type::notch;
        case Equalizer::BandType::peak:      break;
    }
    return Svf::Type::peak;
}

// The EQ bands as doubles while fitting (Equalizer::Band holds floats).
struct EqBand
{
    Equalizer::BandType type;
    double frequency, gainDb, q;
};

std::array<double, numBands> eqDbOf (const std::array<EqBand, Equalizer::numParametricBands>& eq)
{
    std::array<double, numBands> out {};
    for (const auto& band : eq)
    {
        if (band.gainDb == 0.0)
            continue;
        const auto c = Svf::design (svfTypeOf (band.type), band.frequency, band.q, band.gainDb, sampleRate);
        for (int b = 0; b < numBands; ++b)
            out[(size_t) b] += 20.0 * std::log10 (std::abs (Svf::responseAt (c, bands().centre[(size_t) b], sampleRate)));
    }
    return out;
}
} // namespace

std::array<double, numBands> eqDb (const std::array<Equalizer::Band, Equalizer::numParametricBands>& eq)
{
    std::array<EqBand, Equalizer::numParametricBands> d {};
    for (size_t i = 0; i < d.size(); ++i)
        d[i] = { eq[i].type, (double) eq[i].frequency, (double) eq[i].gainDb, (double) eq[i].q };
    return eqDbOf (d);
}

// ---- Nelder-Mead ---------------------------------------------------------------------------------------

std::pair<std::vector<double>, double> nelderMead (const std::function<double (const std::vector<double>&)>& fn,
                                                   std::vector<double> x0, const std::vector<double>& step,
                                                   const std::vector<double>& lo, const std::vector<double>& hi,
                                                   int iterations, double tolerance)
{
    const auto n = x0.size();
    auto clamp = [&] (std::vector<double> v) {
        for (size_t i = 0; i < n; ++i)
            v[i] = std::min (std::max (v[i], lo[i]), hi[i]);
        return v;
    };
    auto combine = [n] (const std::vector<double>& a, double s, const std::vector<double>& b, const std::vector<double>& c) {
        // a + s (b - c)
        std::vector<double> out (n);
        for (size_t i = 0; i < n; ++i)
            out[i] = a[i] + s * (b[i] - c[i]);
        return out;
    };

    std::vector<std::vector<double>> pts { clamp (x0) };
    for (size_t i = 0; i < n; ++i)
    {
        auto p = pts[0];
        p[i] = p[i] + step[i] <= hi[i] ? p[i] + step[i] : p[i] - step[i];
        pts.push_back (clamp (p));
    }
    std::vector<double> vals;
    for (const auto& p : pts)
        vals.push_back (fn (p));

    for (int it = 0; it < iterations; ++it)
    {
        std::vector<size_t> order (n + 1);
        std::iota (order.begin(), order.end(), 0);
        std::stable_sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return vals[a] < vals[b]; });
        std::vector<std::vector<double>> sp;
        std::vector<double> sv;
        for (auto o : order)
        {
            sp.push_back (pts[o]);
            sv.push_back (vals[o]);
        }
        pts = std::move (sp);
        vals = std::move (sv);

        if (std::abs (vals[n] - vals[0]) <= tolerance * (std::abs (vals[0]) + 1.0e-9))
            break;

        std::vector<double> centroid (n, 0.0);
        for (size_t p = 0; p < n; ++p)
            for (size_t i = 0; i < n; ++i)
                centroid[i] += pts[p][i];
        for (auto& v : centroid)
            v /= (double) n;

        const auto xr = clamp (combine (centroid, 1.0, centroid, pts[n]));
        const auto fr = fn (xr);
        if (fr < vals[0])
        {
            const auto xe = clamp (combine (centroid, 2.0, centroid, pts[n]));
            const auto fe = fn (xe);
            if (fe < fr)
            {
                pts[n] = xe;
                vals[n] = fe;
            }
            else
            {
                pts[n] = xr;
                vals[n] = fr;
            }
        }
        else if (fr < vals[n - 1])
        {
            pts[n] = xr;
            vals[n] = fr;
        }
        else
        {
            const auto xc = fr < vals[n] ? clamp (combine (centroid, 0.5, xr, centroid))
                                         : clamp (combine (centroid, 0.5, pts[n], centroid));
            const auto fc = fn (xc);
            if (fc < std::min (fr, vals[n]))
            {
                pts[n] = xc;
                vals[n] = fc;
            }
            else
            {
                for (size_t i = 1; i <= n; ++i)
                {
                    pts[i] = clamp (combine (pts[0], 0.5, pts[i], pts[0]));
                    vals[i] = fn (pts[i]);
                }
            }
        }
    }

    const auto best = (size_t) (std::min_element (vals.begin(), vals.end()) - vals.begin());
    return { pts[best], vals[best] };
}

// ---- Tone fit ------------------------------------------------------------------------------------------

double toneError (const std::vector<double>& r, const std::array<double, 5>& theta, const std::vector<double>& w)
{
    const auto t = toneDb (theta);
    std::vector<double> e (r.size());
    for (size_t b = 0; b < r.size(); ++b)
        e[b] = r[b] - t[b];
    const auto m = weightedMean (e, w);
    double num = 0.0, den = 0.0, ridge = 0.0;
    for (size_t b = 0; b < e.size(); ++b)
    {
        num += w[b] * (e[b] - m) * (e[b] - m);
        den += w[b];
    }
    for (auto v : theta)
        ridge += v * v;
    return std::sqrt (num / den + toneRidge * ridge);
}

namespace
{
/// Each tone band's dB response per dB of gain (at +6 dB, divided by 6): the linearized model.
const std::array<std::array<double, numBands>, 5>& toneBasis()
{
    static const auto basis = [] {
        std::array<std::array<double, numBands>, 5> v {};
        for (size_t i = 0; i < 5; ++i)
        {
            std::array<double, 5> g {};
            g[i] = 6.0;
            const auto r = toneDb (g);
            for (int b = 0; b < numBands; ++b)
                v[i][(size_t) b] = r[(size_t) b] / 6.0;
        }
        return v;
    }();
    return basis;
}

/// Solves the small dense system A x = y by Gaussian elimination with partial pivoting.
template <size_t N>
std::array<double, N> solve (std::array<std::array<double, N>, N> a, std::array<double, N> y)
{
    for (size_t c = 0; c < N; ++c)
    {
        size_t pivot = c;
        for (size_t r = c + 1; r < N; ++r)
            if (std::abs (a[r][c]) > std::abs (a[pivot][c]))
                pivot = r;
        std::swap (a[c], a[pivot]);
        std::swap (y[c], y[pivot]);
        for (size_t r = c + 1; r < N; ++r)
        {
            const auto f = a[r][c] / a[c][c];
            for (size_t k = c; k < N; ++k)
                a[r][k] -= f * a[c][k];
            y[r] -= f * y[c];
        }
    }
    std::array<double, N> x {};
    for (size_t c = N; c-- > 0;)
    {
        auto s = y[c];
        for (size_t k = c + 1; k < N; ++k)
            s -= a[c][k] * x[k];
        x[c] = s / a[c][c];
    }
    return x;
}
} // namespace

std::pair<std::array<double, 5>, double> fitTone (const std::vector<double>& r, const std::vector<double>& w, bool polish)
{
    // Ridge least squares: minimize sum_b (w_b / sum w) (r_b - c - (B theta)_b)^2 + toneRidge |theta|^2 over
    // the five knobs and a free level offset c, through the 6 x 6 normal equations.
    const auto& basis = toneBasis();
    const auto wsum = std::accumulate (w.begin(), w.end(), 0.0);
    std::array<std::array<double, 6>, 6> ata {};
    std::array<double, 6> aty {};
    for (int b = 0; b < numBands; ++b)
    {
        std::array<double, 6> row {};
        for (size_t i = 0; i < 5; ++i)
            row[i] = basis[i][(size_t) b];
        row[5] = 1.0;
        const auto wb = w[(size_t) b] / wsum;
        for (size_t i = 0; i < 6; ++i)
        {
            aty[i] += wb * row[i] * r[(size_t) b];
            for (size_t j = 0; j < 6; ++j)
                ata[i][j] += wb * row[i] * row[j];
        }
    }
    for (size_t i = 0; i < 5; ++i)
        ata[i][i] += toneRidge;
    const auto solution = solve (ata, aty);

    std::array<double, 5> theta {};
    for (size_t i = 0; i < 5; ++i)
        theta[i] = std::clamp (solution[i], -toneRangeDb, toneRangeDb);

    if (! polish)
        return { theta, toneError (r, theta, w) };

    const std::vector<double> lo (5, -toneRangeDb), hi (5, toneRangeDb), step (5, 2.0);
    const auto [x, e] = nelderMead ([&] (const std::vector<double>& v) { return toneError (r, { v[0], v[1], v[2], v[3], v[4] }, w); },
                                    std::vector<double> (theta.begin(), theta.end()), step, lo, hi, 300, 1.0e-6);
    return { { x[0], x[1], x[2], x[3], x[4] }, e };
}

// ---- Match EQ ------------------------------------------------------------------------------------------

std::pair<std::array<Equalizer::Band, Equalizer::numParametricBands>, std::vector<double>> fitMatchEq (const std::vector<double>& residual,
                                                                                                         const std::vector<double>& w)
{
    using BT = Equalizer::BandType;
    static constexpr std::array<BT, 5> kinds { BT::lowShelf, BT::peak, BT::peak, BT::peak, BT::highShelf };
    static constexpr std::array<double, 5> starts { 100.0, 400.0, 1000.0, 3000.0, 8000.0 };
    static constexpr std::array<std::pair<double, double>, 5> freqRanges { { { 40.0, 400.0 }, { 100.0, 1000.0 }, { 300.0, 3000.0 },
                                                                             { 1000.0, 8000.0 }, { 2000.0, 12000.0 } } };
    static constexpr double qLo = 0.3, qHi = 3.0, shelfQ = 0.7071;

    const auto m = weightedMean (residual, w);
    std::vector<double> target (residual.size());
    for (size_t b = 0; b < residual.size(); ++b)
        target[b] = std::clamp (residual[b] - m, -eqCapDb, eqCapDb);

    // Parameters, per band: log2 frequency, gain, and (peaks only) log2 Q: 2 + 3 + 3 + 3 + 2 = 13.
    struct Slot
    {
        size_t band;
        char what; // 'f', 'g', 'q'
    };
    std::vector<Slot> layout;
    for (size_t i = 0; i < kinds.size(); ++i)
    {
        layout.push_back ({ i, 'f' });
        layout.push_back ({ i, 'g' });
        if (kinds[i] == BT::peak)
            layout.push_back ({ i, 'q' });
    }

    auto decode = [&] (const std::vector<double>& p) {
        std::array<EqBand, 5> eq {};
        for (size_t i = 0; i < 5; ++i)
            eq[i] = { kinds[i], 0.0, 0.0, shelfQ };
        for (size_t k = 0; k < layout.size(); ++k)
        {
            auto& band = eq[layout[k].band];
            if (layout[k].what == 'f')      band.frequency = std::pow (2.0, p[k]);
            else if (layout[k].what == 'g') band.gainDb = p[k];
            else                            band.q = std::pow (2.0, p[k]);
        }
        return eq;
    };

    // numpy's interp on log2 frequency, held at the ends.
    auto interp = [&] (double x) {
        std::vector<double> xs ((size_t) numBands);
        for (int b = 0; b < numBands; ++b)
            xs[(size_t) b] = std::log2 (bands().centre[(size_t) b]);
        if (x <= xs.front()) return target.front();
        if (x >= xs.back()) return target.back();
        const auto it = std::upper_bound (xs.begin(), xs.end(), x);
        const auto j = (size_t) (it - xs.begin());
        const auto t = (x - xs[j - 1]) / (xs[j] - xs[j - 1]);
        return target[j - 1] + t * (target[j] - target[j - 1]);
    };

    std::vector<double> x0, lo, hi, step;
    for (const auto& s : layout)
    {
        if (s.what == 'f')
        {
            x0.push_back (std::log2 (starts[s.band]));
            lo.push_back (std::log2 (freqRanges[s.band].first));
            hi.push_back (std::log2 (freqRanges[s.band].second));
            step.push_back (0.5);
        }
        else if (s.what == 'g')
        {
            x0.push_back (interp (std::log2 (starts[s.band])));
            lo.push_back (-eqCapDb);
            hi.push_back (eqCapDb);
            step.push_back (2.0);
        }
        else
        {
            x0.push_back (0.0);
            lo.push_back (std::log2 (qLo));
            hi.push_back (std::log2 (qHi));
            step.push_back (0.5);
        }
    }

    auto err = [&] (const std::vector<double>& p) {
        const auto curve = eqDbOf (decode (p));
        std::vector<double> e (target.size());
        for (size_t b = 0; b < e.size(); ++b)
            e[b] = target[b] - curve[b];
        const auto mean = weightedMean (e, w);
        double num = 0.0, den = 0.0, ridge = 0.0;
        for (size_t b = 0; b < e.size(); ++b)
        {
            num += w[b] * (e[b] - mean) * (e[b] - mean);
            den += w[b];
        }
        for (size_t k = 0; k < layout.size(); ++k)
            if (layout[k].what == 'g')
                ridge += p[k] * p[k];
        return std::sqrt (num / den + eqRidge * ridge);
    };

    auto [p, e1] = nelderMead (err, x0, step, lo, hi, 3000, 1.0e-8);
    std::vector<double> smallStep (step);
    for (auto& v : smallStep)
        v *= 0.25;
    auto [p2, e2] = nelderMead (err, p, smallStep, lo, hi, 3000, 1.0e-9); // a restart with a smaller simplex
    juce::ignoreUnused (e1, e2);

    const auto eq = decode (p2);
    std::array<Equalizer::Band, Equalizer::numParametricBands> out {};
    for (size_t i = 0; i < 5; ++i)
        out[i] = { eq[i].type, (float) eq[i].frequency, (float) eq[i].gainDb, (float) eq[i].q };
    return { out, target };
}

} // namespace ampsim::tonematch
