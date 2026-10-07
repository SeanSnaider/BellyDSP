// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "InformedMask.h"

#include "../dsp/FftDouble.h"
#include "../dsp/PitchDetector.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace ampsim::tonematch::informed
{

namespace
{
constexpr double fs = sampleRate;
constexpr double pi = juce::MathConstants<double>::pi;

// The expressions below are written as the prototype writes them (0.06 * SR, not 2880), so the doubles,
// and the int() truncations of them, come out identical.
int secondsToSamples (double s) { return (int) (s * fs); }

/// numpy's hanning(M): the symmetric Hann window, 0.5 - 0.5 cos(2 pi n / (M - 1)).
std::vector<double> symmetricHann (int m)
{
    std::vector<double> w ((size_t) m, 1.0);
    if (m > 1)
        for (int n = 0; n < m; ++n)
            w[(size_t) n] = 0.5 - 0.5 * std::cos (2.0 * pi * n / (m - 1));
    return w;
}

/// numpy's rfftfreq(n, 1 / fs): k / (n d), computed as numpy does (k times 1 / (n d)).
double rfftFrequency (int k, int n) { return k * (1.0 / (n * (1.0 / fs))); }

double median (std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const auto n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// numpy's interp on a uniform grid xp[k] = k step (k = 0 .. n-1): clamped at both ends.
double interpUniform (double x, double step, const std::vector<double>& fp)
{
    const auto n = (int) fp.size();
    if (x <= 0.0)
        return fp.front();
    const auto j = (int) std::floor (x / step);
    if (j >= n - 1)
        return fp.back();
    const auto x0 = j * step, x1 = (j + 1) * step;
    return (fp[(size_t) j + 1] - fp[(size_t) j]) / (x1 - x0) * (x - x0) + fp[(size_t) j];
}

/// numpy's interp on an increasing grid xp (any spacing): clamped at both ends.
double interp (double x, const std::vector<double>& xp, const std::vector<double>& fp)
{
    if (x <= xp.front())
        return fp.front();
    if (x >= xp.back())
        return fp.back();
    const auto j = (size_t) (std::upper_bound (xp.begin(), xp.end(), x) - xp.begin()) - 1;
    return (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]) * (x - xp[j]) + fp[j];
}

/// scipy.signal's _local_maxima_1d: every sample higher than its left neighbour and, after any run of equal
/// samples, higher than the next one; a flat top reports its middle (rounded down).
std::vector<int> localMaxima (const std::vector<double>& x)
{
    std::vector<int> peaks;
    const auto iMax = (int) x.size() - 1;
    int i = 1;
    while (i < iMax)
    {
        if (x[(size_t) i - 1] < x[(size_t) i])
        {
            auto ahead = i + 1;
            while (ahead < iMax && juce::exactlyEqual (x[(size_t) ahead], x[(size_t) i]))
                ++ahead;
            if (x[(size_t) ahead] < x[(size_t) i])
            {
                peaks.push_back ((i + ahead - 1) / 2);
                i = ahead;
            }
        }
        ++i;
    }
    return peaks;
}

/// scipy.signal's _select_by_peak_distance: from the highest peak down, every lower peak closer than
/// `distance` samples to a kept one is dropped.
std::vector<int> selectByDistance (const std::vector<int>& peaks, const std::vector<double>& height, int distance)
{
    const auto n = peaks.size();
    std::vector<size_t> order (n);
    std::iota (order.begin(), order.end(), size_t { 0 });
    std::stable_sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return height[a] < height[b]; });
    std::vector<uint8_t> keep (n, 1);
    for (size_t r = n; r-- > 0;)
    {
        const auto j = order[r];
        if (! keep[j])
            continue;
        for (auto k = (std::ptrdiff_t) j - 1; k >= 0 && peaks[j] - peaks[(size_t) k] < distance; --k)
            keep[(size_t) k] = 0;
        for (auto k = j + 1; k < n && peaks[k] - peaks[j] < distance; ++k)
            keep[k] = 0;
    }
    std::vector<int> out;
    for (size_t i = 0; i < n; ++i)
        if (keep[i])
            out.push_back (peaks[i]);
    return out;
}
} // namespace

// ---- Onsets ----------------------------------------------------------------------------------------------

