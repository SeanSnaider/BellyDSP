// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "TakeScore.h"

#include "../dsp/AmpTone.h"
#include "../dsp/FftDouble.h"
#include "../dsp/Svf.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace ampsim::tonematch::take
{

namespace
{
double erbNumber (double f) { return 21.4 * std::log10 (1.0 + 0.00437 * f); }
double erbHz (double e) { return (std::pow (10.0, e / 21.4) - 1.0) / 0.00437; }
double binHz (int k) { return k * sampleRate / erbFftSize; }
double dbOf (double p) { return 10.0 * std::log10 (std::max (p, 1.0e-20)); }

Svf::Type svfType (Equalizer::BandType t)
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
} // namespace

const ErbBands& erbBands()
{
    static const ErbBands bands = [] {
        ErbBands b;
        const auto lo = erbNumber (50.0), hi = erbNumber (15000.0);
        for (double e = lo; e <= hi + 1.0e-9; e += 1.0)
        {
            const auto c = erbHz (e);
            b.centre.push_back (c);
            const auto fk = c / 1000.0;
            // ITU-R BS.1387 (PEAQ): the outer and middle ear; Terhardt (1979): the threshold in quiet.
            b.outerEarDb.push_back (-2.184 * std::pow (fk, -0.8) + 6.5 * std::exp (-0.6 * (fk - 3.3) * (fk - 3.3)) - 0.001 * std::pow (fk, 3.6));
            b.thresholdDb.push_back (3.64 * std::pow (fk, -0.8) - 6.5 * std::exp (-0.6 * (fk - 3.3) * (fk - 3.3)) + 0.001 * std::pow (fk, 4.0));
            const auto ec = erbNumber (c);
            int first = -1, last = -1, nearest = 0;
            double nearestDistance = 1.0e300;
            for (int k = 0; k < erbBins; ++k)
            {
                const auto d = std::abs (erbNumber (binHz (k)) - ec);
                if (d <= 0.5)
                {
                    if (first < 0)
                        first = k;
                    last = k + 1;
                }
                if (d < nearestDistance)
                {
                    nearestDistance = d;
                    nearest = k;
                }
            }
            if (first < 0)
            {
                first = nearest;
                last = nearest + 1;
            }
            b.first.push_back (first);
            b.last.push_back (last);
        }
        return b;
    }();
    return bands;
}

std::vector<double> ltasBins (const float* x, int numSamples)
{
    const auto n = std::max (numSamples, erbFftSize);
    const auto frames = 1 + (n - erbFftSize) / erbHop;
    std::vector<double> window ((size_t) erbFftSize);
    for (int i = 0; i < erbFftSize; ++i) // numpy's hanning: symmetric
        window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (erbFftSize - 1));

    FftDouble fft (erbFftOrder);
    std::vector<double> power ((size_t) frames * erbBins), level ((size_t) frames);
    std::vector<std::complex<double>> work ((size_t) erbFftSize);
    for (int t = 0; t < frames; ++t)
    {
        for (int i = 0; i < erbFftSize; ++i)
        {
            const auto idx = t * erbHop + i;
            work[(size_t) i] = { (idx < numSamples ? (double) x[idx] : 0.0) * window[(size_t) i], 0.0 };
        }
        fft.forward (work);
        double total = 0.0;
        for (int k = 0; k < erbBins; ++k)
        {
            const auto p = std::norm (work[(size_t) k]);
            power[(size_t) t * erbBins + (size_t) k] = p;
            total += p;
        }
        level[(size_t) t] = dbOf (total);
    }
    const auto threshold = percentile (level, 95.0) - activeRangeDb;
    std::vector<double> out ((size_t) erbBins, 0.0);
    int count = 0;
    for (int t = 0; t < frames; ++t)
        if (level[(size_t) t] > threshold)
        {
            for (int k = 0; k < erbBins; ++k)
                out[(size_t) k] += power[(size_t) t * erbBins + (size_t) k];
            ++count;
        }
    for (auto& v : out)
        v /= std::max (1, count);
    return out;
}

std::vector<double> bandsDb (const std::vector<double>& bins, const std::vector<double>* filterPower)
{
    const auto& b = erbBands();
    std::vector<double> out ((size_t) b.size());
    for (int i = 0; i < b.size(); ++i)
    {
        double s = 0.0;
        for (int k = b.first[(size_t) i]; k < b.last[(size_t) i]; ++k)
            s += bins[(size_t) k] * (filterPower != nullptr ? (*filterPower)[(size_t) k] : 1.0);
        out[(size_t) i] = dbOf (s / (b.last[(size_t) i] - b.first[(size_t) i]));
    }
    return out;
}

std::vector<double> loudnessWeights (const std::vector<double>& bandDb)
{
    const auto& b = erbBands();
    double loudest = -1.0e300;
    for (size_t i = 0; i < bandDb.size(); ++i)
        loudest = std::max (loudest, bandDb[i] + b.outerEarDb[i]);
    const auto p = 85.0 - loudest;
    std::vector<double> n (bandDb.size());
    double sum = 0.0;
    for (size_t i = 0; i < bandDb.size(); ++i)
    {
        // Zwicker's power law: specific loudness grows as intensity^0.23 above the threshold.
        n[i] = std::max (0.0, std::pow (10.0, 0.023 * (bandDb[i] + b.outerEarDb[i] + p - b.thresholdDb[i])) - 1.0);
        sum += n[i];
    }
    for (auto& v : n)
        v /= std::max (sum, 1.0e-12);
    return n;
}

