// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "MatchCurve.h"
#include "MinimumPhase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ampsim
{

namespace
{
// ---- The smoothing's frequency axis ------------------------------------------------------------
//
// A Gaussian whose width (sigma, in Hz) depends on frequency: sigma(f) = max(minSmoothingHz, c f), with
// c = ln 2 / 12, the width in Hz of 1/12 octave around f (d(ln f) = df / f, so a sigma of ln 2 / 12 in
// natural-log frequency is c f in Hz). Smoothing with a varying width is a plain convolution on a warped
// axis u whose unit is one local sigma: du/df = 1 / sigma(f). Below the corner fc = minSmoothingHz / c the
// width is constant, u = f / minSmoothingHz; above it u = uc + ln(f / fc) / c. du/df is continuous at fc
// (both sides give 1 / minSmoothingHz), so the map has no kink. On u the kernel is a fixed Gaussian with
// sigma 1, sampled every 1/8 of a sigma, out to 4 sigma.
constexpr double logWidth = std::numbers::ln2 * MatchCurve::smoothingOctaves;
constexpr double cornerHz = MatchCurve::minSmoothingHz / logWidth;
constexpr double cornerU = cornerHz / MatchCurve::minSmoothingHz;
constexpr double gridStep = 0.125;
constexpr int kernelRadius = 32; // 4 sigma at 1/8 sigma per step

double warp (double hz)
{
    hz = std::max (0.0, hz);
    return hz <= cornerHz ? hz / MatchCurve::minSmoothingHz : cornerU + std::log (hz / cornerHz) / logWidth;
}

double unwarp (double u) { return u <= cornerU ? u * MatchCurve::minSmoothingHz : cornerHz * std::exp ((u - cornerU) * logWidth); }

/// Steps 1 and 2: the points interpolated linearly in dB over log2 frequency, held flat past both ends,
/// clamped to +-maxDb.
double rawDb (const std::vector<MatchCurve::Point>& points, double hz)
{
    if (points.empty())
        return 0.0;

    double db = points.back().db;
    if (hz <= points.front().hz)
        db = points.front().db;
    else if (hz < points.back().hz)
    {
        const auto next = std::upper_bound (points.begin(), points.end(), hz, [] (double f, const MatchCurve::Point& p) { return f < p.hz; });
        const auto& b = *next;
        const auto& a = *(next - 1);
        const auto t = std::log2 (hz / a.hz) / std::log2 (b.hz / a.hz);
        db = a.db + t * (b.db - a.db);
    }
    return std::clamp (db, -MatchCurve::maxDb, MatchCurve::maxDb);
}

/// Step 4: 1 inside 25 Hz to 18 kHz, a raised cosine down to 0 at 12.5 Hz (over one octave, on log
/// frequency) and at 22 kHz (linear frequency: the top of the band is narrow on a log axis), 0 outside.
double edgeTaper (double hz)
{
    if (hz < MatchCurve::lowEdgeHz)
    {
        const auto x = hz <= MatchCurve::lowZeroHz ? 0.0 : std::log2 (hz / MatchCurve::lowZeroHz) / std::log2 (MatchCurve::lowEdgeHz / MatchCurve::lowZeroHz);
        return 0.5 - 0.5 * std::cos (std::numbers::pi * x);
    }
    if (hz > MatchCurve::highEdgeHz)
    {
        const auto x = std::clamp ((MatchCurve::highZeroHz - hz) / (MatchCurve::highZeroHz - MatchCurve::highEdgeHz), 0.0, 1.0);
        return 0.5 - 0.5 * std::cos (std::numbers::pi * x);
    }
    return 1.0;
}

/// Step 3: the clamped curve sampled on the warped axis from 0 Hz to Nyquist and smoothed with the fixed
/// Gaussian (the ends held, so the edges aren't pulled toward 0 dB: step 4 does that, deliberately).
std::vector<double> smoothedGrid (const std::vector<MatchCurve::Point>& points)
{
    const auto top = warp (MatchCurve::designSampleRate / 2.0);
    const auto size = (int) std::ceil (top / gridStep) + 1;
    std::vector<double> raw ((size_t) size), out ((size_t) size);
    for (int i = 0; i < size; ++i)
        raw[(size_t) i] = rawDb (points, unwarp (i * gridStep));

    std::array<double, 2 * kernelRadius + 1> kernel {};
    double sum = 0.0;
    for (int k = -kernelRadius; k <= kernelRadius; ++k)
    {
        kernel[(size_t) (k + kernelRadius)] = std::exp (-0.5 * (k * gridStep) * (k * gridStep));
        sum += kernel[(size_t) (k + kernelRadius)];
    }
    for (auto& w : kernel)
        w /= sum;

    for (int i = 0; i < size; ++i)
    {
        double acc = 0.0;
        for (int k = -kernelRadius; k <= kernelRadius; ++k)
            acc += kernel[(size_t) (k + kernelRadius)] * raw[(size_t) std::clamp (i + k, 0, size - 1)];
        out[(size_t) i] = acc;
    }
    return out;
}

double onGrid (const std::vector<double>& grid, double hz)
{
    const auto position = std::min (warp (hz) / gridStep, (double) (grid.size() - 1));
    const auto i = std::min ((size_t) position, grid.size() - 2);
    const auto t = position - (double) i;
    return grid[i] + t * (grid[i + 1] - grid[i]);
}
} // namespace

// ---- The curve ------------------------------------------------------------------------------------

MatchCurve::Curve MatchCurve::Curve::fromPoints (std::vector<Point> points)
{
    points.erase (std::remove_if (points.begin(), points.end(),
                                  [] (const Point& p) { return ! std::isfinite (p.hz) || ! std::isfinite (p.db) || p.hz <= 0.0; }),
                  points.end());
    std::stable_sort (points.begin(), points.end(), [] (const Point& a, const Point& b) { return a.hz < b.hz; });

    Curve curve;
    for (const auto& p : points)
    {
        if (! curve.points.empty() && ! (curve.points.back().hz < p.hz)) // sorted, so this is the same frequency
            curve.points.back() = p;
        else
            curve.points.push_back (p);
    }

    if (curve.points.size() > (size_t) maxPoints)
    {
        // Thin evenly (keeping the first and last), rather than refusing a fine curve.
        std::vector<Point> thinned;
        const auto n = curve.points.size();
        for (int i = 0; i < maxPoints; ++i)
            thinned.push_back (curve.points[(size_t) std::llround ((double) i * (double) (n - 1) / (double) (maxPoints - 1))]);
        curve.points = std::move (thinned);
    }
    return curve;
}

juce::var MatchCurve::Curve::toVar() const
{
    juce::Array<juce::var> list;
    for (const auto& p : points)
        list.add (juce::Array<juce::var> { p.hz, p.db });
    return list;
}

MatchCurve::Curve MatchCurve::Curve::fromVar (const juce::var& v)
{
    std::vector<Point> points;
    if (const auto* list = v.getArray())
        for (const auto& item : *list)
            if (const auto* pair = item.getArray(); pair != nullptr && pair->size() == 2)
                points.push_back ({ (double) pair->getReference (0), (double) pair->getReference (1) });
    return fromPoints (std::move (points));
}

bool MatchCurve::Curve::isFlat() const
{
    return std::all_of (points.begin(), points.end(), [] (const Point& p) { return std::abs (p.db) < 0.001; });
}

std::vector<double> MatchCurve::targetDb (const Curve& curve, double amount, const std::vector<double>& frequencies)
{
    std::vector<double> out (frequencies.size(), 0.0);
    amount = std::clamp (amount, 0.0, 1.0);
    if (curve.isFlat() || amount <= 0.0)
        return out;

    const auto grid = smoothedGrid (curve.points);
    for (size_t i = 0; i < frequencies.size(); ++i)
        out[i] = amount * edgeTaper (frequencies[i]) * onGrid (grid, frequencies[i]); // steps 4 and 5
    return out;
}

double MatchCurve::targetDb (const Curve& curve, double amount, double hz)
{
    return targetDb (curve, amount, std::vector<double> { hz })[0];
}

std::vector<float> MatchCurve::designFir (const Curve& curve, double amount)
{
    if (curve.isFlat() || amount <= 0.0)
        return {};

    // The target on the design grid's bins, k fs / N for k = 0 .. N/2, as a natural-log magnitude:
    // ln |H| = dB ln(10) / 20.
    const auto size = 1 << designFftOrder;
    std::vector<double> frequencies ((size_t) size / 2 + 1);
    for (size_t k = 0; k < frequencies.size(); ++k)
        frequencies[k] = (double) k * designSampleRate / (double) size;

    auto logMagnitude = targetDb (curve, amount, frequencies);
    for (auto& v : logMagnitude)
        v *= std::numbers::ln10 / 20.0;

    // The minimum-phase filter with that magnitude (the folded real cepstrum), no delay, cut to firLength.
    auto fir = minphase::fromLogMagnitude (logMagnitude, designFftOrder, 0.0, firLength);

    // Its tail is already tiny (the smoothing floor keeps every feature wider than the FIR's resolution),
    // and a raised-cosine fade over the last quarter takes what's left smoothly to exactly 0 at the last
    // tap, so the cut adds no ripple to the magnitude (a hard cut would: a sinc in frequency).
    const auto fade = firLength / 4;
    for (int i = 0; i < fade; ++i)
        fir[(size_t) (firLength - fade + i)] *= (float) (0.5 + 0.5 * std::cos (std::numbers::pi * (double) (i + 1) / (double) fade));
    return fir;
}

// ---- The block ------------------------------------------------------------------------------------

MatchCurve::MatchCurve() = default;

void MatchCurve::setCurve (const Curve& curve, double amount)
{
    amount = std::clamp (amount, 0.0, 1.0);
    auto fir = designFir (curve, amount);

    {
        const std::scoped_lock lock (designedMutex);
        designedCurve = curve;
        designedAmount = amount;
        designedFir = fir;
    }

    // A flat curve becomes a one-tap identity, so the change to no correction crossfades like any other.
    if (fir.empty())
        fir = { 1.0f };

    auto pending = std::make_unique<PendingFir>();
    pending->taps.setSize (1, (int) fir.size());
    pending->taps.copyFrom (0, 0, fir.data(), (int) fir.size());
    handoff.publish (std::move (pending));
}

MatchCurve::Curve MatchCurve::getCurve() const
{
    const std::scoped_lock lock (designedMutex);
    return designedCurve;
}

double MatchCurve::getAmount() const
{
    const std::scoped_lock lock (designedMutex);
    return designedAmount;
}

std::vector<float> MatchCurve::getFir() const
{
    const std::scoped_lock lock (designedMutex);
    return designedFir;
}

void MatchCurve::prepare (double sampleRate, int maxBlockSize)
{
    // A FIR queued before prepare() is installed first, so JUCE builds its engine here and it plays from
    // the first buffer (what the offline renderer relies on). Safe: prepare() never overlaps process().
    installPending();
    convolution.prepare ({ sampleRate, (juce::uint32) maxBlockSize, 2 });

    delete toRetire;
    toRetire = nullptr;
    handoff.collect();
}

void MatchCurve::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    installPending();

    // No FIR yet: a plain wire, bit for bit.
    if (! installed)
        return;

    // Both sides through the same FIR (a mono IR on a two-channel engine runs one copy per channel).
    auto stereo = block.getSubsetChannelBlock (0, 2);
    convolution.process (juce::dsp::ProcessContextReplacing<float> (stereo));
}

void MatchCurve::installPending() noexcept
{
    if (toRetire != nullptr && handoff.retire (toRetire))
        toRetire = nullptr;

    if (toRetire != nullptr)
        return; // the last one hasn't been collected yet: try again next buffer

    if (auto* pending = handoff.take())
    {
        // Wait-free: the buffer is moved into JUCE's queue (nothing allocated or copied here), its own
        // background thread builds the engine, and it crossfades from the current one over 50 ms.
        installedLength = pending->taps.getNumSamples();
        convolution.loadImpulseResponse (std::move (pending->taps), designSampleRate, juce::dsp::Convolution::Stereo::no,
                                         juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
        installed = true;

        if (! handoff.retire (pending))
            toRetire = pending;
    }
}

} // namespace ampsim