std::vector<double> spectralFlux (const float* x, int numSamples, const std::vector<uint8_t>* bins, const std::atomic<bool>* cancel)
{
    // Frames of 1024 every 120 samples over [512 zeros, x, 1024 zeros]: floor((n + 512) / 120) + 1 frames,
    // frame t from sample t hop - 512.
    const auto numFrames = (numSamples + onsetFftSize / 2) / onsetHop + 1;
    const auto window = symmetricHann (onsetFftSize);
    std::vector<uint8_t> use ((size_t) onsetBins, 0);
    if (bins != nullptr)
        use = *bins;
    else
        for (int k = 0; k < onsetBins; ++k)
            use[(size_t) k] = rfftFrequency (k, onsetFftSize) >= 100.0 && rfftFrequency (k, onsetFftSize) <= 10000.0 ? 1 : 0;

    const FftDouble fft (onsetFftOrder);
    std::vector<std::complex<double>> work ((size_t) onsetFftSize);
    std::vector<double> mag ((size_t) onsetBins), previous ((size_t) onsetBins);
    auto magnitudes = [&] (int t) {
        const auto start = t * onsetHop - onsetFftSize / 2;
        for (int i = 0; i < onsetFftSize; ++i)
        {
            const auto n = start + i;
            work[(size_t) i] = { n >= 0 && n < numSamples ? (double) x[n] * window[(size_t) i] : 0.0, 0.0 };
        }
        fft.forward (work);
        for (int k = 0; k < onsetBins; ++k)
            mag[(size_t) k] = std::abs (work[(size_t) k]);
    };

    // Two passes: the largest magnitude anywhere first (the log's floor is relative to it), then the flux,
    // so no spectrogram is held.
    double largest = 0.0;
    const auto cancelled = [cancel] (int t) { return cancel != nullptr && (t & 255) == 0 && cancel->load(); };
    for (int t = 0; t < numFrames; ++t)
    {
        if (cancelled (t))
            return {};
        magnitudes (t);
        largest = std::max (largest, *std::max_element (mag.begin(), mag.end()));
    }
    const auto offset = 1.0e-4 * largest;

    std::vector<double> flux ((size_t) numFrames, 0.0);
    for (int t = 0; t < numFrames; ++t)
    {
        if (cancelled (t))
            return {};
        magnitudes (t);
        double s = 0.0;
        for (int k = 0; k < onsetBins; ++k)
        {
            const auto logMag = std::log (mag[(size_t) k] + offset);
            if (t > 0 && use[(size_t) k])
                s += std::max (0.0, logMag - previous[(size_t) k]);
            previous[(size_t) k] = logMag;
        }
        flux[(size_t) t] = s;
    }
    return flux;
}

std::vector<int> fluxPeaks (const std::vector<float>& di, const std::atomic<bool>* cancel)
{
    const auto sf = spectralFlux (di.data(), (int) di.size(), nullptr, cancel);
    const auto n = (int) sf.size();
    if (n == 0)
        return {};

    // The threshold: the moving median over 81 frames (+-100 ms; zeros beyond the ends, as scipy's medfilt)
    // plus a tenth of the flux's 99th percentile.
    const auto top = 0.1 * percentile (sf, 99.0);
    std::vector<double> threshold ((size_t) n), window (81);
    for (int i = 0; i < n; ++i)
    {
        for (int k = -40; k <= 40; ++k)
            window[(size_t) (k + 40)] = i + k >= 0 && i + k < n ? sf[(size_t) (i + k)] : 0.0;
        std::nth_element (window.begin(), window.begin() + 40, window.end());
        threshold[(size_t) i] = window[40] + top;
    }

    std::vector<int> peaks;
    std::vector<double> heights;
    for (auto p : localMaxima (sf))
        if (sf[(size_t) p] >= threshold[(size_t) p])
        {
            peaks.push_back (p);
            heights.push_back (sf[(size_t) p]);
        }
    const auto distance = (int) std::ceil ((double) (int) (0.045 * fs / onsetHop));
    auto kept = selectByDistance (peaks, heights, distance);
    for (auto& p : kept)
        p *= onsetHop;
    return kept;
}