double loudnessDistance (const std::vector<double>& ref, const std::vector<double>& m, const std::vector<double>& w)
{
    double dbar = 0.0;
    for (size_t i = 0; i < ref.size(); ++i)
        dbar += w[i] * (ref[i] - m[i]);
    double s = 0.0;
    for (size_t i = 0; i < ref.size(); ++i)
    {
        const auto d = ref[i] - m[i] - dbar;
        s += w[i] * d * d;
    }
    return std::sqrt (s);
}

std::vector<double> linearPower (const std::array<double, 5>& tone, const std::array<Equalizer::Band, Equalizer::numParametricBands>* eq)
{
    std::vector<double> h ((size_t) erbBins, 1.0);
    const auto apply = [&h] (const Svf::Coefficients& c) {
        for (int k = 0; k < erbBins; ++k)
            h[(size_t) k] *= std::norm (Svf::responseAt (c, binHz (k), sampleRate));
    };
    for (size_t band = 0; band < AmpTone::numBands; ++band)
        if (tone[band] != 0.0)
            apply (Svf::design (AmpTone::bands[band].type, AmpTone::bands[band].frequency, AmpTone::bands[band].q, tone[band], sampleRate));
    if (eq != nullptr)
        for (const auto& band : *eq)
            if (band.gainDb != 0.0f)
                apply (Svf::design (svfType (band.type), band.frequency, band.q, band.gainDb, sampleRate));
    return h;
}

NoteMeasures noteMeasures (const std::vector<float>& x, const std::vector<int>& starts, const std::vector<int>& lengths)
{
    NoteMeasures m;
    for (size_t k = 0; k < starts.size(); ++k)
    {
        double total = 0.0, attack = 0.0;
        const auto o = starts[k], n = lengths[k];
        for (int i = 0; i < n && o + i < (int) x.size(); ++i)
        {
            const auto e = (double) x[(size_t) (o + i)] * x[(size_t) (o + i)];
            total += e;
            if (i < attackSamples)
                attack += e;
        }
        total += 1.0e-30;
        m.attack.push_back (10.0 * std::log10 (attack / total + 1.0e-6));
        m.level.push_back (10.0 * std::log10 (total / std::max (1, n) + 1.0e-20));
    }
    return m;
}

TakeNotes TakeNotes::of (const std::vector<float>& target, const std::vector<informed::Note>& notes)
{
    TakeNotes tn;
    for (const auto& n : notes)
    {
        const auto length = std::min (n.length, noteMaxSamples);
        if (length < noteMinSamples)
            continue;
        tn.takeOnsets.push_back (n.o);
        tn.targetOnsets.push_back (n.t);
        tn.lengths.push_back (length);
    }
    tn.target = noteMeasures (target, tn.targetOnsets, tn.lengths);
    for (auto l : tn.target.level)
        tn.noteWeights.push_back (std::pow (std::pow (10.0, l / 10.0), 0.3));
    tn.erbDb = bandsDb (ltasBins (target));
    tn.erbWeights = loudnessWeights (tn.erbDb);
    return tn;
}

namespace
{
double stdDev (const std::vector<double>& v)
{
    if (v.empty())
        return 0.0;
    double mean = 0.0;
    for (auto x : v)
        mean += x;
    mean /= (double) v.size();
    double s = 0.0;
    for (auto x : v)
        s += (x - mean) * (x - mean);
    return std::sqrt (s / (double) v.size());
}
} // namespace

Score score (const TakeNotes& tn, const Analysis& target, const std::vector<float>& ampRender, const std::vector<float>& candidate,
             const std::array<double, 5>& tone, const std::array<Equalizer::Band, Equalizer::numParametricBands>* eq, double spectral,
             const Weights& w)
{
    const auto ca = Analysis::of (candidate);
    return scoreOf (tn, target, ca.features[1], ca.features[3], noteMeasures (ampRender, tn.takeOnsets, tn.lengths), ltasBins (candidate), tone, eq,
                    spectral, w);
}

Score scoreOf (const TakeNotes& tn, const Analysis& target, double flux, double crest, const NoteMeasures& m, const std::vector<double>& candidateLtasBins,
               const std::array<double, 5>& tone, const std::array<Equalizer::Band, Equalizer::numParametricBands>* eq, double spectral, const Weights& w)
{
    Score s;
    s.spectral = spectral;
    s.flux = std::abs (target.features[1] - flux);
    s.crest = std::abs (target.features[3] - crest);
    double num = 0.0, den = 0.0;
    for (size_t k = 0; k < m.attack.size(); ++k)
    {
        num += tn.noteWeights[k] * std::abs (tn.target.attack[k] - m.attack[k]);
        den += tn.noteWeights[k];
    }
    s.attack = den > 0.0 ? num / den : 0.0;
    s.spread = std::abs (stdDev (tn.target.level) - stdDev (m.level));
    const auto filter = linearPower (tone, eq);
    s.erb = loudnessDistance (tn.erbDb, bandsDb (candidateLtasBins, &filter), tn.erbWeights);
    s.total = spectral + w.flux * s.flux + w.crest * s.crest + w.attack * s.attack + w.spread * s.spread + w.erb * s.erb;
    return s;
}

} // namespace ampsim::tonematch::take