int refineOnset (const std::vector<float>& x, int guess, int radius)
{
    // seg = x[lo - 48 : hi + 48]^2; env = sqrt of its 48-sample moving mean (numpy's convolve "same": the
    // mean of seg[i - 24 .. i + 23], zeros outside); the rise e[i + 48] - e[i]; its argmax over the first
    // hi - lo positions, minus half the window.
    const auto len = (int) x.size();
    const auto lo = std::max (48, guess - radius), hi = std::min (len - 1, guess + radius);
    const auto segStart = lo - 48, segEnd = std::min (len, hi + 48);
    const auto segLength = segEnd - segStart;
    if (segLength <= 48 || hi <= lo)
        return guess;
    std::vector<double> sq ((size_t) segLength), env ((size_t) segLength);
    for (int i = 0; i < segLength; ++i)
        sq[(size_t) i] = (double) x[(size_t) (segStart + i)] * (double) x[(size_t) (segStart + i)];
    const auto kernel = 1.0 / 48.0;
    for (int i = 0; i < segLength; ++i)
    {
        double s = 0.0;
        for (int j = std::max (0, i - 24); j <= std::min (segLength - 1, i + 23); ++j)
            s += sq[(size_t) j] * kernel;
        env[(size_t) i] = std::sqrt (s);
    }
    const auto count = std::min (hi - lo, segLength - 48);
    int best = 0;
    double bestRise = -1.0e300;
    for (int i = 0; i < count; ++i)
    {
        const auto rise = env[(size_t) i + 48] - env[(size_t) i];
        if (rise > bestRise)
        {
            bestRise = rise;
            best = i;
        }
    }
    return lo + best - 24;
}

std::vector<int> detectOnsets (const std::vector<float>& di, const std::vector<int>& peaks)
{
    std::vector<int> onsets;
    for (auto p : peaks)
        onsets.push_back (refineOnset (di, p));
    return onsets;
}

int fluxLag (const std::vector<int>& peaks, const std::vector<int>& onsets)
{
    if (peaks.empty() || onsets.empty())
        return 0;
    std::vector<double> d;
    for (auto p : peaks)
    {
        // The nearest onset (the first of equals, as numpy's argmin).
        size_t nearest = 0;
        for (size_t k = 1; k < onsets.size(); ++k)
            if (std::abs (onsets[k] - p) < std::abs (onsets[nearest] - p))
                nearest = k;
        d.push_back ((double) (p - onsets[nearest]));
    }
    return (int) median (d); // int() of numpy's median: truncated toward zero
}

std::vector<double> noteF0 (const std::vector<float>& di, const std::vector<int>& onsets, const std::vector<int>& ends,
                            const std::atomic<bool>* cancel)
{
    // The readings: decimated index n (the newest of the frame is n - 1) runs from (o + 20 ms) / 4 + L to
    // end / 4 in steps of 120 (10 ms at 12 kHz), L the detector's frame length. The detector's history holds
    // n decimated samples once 4n input samples are in, so one pass over the DI serves every note in order.
    PitchDetector detector;
    detector.prepare (fs, PitchDetector::Settings::harmonizer());
    const auto length = detector.getFrameLength();
    const auto step = (int) (fs / PitchDetector::decimation) / 100;

    std::vector<double> out;
    int pushed = 0; // input samples pushed so far
    for (size_t k = 0; k < onsets.size(); ++k)
    {
        if (cancel != nullptr && cancel->load())
            return {};
        std::vector<double> readings;
        for (int n = (onsets[k] + secondsToSamples (0.02)) / 4 + length; n < ends[k] / 4; n += step)
        {
            const auto need = std::min ((int) di.size(), 4 * n);
            if (need > pushed)
            {
                detector.push (di.data() + pushed, need - pushed);
                pushed = need;
            }
            const auto e = detector.detect();
            if (e.clarity >= 0.9)
                readings.push_back (e.frequency);
        }
        out.push_back (median (readings));
    }
    return out;
}

std::vector<uint8_t> harmonicBins (double f0, int harmonics, double tolerance)
{
    std::vector<uint8_t> bins ((size_t) onsetBins, 0);
    for (int k = 0; k < onsetBins; ++k)
    {
        const auto f = rfftFrequency (k, onsetFftSize);
        if (f0 <= 0.0)
            bins[(size_t) k] = f >= 100.0 && f <= 10000.0 ? 1 : 0;
        else
            for (int h = 1; h <= harmonics && ! bins[(size_t) k]; ++h)
                bins[(size_t) k] = std::abs (f - h * f0) <= tolerance * h * f0 + 0.6 * fs / onsetFftSize ? 1 : 0;
    }
    return bins;
}

// ---- The notes in the target ---------------------------------------------------------------------------

std::vector<Note> alignNotes (const std::vector<float>& di, const std::vector<float>& target, int bandFrames,
                              const std::atomic<bool>& cancel, AlignmentDetail* detail)
{
    AlignmentDetail local;
    auto& d = detail != nullptr ? *detail : local;
    d = {};
    d.peaks = fluxPeaks (di, &cancel);
    d.onsets = detectOnsets (di, d.peaks);
    if (d.onsets.empty() || target.empty() || cancel.load())
        return {};
    const auto& onsets = d.onsets;
    const auto numNotes = onsets.size();
    d.ends.assign (onsets.begin() + 1, onsets.end());
    d.ends.push_back (std::min ((int) di.size(), onsets.back() + secondsToSamples (1.0)));
    d.f0 = noteF0 (di, onsets, d.ends, &cancel);
    if (cancel.load())
        return {};

    // The coarse map: chroma DTW (tone match's own align) between the target and the take through a plain
    // distortion, tanh(10 x), so the take's partials look like a distorted guitar's. Each take frame's mean
    // partner on the target's clock, at the frames' centres (t hop + N/2), interpolated linearly.
    std::vector<float> distorted (di.size());
    for (size_t i = 0; i < di.size(); ++i)
        distorted[i] = (float) std::tanh (10.0 * (double) di[i]);
    d.path = align (Analysis::of (target), Analysis::of (distorted), bandFrames);
    if (cancel.load() || d.path.path.empty())
        return {};
    std::vector<double> uj, meanI;
    {
        std::vector<std::pair<int, int>> byJ;
        for (const auto& [i, j] : d.path.path)
            byJ.emplace_back (j, i);
        std::stable_sort (byJ.begin(), byJ.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t s = 0; s < byJ.size();)
        {
            size_t e = s;
            double sum = 0.0;
            while (e < byJ.size() && byJ[e].first == byJ[s].first)
                sum += byJ[e++].second;
            uj.push_back (byJ[s].first);
            meanI.push_back (sum / (double) (e - s));
            s = e;
        }
    }
    const auto centre = fftSize / 2.0;
    for (auto o : onsets)
        d.estimates.push_back (interp ((o - centre) / hop, uj, meanI) * hop + centre);

    // The fine map: the target's flux over this note's harmonic bins, times a 30 ms Gaussian prior around the
    // estimate, maximized within +-60 ms, minus the flux's own lag (measured on the take).
    d.lag = fluxLag (d.peaks, onsets);
    const auto len = (int) target.size();
    d.targetOnsets.resize (numNotes);
    for (size_t k = 0; k < numNotes; ++k)
    {
        if (cancel.load())
            return {};
        const auto e = d.estimates[k];
        auto lo = (int) (e - 0.06 * fs), hi = (int) (e + 0.06 * fs);
        lo = std::max (0, lo);
        hi = std::min (len - 1, hi);
        if (hi - lo < 4 * onsetHop)
        {
            d.targetOnsets[k] = (int) juce::jlimit (0.0, (double) (len - 1), e);
            continue;
        }
        const auto bins = harmonicBins (d.f0[k]);
        const auto segStart = std::max (0, lo - onsetFftSize), segEnd = std::min (len, hi + onsetFftSize);
        const auto sf = spectralFlux (target.data() + segStart, segEnd - segStart, &bins, &cancel);
        double best = -1.0;
        int bestTime = 0;
        bool any = false;
        for (size_t f = 0; f < sf.size(); ++f)
        {
            const auto time = segStart + (int) f * onsetHop;
            if (time < lo || time > hi)
                continue;
            const auto z = (time - e) / (0.03 * fs);
            const auto score = sf[f] * std::exp (-0.5 * z * z);
            if (! any || score > best)
            {
                best = score;
                bestTime = time;
                any = true;
            }
        }
        d.targetOnsets[k] = any && best > 0.0 ? bestTime - d.lag : (int) e;
    }
    // In order, at least 20 ms apart.
    for (size_t k = 1; k < numNotes; ++k)
        d.targetOnsets[k] = std::max (d.targetOnsets[k], d.targetOnsets[k - 1] + secondsToSamples (0.02));
    d.targetNext.assign (d.targetOnsets.begin() + 1, d.targetOnsets.end());
    d.targetNext.push_back (std::min (len, d.targetOnsets.back() + secondsToSamples (1.0)));

    std::vector<Note> notes;
    for (size_t k = 0; k < numNotes; ++k)
    {
        const auto length = std::min (d.ends[k] - onsets[k], d.targetNext[k] - d.targetOnsets[k]);
        if (length > secondsToSamples (0.03) && d.targetOnsets[k] + length <= len)
        {
            Note n;
            n.o = onsets[k];
            n.oNext = d.ends[k];
            n.t = d.targetOnsets[k];
            n.tNext = d.targetNext[k];
            n.length = length;
            n.f0 = d.f0[k];
            notes.push_back (n);
        }
    }
    return notes;
}

double refinePitchInTarget (const std::vector<float>& target, const Note& note)
{
    // target[t + 20 ms : t + L], its first 16384 samples under a symmetric Hann of their length, zero-padded
    // to 16384 points; the magnitude spectrum read by linear interpolation at h f0' for h = 1 .. 8.
    const auto from = std::min ((int) target.size(), note.t + secondsToSamples (0.02));
    const auto to = std::min ((int) target.size(), note.t + note.length);
    const auto segLength = std::max (0, to - from);
    if (note.f0 <= 0.0 || segLength < 2048)
        return note.f0;
    constexpr int order = 14, n = 1 << order;
    const auto used = std::min (segLength, n);
    const auto window = symmetricHann (used);
    std::vector<std::complex<double>> work ((size_t) n);
    for (int i = 0; i < used; ++i)
        work[(size_t) i] = { (double) target[(size_t) (from + i)] * window[(size_t) i], 0.0 };
    FftDouble (order).forward (work);
    std::vector<double> mag ((size_t) n / 2 + 1);
    for (size_t k = 0; k < mag.size(); ++k)
        mag[k] = std::abs (work[k]);
    const auto binHz = 1.0 / (n * (1.0 / fs));

    double best = -1.0, bestCents = 0.0;
    for (int i = 0; i <= 24; ++i)
    {
        const auto c = -60.0 + i * 5.0;
        const auto f0 = note.f0 * std::pow (2.0, c / 1200.0);
        double s = 0.0;
        for (int h = 1; h <= 8; ++h)
            s += interpUniform (h * f0, binHz, mag);
        if (s > best)
        {
            best = s;
            bestCents = c;
        }
    }
    return note.f0 * std::pow (2.0, bestCents / 1200.0);
}

// ---- The mask --------------------------------------------------------------------------------------------

Mask harmonicMask (const std::vector<float>& target, std::vector<Note>& notes, const MaskSettings& settings, const std::atomic<bool>& cancel)
{
    Mask mask;
    mask.frames = Mask::framesFor ((int) target.size());
    mask.values.assign ((size_t) mask.frames * maskBins, (float) settings.floor);
    const auto binHz = fs / maskFftSize;
    const auto lobe = std::pow (2.0, settings.cents / 1200.0) - 1.0;

    // Frame k's time as scipy computes it ((N/2 + k hop) / fs - (N/2) / fs, times fs: within 1e-9 of k hop,
    // but compared with whole samples below, so computed the same way).
    std::vector<double> frameTime ((size_t) mask.frames);
    for (int k = 0; k < mask.frames; ++k)
        frameTime[(size_t) k] = ((maskFftSize / 2.0 + k * (double) maskHop) / fs - (maskFftSize / 2.0) / fs) * fs;

    std::vector<double> comb ((size_t) maskBins);
    for (auto& note : notes)
    {
        if (cancel.load())
            return {};
        note.f0Target = refinePitchInTarget (target, note);
        const auto f0 = note.f0Target;
        if (f0 <= 0.0)
            continue;

        // The comb: each harmonic a Gaussian lobe, the largest wins. Values under the floor can't matter (the
        // mask is at least the floor), and exp(-z^2 / 2) < 0.1 beyond |z| = 2.15, so each lobe is evaluated
        // only within 4 sigma.
        std::fill (comb.begin(), comb.end(), 0.0);
        for (int h = 1; h <= settings.harmonics; ++h)
        {
            const auto fh = h * f0;
            if (fh > 0.45 * fs)
                break;
            const auto sigma = std::max (fh * lobe, 1.5 * binHz);
            const auto first = std::max (0, (int) std::floor ((fh - 4.0 * sigma) / binHz));
            const auto last = std::min (maskBins - 1, (int) std::ceil ((fh + 4.0 * sigma) / binHz));
            for (int b = first; b <= last; ++b)
            {
                const auto z = (rfftFrequency (b, maskFftSize) - fh) / sigma;
                comb[(size_t) b] = std::max (comb[(size_t) b], std::exp (-0.5 * z * z));
            }
        }
        // The frames whose window (centred on its time, N long) overlaps the note's span [t, tNext).
        for (int k = 0; k < mask.frames; ++k)
        {
            const auto t = frameTime[(size_t) k];
            if (! (t + maskFftSize / 2.0 > note.t && t - maskFftSize / 2.0 < note.tNext))
                continue;
            auto* column = mask.values.data() + (size_t) k * maskBins;
            for (int b = 0; b < maskBins; ++b)
                column[b] = std::max (column[b], (float) comb[(size_t) b]);
        }
    }

    // Along time: each bin's moving average over 2 r + 1 frames (zeros beyond the ends, as numpy's convolve
    // "same"), floored again.
    if (settings.timeSmooth > 0)
    {
        const auto r = settings.timeSmooth;
        const auto weight = 1.0 / (2 * r + 1);
        std::vector<double> row ((size_t) mask.frames);
        for (int b = 0; b < maskBins; ++b)
        {
            for (int k = 0; k < mask.frames; ++k)
                row[(size_t) k] = mask.at (k, b);
            for (int k = 0; k < mask.frames; ++k)
            {
                double s = 0.0;
                for (int j = std::max (0, k - r); j <= std::min (mask.frames - 1, k + r); ++j)
                    s += row[(size_t) j] * weight;
                mask.values[(size_t) k * maskBins + (size_t) b] = (float) std::max (settings.floor, s);
            }
        }
    }
    return mask;
}

std::vector<float> applyMask (const std::vector<float>& target, const Mask& mask, const std::atomic<bool>& cancel)
{
    // The STFT and its inverse as scipy's stft / istft do them (boundary zeros, padded): frame k covers
    // samples [k hop - N/2, k hop + N/2) under the periodic Hann w[i] = 0.5 - 0.5 cos(2 pi i / N). The
    // inverse is the least-squares one (Griffin and Lim 1984): each frame's inverse FFT windowed by w again
    // and overlap-added, the sum divided by sum_k w^2(n - k hop). At hop N/8 those squared windows add to a
    // constant (3/8 N / hop = 3: the constant-overlap-add condition for w^2, which periodic Hann meets at any
    // hop of N/4 or less), so an unmasked frame comes back exactly; dividing by the actual sum also keeps
    // the first and last samples exact, where fewer frames overlap. The mask is real and the same for bin j
    // and its mirror N - j, so the spectrum stays conjugate-symmetric and the output real.
    const auto n = (int) target.size();
    if (mask.frames != Mask::framesFor (n))
        return {};
    std::vector<double> window ((size_t) maskFftSize);
    for (int i = 0; i < maskFftSize; ++i)
        window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * pi * i / maskFftSize);

    const FftDouble fft (maskFftOrder);
    std::vector<std::complex<double>> work ((size_t) maskFftSize);
    std::vector<double> sum ((size_t) n, 0.0), norm ((size_t) n, 0.0);
    for (int k = 0; k < mask.frames; ++k)
    {
        if ((k & 63) == 0 && cancel.load())
            return {};
        const auto start = k * maskHop - maskFftSize / 2;
        for (int i = 0; i < maskFftSize; ++i)
        {
            const auto s = start + i;
            work[(size_t) i] = { s >= 0 && s < n ? (double) target[(size_t) s] * window[(size_t) i] : 0.0, 0.0 };
        }
        fft.forward (work);
        const auto* m = mask.values.data() + (size_t) k * maskBins;
        for (int b = 0; b < maskBins; ++b)
        {
            work[(size_t) b] *= (double) m[b];
            if (b > 0 && b < maskFftSize / 2)
                work[(size_t) (maskFftSize - b)] *= (double) m[b];
        }
        fft.inverse (work);
        for (int i = 0; i < maskFftSize; ++i)
        {
            const auto s = start + i;
            if (s >= 0 && s < n)
            {
                sum[(size_t) s] += work[(size_t) i].real() * window[(size_t) i];
                norm[(size_t) s] += window[(size_t) i] * window[(size_t) i];
            }
        }
    }
    std::vector<float> out ((size_t) n);
    for (int i = 0; i < n; ++i)
        out[(size_t) i] = (float) (norm[(size_t) i] > 1.0e-10 ? sum[(size_t) i] / norm[(size_t) i] : sum[(size_t) i]);
    return out;
}

// ---- The whole cleanup ----------------------------------------------------------------------------------

Result cleanUp (const std::vector<float>& target, const std::vector<float>& take, double bandSeconds, const std::atomic<bool>& cancel,
                const ProgressFn& progress, const MaskSettings& settings)
{
    const auto started = juce::Time::getMillisecondCounterHiRes();
    Result r;
    auto report = [&] (double f, const juce::String& s) {
        if (progress)
            progress (f, s);
    };
    auto stop = [&] {
        r.cancelled = true;
        r.error = "Cancelled";
        return r;
    };

    report (0.0, "Cleaning up the target: finding the notes in your take");
    const auto bandFrames = bandSeconds > 0.0 ? (int) std::ceil (bandSeconds * fs / hop) : 0;
    AlignmentDetail detail;
    auto notes = alignNotes (take, target, bandFrames, cancel, &detail);
    if (cancel.load())
        return stop();
    r.onsets = (int) detail.onsets.size();
    r.notes = (int) notes.size();
    for (const auto& note : notes)
        r.pitched += note.f0 > 0.0 ? 1 : 0;
    if (r.pitched == 0)
    {
        r.error = r.onsets == 0 ? "found no notes in your take" : "found no notes with a clear pitch in your take";
        return r;
    }

    report (0.5, "Cleaning up the target: keeping the harmonics of your " + juce::String (r.pitched) + " notes");
    const auto mask = harmonicMask (target, notes, settings, cancel);
    if (cancel.load())
        return stop();
    report (0.7, "Cleaning up the target: the inverse STFT");
    r.output = applyMask (target, mask, cancel);
    if (cancel.load() || r.output.empty())
        return stop();

    double before = 0.0, after = 0.0;
    for (size_t i = 0; i < target.size(); ++i)
    {
        before += (double) target[i] * target[i];
        after += (double) r.output[i] * r.output[i];
    }
    r.keptDb = 10.0 * std::log10 (std::max (after, 1.0e-30) / std::max (before, 1.0e-30));
    r.ok = true;
    r.seconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
    report (1.0, "Cleaned up the target");
    return r;
}

Bleed measureBleed (const std::vector<float>& target, const std::vector<float>& take, const std::vector<float>& proxy, double bandSeconds,
                    const std::atomic<bool>& cancel)
{
    Bleed b;
    b.cleanedTarget = cleanUp (target, take, bandSeconds, cancel);
    if (b.cleanedTarget.cancelled || cancel.load())
    {
        b.cancelled = true;
        b.error = "Cancelled";
        return b;
    }
    if (! b.cleanedTarget.ok)
    {
        b.error = b.cleanedTarget.error;
        return b;
    }
    const auto p = cleanUp (proxy, take, bandSeconds, cancel);
    if (p.cancelled || cancel.load())
    {
        b.cancelled = true;
        b.error = "Cancelled";
        return b;
    }
    if (! p.ok)
    {
        b.error = p.error;
        return b;
    }
    b.keptTargetDb = b.cleanedTarget.keptDb;
    b.keptProxyDb = p.keptDb;
    b.excessDb = b.keptProxyDb - b.keptTargetDb;
    b.ok = true;
    return b;
}

} // namespace ampsim::tonematch::informed
