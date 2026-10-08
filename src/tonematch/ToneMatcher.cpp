// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "ToneMatcher.h"

#include "../dsp/AmpSection.h"
#include "../dsp/CabIR.h"
#include "../dsp/AmpTone.h"
#include "../dsp/Equalizer.h"
#include "../dsp/Svf.h"
#include "InformedMask.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <map>
#include <numeric>
#include <mutex>
#include <set>
#include <thread>

namespace ampsim::tonematch
{

namespace
{
/// Runs fn(index, worker) for index 0 .. count-1 on `threads` threads (the caller's included), each
/// taking the next index from a shared counter, so slow and fast jobs balance out.
void parallelFor (int count, int threads, const std::function<void (int, int)>& fn)
{
    threads = juce::jlimit (1, juce::jmax (1, count), threads);
    std::atomic<int> next { 0 };
    auto worker = [&] (int w) {
        for (int i = next++; i < count; i = next++)
            fn (i, w);
    };
    std::vector<std::thread> pool;
    for (int w = 1; w < threads; ++w)
        pool.emplace_back (worker, w);
    worker (0);
    for (auto& t : pool)
        t.join();
}

int defaultThreads()
{
    return juce::jmax (1, (int) std::thread::hardware_concurrency() - 1);
}

constexpr int renderBlock = 128;
} // namespace

// ---- Rendering and the cab ------------------------------------------------------------------------------

namespace
{
/// Runs a mono block over x in place, in the chain's block size, the DI as its context. False if cancelled.
bool runBlock (Block& block, std::vector<float>& x, const std::vector<float>& di, const std::atomic<bool>& cancel)
{
    juce::AudioBuffer<float> buffer (1, renderBlock);
    for (size_t start = 0; start < x.size(); start += renderBlock)
    {
        if (cancel.load (std::memory_order_relaxed))
            return false;
        const auto len = (int) std::min ((size_t) renderBlock, x.size() - start);
        buffer.copyFrom (0, 0, x.data() + start, len);
        BlockContext context { di.data() + std::min (start, di.size()), len };
        block.process (juce::dsp::AudioBlock<float> (buffer).getSubBlock (0, (size_t) len), context);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + len, x.begin() + (std::ptrdiff_t) start);
    }
    return true;
}
} // namespace

std::vector<float> ToneMatcher::renderPedal (const Pedal& pedal, const std::vector<float>& di, const std::atomic<bool>& cancel)
{
    std::vector<float> out (di);
    // The chain's own blocks, settings in before prepare() (which snaps their smoothers), as ampsim_render sets them.
    switch (pedal.kind)
    {
        case Pedal::Kind::none:
            return out;
        case Pedal::Kind::compressor:
        {
            Compressor c (false);
            c.setSettings (pedal.compressor);
            c.prepare (sampleRate, renderBlock);
            return runBlock (c, out, di, cancel) ? out : std::vector<float>();
        }
        case Pedal::Kind::boost:
        {
            auto b = std::make_unique<Boost>();
            b->setSettings (pedal.boost);
            b->prepare (sampleRate, renderBlock);
            return runBlock (*b, out, di, cancel) ? out : std::vector<float>();
        }
        case Pedal::Kind::overdrive:
        {
            auto o = std::make_unique<Overdrive>();
            o->setSettings (pedal.overdrive);
            o->prepare (sampleRate, renderBlock);
            return runBlock (*o, out, di, cancel) ? out : std::vector<float>();
        }
    }
    return out;
}

bool ToneMatcher::compress (const Compressor::Settings& settings, std::vector<float>& x, const std::atomic<bool>& cancel)
{
    Compressor c (false);
    c.setSettings (settings);
    c.prepare (sampleRate, renderBlock);
    const auto context = x; // the post compressor's sidechain is its own input
    return runBlock (c, x, context, cancel);
}

double ToneMatcher::playingLevelDb (const std::vector<float>& x)
{
    // numpy's convolve(x^2, ones(2400) / 2400, "same"): the mean square over [n - 1200, n + 1200).
    const auto n = (int) x.size();
    std::vector<double> prefix ((size_t) n + 1, 0.0);
    for (int i = 0; i < n; ++i)
        prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) x[(size_t) i] * x[(size_t) i];
    std::vector<double> e;
    e.reserve ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const auto lo = juce::jlimit (0, n, i - 1200), hi = juce::jlimit (0, n, i + 1200);
        const auto v = std::sqrt (std::max (0.0, (prefix[(size_t) hi] - prefix[(size_t) lo]) / 2400.0));
        if (v > 1.0e-6)
            e.push_back (v);
    }
    return e.empty() ? -120.0 : 20.0 * std::log10 (percentile (std::move (e), 90.0));
}

std::vector<std::pair<Pedal, double>> ToneMatcher::pedalVariants (const std::vector<float>& di, double gainDb, const MatchSettings& settings)
{
    std::vector<std::pair<Pedal, double>> out;
    const Overdrive::Mode modes[] { Overdrive::Mode::midDrive, Overdrive::Mode::distortion, Overdrive::Mode::transparent, Overdrive::Mode::fuzz };
    for (auto mode : modes)
        for (auto drive : { 0.3f, 0.7f })
        {
            Pedal p;
            p.kind = Pedal::Kind::overdrive;
            p.overdrive.mode = mode;
            p.overdrive.drive = drive;
            p.overdrive.tone = 0.5f;
            p.overdrive.levelDb = 0.0f;
            p.overdrive.tightHz = DriveEngine::tightOffHz;
            p.overdrive.unityTrim = true;
            p.overdrive.oversampling = settings.pedalOversampling;
            p.overdrive.voltsAtFullScale = settings.voltsAtFullScale;
            out.push_back ({ p, gainDb });
            out.push_back ({ p, std::max (settings.gainMin, gainDb - 6.0) });
        }
    if (gainDb >= 18.0)
        for (auto level : { 6.0f, 12.0f })
        {
            // The Gain knob ends at +24 dB: a clean boost in front drives the amp further.
            Pedal p;
            p.kind = Pedal::Kind::boost;
            p.boost.mode = Boost::Mode::clean;
            p.boost.levelDb = level;
            p.boost.tiltDb = 0.0f;
            p.boost.unityTrim = true;
            p.boost.oversampling = settings.pedalOversampling;
            p.boost.voltsAtFullScale = settings.voltsAtFullScale;
            out.push_back ({ p, settings.gainMax });
        }
    const auto play = playingLevelDb (di);
    for (auto below : { 6.0, 12.0 })
    {
        Pedal p;
        p.kind = Pedal::Kind::compressor;
        auto& c = p.compressor;
        c.mode = Compressor::Mode::pedal;
        c.detector = Compressor::Detector::peak;
        c.thresholdDb = (float) (std::round ((play - below) * 10.0) / 10.0);
        c.ratio = 4.0f;
        c.attackMs = 2.0f;
        c.releaseMs = 200.0f;
        c.autoRelease = false;
        c.makeupDb = (float) (std::round (0.75 * below * 100.0) / 100.0);
        c.autoMakeup = false;
        c.mix = 1.0f;
        out.push_back ({ p, gainDb });
    }
    return out;
}

std::vector<float> ToneMatcher::renderAmp (const juce::File& model, const NamAmp::Calibration& calibration, const std::vector<float>& di,
                                           double gainDb, const std::atomic<bool>& cancel, const Pedal& pedal)
{
    // A private amp section, built here and dropped at the end: the chain's own amp block, the capture in
    // its first amp (selected), the other amps empty. Settings go in before prepare(), which snaps the
    // smoothers and installs the model with no fade, as ampsim_render does.
    // Cancelled already: return before loading the capture, and let the load itself stop at the flag. A load
    // measures the capture's loudness (a gain set's five steps: seconds of rendering), and before this every grid
    // job queued or running at a cancel paid for a whole one; on a two-thread CI runner that kept a cancelled
    // match going for over 5 s.
    if (cancel.load (std::memory_order_relaxed))
        return {};
    auto section = std::make_unique<AmpSection>();
    if (! section->amp (0).model.loadModel (model, true, calibration, &cancel).ok)
        return {};

    section->amp (0).inputTrim.setGainDecibels ((float) gainDb);
    section->prepare (sampleRate, renderBlock);

    auto out = renderPedal (pedal, di, cancel);
    if (out.empty() && ! di.empty())
        return {};
    return runBlock (*section, out, di, cancel) ? out : std::vector<float>();
}

std::vector<float> ToneMatcher::renderTone (const ToneSettings& settings, const std::vector<float>& di, const std::atomic<bool>& cancel)
{
    if (cancel.load (std::memory_order_relaxed))
        return {}; // before the capture loads (as renderAmp)

    // The pedal in front, then the amp: a private AmpSection, settings in before prepare(), which snaps every
    // smoother (as renderAmp).
    auto out = di;
    for (const auto& pedal : settings.pedals)
    {
        out = renderPedal (pedal, out, cancel);
        if (cancel.load() || out.empty())
            return {};
    }
    if (settings.ampOn && settings.model != juce::File())
    {
        auto section = std::make_unique<AmpSection>();
        auto& slot = section->amp (0);
        if (! slot.model.loadModel (settings.model, true, settings.calibration, &cancel).ok)
            return {};
        slot.inputTrim.setGainDecibels (settings.gainDb);
        // To the nearest 0.01 dB, exactly as the processor sets them, so a centred knob is exactly flat.
        for (int b = 0; b < AmpTone::numBands; ++b)
            slot.tone.setGainDb ((AmpTone::Band) b, std::round (settings.tone[(size_t) b] * 100.0f) / 100.0f);
        slot.outputTrim.setGainDecibels (settings.masterDb);
        section->prepare (sampleRate, renderBlock);
        if (! runBlock (*section, out, di, cancel))
            return {};
    }

    // The cab: close mic 1's IR, convolved (exact linear convolution, as the cab block's partitioned one).
    if (! settings.cabIR.empty())
        out = convolve (out, settings.cabIR);
    if (cancel.load())
        return {};

    // The match curve: its minimum-phase FIR (the block's output is exactly this convolution), after the cab.
    if (settings.matchCurveOn)
        if (const auto fir = MatchCurve::designFir (settings.matchCurve, settings.matchCurveAmount); ! fir.empty())
            out = convolve (out, fir);
    if (cancel.load())
        return {};

    // The post EQ: the chain's own block, mono, settings in before prepare() (which snaps them).
    if (settings.postEqOn)
    {
        Equalizer eq (false);
        eq.setSettings (settings.postEq);
        eq.prepare (sampleRate, renderBlock);
        if (! runBlock (eq, out, di, cancel))
            return {};
    }
    // The post compressor, after the post EQ.
    if (settings.postCompressor.on && ! compress (settings.postCompressor.settings, out, cancel))
        return {};
    return out;
}

ToneSettings ToneMatcher::settingsFor (const MatchResult& r, const MatchSettings& settings)
{
    ToneSettings t;
    t.model = r.model != juce::File() ? r.model : settings.models[(size_t) juce::jlimit (0, (int) settings.models.size() - 1, r.slot)];
    t.calibration = settings.calibration;
    t.gainDb = (float) r.gainDb;
    for (size_t b = 0; b < 5; ++b)
        t.tone[b] = (float) r.tone[b];
    t.cabIR = irAsPlayed (r.cab);
    t.matchCurveOn = ! r.matchCurve.points.empty();
    t.matchCurve = r.matchCurve;
    t.matchCurveAmount = r.matchCurveAmountPercent / 100.0;
    t.postEqOn = r.usesMatchEq();
    t.postEq.mode = Equalizer::Mode::parametric;
    for (size_t b = 0; b < r.eq.size(); ++b)
        t.postEq.bands[b] = r.eq[b];
    t.postEq.lowCut.on = false;
    t.postEq.highCut.on = false;
    if (r.pedal.kind != Pedal::Kind::none)
        t.pedals.push_back (r.pedal);
    t.postCompressor = r.postCompressor;
    return t;
}

std::vector<double> ToneMatcher::linearPowerOnBins (const std::array<double, 5>& tone,
                                                    const std::array<Equalizer::Band, Equalizer::numParametricBands>* eq)
{
    std::vector<double> h ((size_t) numBins, 1.0);
    const auto apply = [&h] (const Svf::Coefficients& c) {
        for (int k = 0; k < numBins; ++k)
            h[(size_t) k] *= std::norm (Svf::responseAt (c, k * sampleRate / fftSize, sampleRate));
    };
    for (size_t band = 0; band < AmpTone::numBands; ++band)
        if (tone[band] != 0.0)
            apply (Svf::design (AmpTone::bands[band].type, AmpTone::bands[band].frequency, AmpTone::bands[band].q, tone[band], sampleRate));
    if (eq != nullptr)
        for (const auto& b : *eq)
            if (b.gainDb != 0.0f)
                apply (Svf::design (b.type == Equalizer::BandType::lowShelf    ? Svf::Type::lowShelf
                                    : b.type == Equalizer::BandType::highShelf ? Svf::Type::highShelf
                                    : b.type == Equalizer::BandType::notch     ? Svf::Type::notch
                                                                               : Svf::Type::peak,
                                    b.frequency, b.q, b.gainDb, sampleRate));
    return h;
}

MatchCurve::Curve ToneMatcher::fitMatchCurve (const Analysis& target, const std::vector<double>& candidateBins)
{
    const auto n = numBins;
    const auto dbOf = [] (double p) { return 10.0 * std::log10 (std::max (p, 1.0e-20)); };
    std::vector<double> f ((size_t) n), lf ((size_t) n), tDb ((size_t) n), d ((size_t) n);
    for (int k = 0; k < n; ++k)
    {
        f[(size_t) k] = k * sampleRate / fftSize;
        lf[(size_t) k] = std::log2 (std::max (f[(size_t) k], 1.0));
        tDb[(size_t) k] = dbOf (target.ltasBins[(size_t) k]);
        d[(size_t) k] = tDb[(size_t) k] - dbOf (candidateBins[(size_t) k]);
    }
    // A Gaussian of 1/12 octave (sigma) on log frequency, over +-3 sigma (numpy's searchsorted, left), bin 0 kept.
    const auto smooth = [&] (const std::vector<double>& x) {
        const double frac = 12.0;
        std::vector<double> out (x);
        for (int i = 1; i < n; ++i)
        {
            const auto lo = (int) (std::lower_bound (lf.begin(), lf.end(), lf[(size_t) i] - 3.0 / frac) - lf.begin());
            const auto hi = (int) (std::lower_bound (lf.begin(), lf.end(), lf[(size_t) i] + 3.0 / frac) - lf.begin());
            double num = 0.0, den = 0.0;
            for (int j = lo; j < hi; ++j)
            {
                const auto z = (lf[(size_t) j] - lf[(size_t) i]) * frac;
                const auto w = std::exp (-0.5 * z * z);
                num += w * x[(size_t) j];
                den += w;
            }
            out[(size_t) i] = num / den;
        }
        return out;
    };
    const auto ds = smooth (d), ts = smooth (tDb);
    double loudest = -1.0e300;
    for (int k = 0; k < n; ++k)
        if (f[(size_t) k] >= 60.0 && f[(size_t) k] <= 14000.0)
            loudest = std::max (loudest, ts[(size_t) k]);
    std::vector<double> conf ((size_t) n, 0.0);
    double num = 0.0, den = 0.0;
    for (int k = 0; k < n; ++k)
        if (f[(size_t) k] >= 60.0 && f[(size_t) k] <= 14000.0)
        {
            conf[(size_t) k] = juce::jlimit (minConfidence, 1.0, (ignoredDb - (loudest - ts[(size_t) k])) / (ignoredDb - confidentDb));
            num += ds[(size_t) k] * conf[(size_t) k];
            den += conf[(size_t) k];
        }
    const auto mean = num / den;
    std::vector<double> curve ((size_t) n);
    for (int k = 0; k < n; ++k)
        curve[(size_t) k] = juce::jlimit (-eqCapDb, eqCapDb, (ds[(size_t) k] - mean) * conf[(size_t) k]);

    std::vector<MatchCurve::Point> points;
    const auto count = (int) std::floor (48.0 * std::log2 (16000.0 / 40.0)) + 1;
    for (int i = 0; i < count; ++i)
    {
        const auto hz = 40.0 * std::pow (2.0, i / 48.0);
        // np.interp on the bins (linear in Hz).
        const auto pos = hz * fftSize / sampleRate;
        const auto k = juce::jlimit (0, n - 2, (int) std::floor (pos));
        const auto frac = pos - k;
        points.push_back ({ hz, curve[(size_t) k] + (curve[(size_t) k + 1] - curve[(size_t) k]) * frac });
    }
    return MatchCurve::Curve::fromPoints (std::move (points));
}

std::vector<float> ToneMatcher::irAsPlayed (const juce::File& file)
{
    CabIR mic;
    if (! mic.loadFile (file).ok)
        return {};
    return mic.getLoadedIR();
}

std::vector<float> ToneMatcher::loadIR (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return {};

    const auto maxLength = (juce::int64) std::round (CabIR::maxIRSeconds * reader->sampleRate);
    const auto length = (int) std::min (reader->lengthInSamples, maxLength);
    juce::AudioBuffer<float> buffer ((int) reader->numChannels, length);
    reader->read (&buffer, 0, length, 0, true, reader->numChannels > 1);
    return std::vector<float> (buffer.getReadPointer (0), buffer.getReadPointer (0) + length);
}

std::vector<float> ToneMatcher::convolve (const std::vector<float>& x, const std::vector<float>& ir)
{
    // Overlap-add: blocks of L samples, each zero-padded to the FFT size F >= L + len(ir) - 1, multiplied
    // by the IR's spectrum, and summed back into place. Exact linear convolution up to float rounding.
    std::vector<float> y (x.size(), 0.0f);
    if (x.empty() || ir.empty())
        return y;

    int order = 1;
    while ((1 << order) < 2 * (int) ir.size())
        ++order;
    order = juce::jmax (order, 14);
    const auto F = 1 << order;
    const auto L = F - (int) ir.size() + 1;

    juce::dsp::FFT fft (order);
    std::vector<float> h ((size_t) F * 2, 0.0f), work ((size_t) F * 2);
    std::copy (ir.begin(), ir.end(), h.begin());
    fft.performRealOnlyForwardTransform (h.data(), true);

    for (size_t start = 0; start < x.size(); start += (size_t) L)
    {
        const auto len = std::min ((size_t) L, x.size() - start);
        std::fill (work.begin(), work.end(), 0.0f);
        std::copy (x.begin() + (std::ptrdiff_t) start, x.begin() + (std::ptrdiff_t) (start + len), work.begin());
        fft.performRealOnlyForwardTransform (work.data(), true);
        for (int k = 0; k <= F / 2; ++k)
        {
            const std::complex<float> a (work[(size_t) (2 * k)], work[(size_t) (2 * k + 1)]);
            const std::complex<float> b (h[(size_t) (2 * k)], h[(size_t) (2 * k + 1)]);
            const auto c = a * b;
            work[(size_t) (2 * k)] = c.real();
            work[(size_t) (2 * k + 1)] = c.imag();
        }
        fft.performRealOnlyInverseTransform (work.data()); // JUCE scales the inverse by 1/F
        for (size_t i = 0; i < (size_t) F && start + i < y.size(); ++i)
            y[start + i] += work[i];
    }
    return y;
}

std::vector<double> ToneMatcher::cabPowerOnBins (const std::vector<float>& ir)
{
    // |FFT(ir, n)|^2 with n a power of two >= max(fftSize, len(ir)); n is a multiple of fftSize, so the
    // analysis bins are every (n / fftSize)-th of its bins (the prototype's np.interp lands on them exactly).
    int order = fftOrder;
    while ((1 << order) < (int) ir.size())
        ++order;
    const auto n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> work ((size_t) n * 2, 0.0f);
    std::copy (ir.begin(), ir.end(), work.begin());
    fft.performRealOnlyForwardTransform (work.data(), true);
    const auto ratio = n / fftSize;
    std::vector<double> out ((size_t) numBins);
    for (int k = 0; k < numBins; ++k)
    {
        const double re = work[(size_t) (2 * k * ratio)], im = work[(size_t) (2 * k * ratio + 1)];
        out[(size_t) k] = re * re + im * im;
    }
    return out;
}

ToneMatcher::Candidate ToneMatcher::score (int slot, double gainDb, int cab, const std::vector<float>& ampOutput, const std::vector<float>& ir,
                                           const Analysis& target, Mode mode, const Alignment* alignment, bool takeLtasBins)
{
    Candidate c;
    c.slot = slot;
    c.gainDb = gainDb;
    c.cab = cab;
    const auto y = convolve (ampOutput, ir); // exactly what the cab does, up to its level
    c.analysis = Analysis::of (y);
    if (takeLtasBins)
        c.takeLtasBins = take::ltasBins (y);
    if (! c.analysis.ok)
    {
        c.spectral = c.distortion = 1.0e9;
        return c;
    }
    c.distortion = nonlinearDistance (target.features, c.analysis.features);

    if (mode == Mode::samePart && alignment != nullptr)
    {
        const auto d = alignedDifference (target, c.analysis, *alignment);
        c.residual = d.residual;
        c.distortion += d.envelope / envelopeScaleDb;
    }
    else
    {
        std::vector<double> r ((size_t) numBands);
        for (size_t b = 0; b < r.size(); ++b)
            r[b] = target.ltas[b] - c.analysis.ltas[b];
        c.residual = smoothBands (r);
    }

    std::tie (c.tone, c.spectral) = fitTone (c.residual, target.weights, false);
    return c;
}

// ---- The search -------------------------------------------------------------------------------------------

MatchResult ToneMatcher::match (const std::vector<float>& targetSignal, const std::vector<float>& reference, const MatchSettings& settings,
                                const std::atomic<bool>& cancel, const ProgressFn& progress)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    MatchResult result;
    result.mode = settings.mode;
    const auto threads = settings.threads > 0 ? settings.threads : defaultThreads();

    auto report = [&] (double fraction, const juce::String& stage) {
        if (progress)
            progress (juce::jlimit (0.0, 1.0, fraction), stage);
    };
    auto cancelled = [&] {
        if (! cancel.load())
            return false;
        result.cancelled = true;
        result.error = "Cancelled";
        return true;
    };

    report (0.0, "Analysing the target");
    const auto target = Analysis::of (targetSignal);
    if (cancelled())
        return result;
    if (! target.ok)
    {
        result.error = "The target has too little playing in it to analyse.";
        return result;
    }

    std::vector<int> slots;
    for (int s = 0; s < (int) settings.models.size(); ++s)
        if (settings.models[(size_t) s] != juce::File())
            slots.push_back (s);
    if (slots.empty() || settings.cabs.empty())
    {
        result.error = "Nothing to search: no captures or no cabs.";
        return result;
    }

    std::vector<std::vector<float>> irs;
    std::vector<std::vector<double>> cabBins;
    for (const auto& f : settings.cabs)
    {
        if (cancelled())
            return result; // a dozen IRs to read and transform: check between them
        irs.push_back (loadIR (f));
        cabBins.push_back (cabPowerOnBins (irs.back()));
    }

    // A play-along take: its notes paired with the target's, first, because with enough of them the take-aware score
    // decides and the search looks wider (MatchSettings::takeRefinesEverySlot, takePedalSlotsByPre, takePreShortlist).
    take::TakeNotes takeNotes;
    if (settings.takeIsLinedUp)
    {
        report (0.01, "Pairing your notes with the target's");
        const auto bandFrames = (int) std::ceil (playAlongBandSeconds * sampleRate / hop);
        takeNotes = take::TakeNotes::of (targetSignal, informed::alignNotes (reference, targetSignal, bandFrames, cancel));
        if (cancelled())
            return result;
    }
    const auto takeAware = takeNotes.usable();

    // Renders, keyed by (slot, Gain in thousandths of a dB, pedal: an index into `pedals`, -1 for none).
    std::map<std::tuple<int, int, int>, std::vector<float>> renders;
    // With a take: each render's note measures (take::noteMeasures at the take's onsets), for S and S_pre.
    std::map<std::tuple<int, int, int>, take::NoteMeasures> noteMeasuresOf;
    std::mutex noteMeasuresLock;
    auto key = [] (int s, double g, int p = -1) { return std::make_tuple (s, (int) std::lround (g * 1000.0), p); };
    std::atomic<bool> renderFailed { false };
    std::vector<Pedal> pedals;

    auto renderAll = [&] (const std::vector<std::pair<int, double>>& jobs, double fromFraction, double toFraction,
                          const std::vector<int>& pedalOf = {}) {
        std::vector<std::vector<float>> outs (jobs.size());
        std::atomic<int> done { 0 };
        parallelFor ((int) jobs.size(), threads, [&] (int i, int) {
            const auto p = pedalOf.empty() ? -1 : pedalOf[(size_t) i];
            outs[(size_t) i] = renderAmp (settings.models[(size_t) jobs[(size_t) i].first], settings.calibration, reference,
                                          jobs[(size_t) i].second, cancel, p >= 0 ? pedals[(size_t) p] : Pedal {});
            if (outs[(size_t) i].empty() && ! cancel.load())
                renderFailed = true;
            report (fromFraction + (toFraction - fromFraction) * ++done / (double) jobs.size(),
                    p >= 0 ? "Trying pedals in front of the amp" : "Rendering your DI through the amps");
        });
        for (size_t i = 0; i < jobs.size(); ++i)
            renders[key (jobs[i].first, jobs[i].second, pedalOf.empty() ? -1 : pedalOf[i])] = std::move (outs[i]);
    };

    // 1. The grid.
    std::vector<std::pair<int, double>> grid;
    for (auto s : slots)
        for (auto g : settings.gainGrid)
            grid.push_back ({ s, g });
    renderAll (grid, 0.02, 0.45);
    if (cancelled())
        return result;
    if (renderFailed)
    {
        result.error = "A capture failed to load.";
        return result;
    }

    // Same part: align the target with each slot's render at 0 dB, once.
    std::map<int, Alignment> alignments;
    if (settings.mode == Mode::samePart)
    {
        report (0.46, "Aligning the two performances");
        const auto bandFrames = settings.alignmentBandSeconds > 0.0 ? (int) std::ceil (settings.alignmentBandSeconds * sampleRate / hop) : 0;
        for (auto s : slots)
        {
            alignments[s] = align (target, Analysis::of (renders.at (key (s, 0.0))), bandFrames);
            if (cancelled())
                return result;
        }
    }

    // 2. Candidates.
    std::map<std::tuple<int, int, int, int>, Candidate> candidates;
    std::mutex candidatesLock;

    auto screen = [&] (int slot, double gain) {
        const auto& y = renders.at (key (slot, gain));
        const auto ya = Analysis::of (y);
        std::vector<double> ref ((size_t) numBands);
        if (settings.mode == Mode::samePart)
        {
            const auto& path = alignments.at (slot).path;
            std::vector<double> sum ((size_t) numBands, 0.0);
            int pairs = 0;
            for (const auto& [i, j] : path)
            {
                if (! target.active[(size_t) i] || ! ya.active[(size_t) j])
                    continue;
                for (int b = 0; b < numBands; ++b)
                    sum[(size_t) b] += target.frame (i)[b] - ya.frame (j)[b];
                ++pairs;
            }
            for (int b = 0; b < numBands; ++b)
                ref[(size_t) b] = sum[(size_t) b] / juce::jmax (1, pairs) + ya.ltas[(size_t) b];
        }
        else
            ref = target.ltas;

        std::vector<std::pair<double, int>> scores;
        for (size_t c = 0; c < irs.size(); ++c)
        {
            std::vector<double> r ((size_t) numBands);
            const auto& bd = bands();
            for (int b = 0; b < numBands; ++b)
            {
                double s = 0.0;
                for (int k = bd.first[(size_t) b]; k < bd.last[(size_t) b]; ++k)
                    s += ya.ltasBins[(size_t) k] * cabBins[c][(size_t) k];
                const auto predicted = 10.0 * std::log10 (std::max (s / (bd.last[(size_t) b] - bd.first[(size_t) b]), 1.0e-20));
                r[(size_t) b] = ref[(size_t) b] - predicted;
            }
            scores.push_back ({ fitTone (smoothBands (r), target.weights, false).second, (int) c });
        }
        std::stable_sort (scores.begin(), scores.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<int> out;
        for (int i = 0; i < juce::jmin (settings.cabsPerAmp, (int) scores.size()); ++i)
            out.push_back (scores[(size_t) i].second);
        return out;
    };

    auto evaluate = [&] (const std::vector<std::tuple<int, double, std::vector<int>>>& jobs, double fromFraction, double toFraction,
                         int pedalIndex = -1, const std::vector<int>& pedalOf = {}) {
        // Expand (slot, Gain, cabs or empty for "screen first") into one job per candidate. The screens
        // run in parallel; the maps are only read while the workers run.
        std::vector<std::vector<int>> chosenCabs (jobs.size());
        parallelFor ((int) jobs.size(), threads, [&] (int i, int) {
            if (cancel.load())
                return; // the result is discarded anyway
            const auto& [s, g, cabs] = jobs[(size_t) i];
            chosenCabs[(size_t) i] = cabs.empty() ? screen (s, g) : cabs;
            if (takeAware)
            {
                const auto k = key (s, g, pedalOf.empty() ? pedalIndex : pedalOf[(size_t) i]);
                {
                    const std::lock_guard<std::mutex> lock (noteMeasuresLock); // worker threads only, never audio
                    if (noteMeasuresOf.count (k) > 0)
                        return;
                }
                auto m = take::noteMeasures (renders.at (k), takeNotes.takeOnsets, takeNotes.lengths);
                const std::lock_guard<std::mutex> lock (noteMeasuresLock);
                noteMeasuresOf[k] = std::move (m);
            }
        });
        std::vector<std::tuple<int, double, int, int>> work;
        for (size_t j = 0; j < jobs.size(); ++j)
        {
            const auto& [s, g, cabs] = jobs[j];
            const auto p = pedalOf.empty() ? pedalIndex : pedalOf[j];
            for (auto c : chosenCabs[j])
                if (candidates.find ({ s, (int) std::lround (g * 1000.0), c, p }) == candidates.end())
                    work.push_back ({ s, g, c, p });
        }
        std::atomic<int> done { 0 };
        parallelFor ((int) work.size(), threads, [&] (int i, int) {
            if (cancel.load())
                return;
            const auto [s, g, c, p] = work[(size_t) i];
            auto cand = score (s, g, c, renders.at (key (s, g, p)), irs[(size_t) c], target, settings.mode,
                               settings.mode == Mode::samePart ? &alignments.at (s) : nullptr, takeAware);
            cand.pedal = p;
            if (takeAware)
                cand.pre = take::scoreOf (takeNotes, target, cand.analysis.features[1], cand.analysis.features[3], noteMeasuresOf.at (key (s, g, p)),
                                          cand.takeLtasBins, cand.tone, nullptr, cand.spectral)
                               .total;
            {
                const std::lock_guard<std::mutex> lock (candidatesLock); // worker threads only, never audio
                candidates[{ s, (int) std::lround (g * 1000.0), c, p }] = std::move (cand);
            }
            report (fromFraction + (toFraction - fromFraction) * ++done / (double) work.size(), "Trying cabs and tone");
        });
    };

    {
        std::vector<std::tuple<int, double, std::vector<int>>> jobs;
        for (const auto& [s, g] : grid)
            jobs.push_back ({ s, g, {} });
        evaluate (jobs, 0.47, 0.70);
    }
    if (cancelled())
        return result;

    // 3. Gain, coarse to fine, for the best slots, with each slot's best cabs.
    //    With a take, every slot (MatchSettings::takeRefinesEverySlot): the slots the old score ranks low are often
    //    the take-aware score's best.
    auto bestFor = [&] (int slot, bool byPre = false) -> const Candidate* {
        const Candidate* best = nullptr;
        for (const auto& [k, c] : candidates)
            if (c.slot == slot && c.pedal < 0 && (best == nullptr || (byPre ? c.pre < best->pre : c.total() < best->total())))
                best = &c;
        return best;
    };

    auto slotOrder = slots;
    std::stable_sort (slotOrder.begin(), slotOrder.end(), [&] (int a, int b) { return bestFor (a)->total() < bestFor (b)->total(); });
    const auto refineCount = takeAware && settings.takeRefinesEverySlot ? (int) slotOrder.size()
                                                                        : juce::jmin (settings.refineSlots, (int) slotOrder.size());
    double fraction = 0.70;
    const auto refineEnd = settings.searchPedals ? 0.80 : 0.92;
    const auto fractionStep = (refineEnd - 0.70) / juce::jmax (1, refineCount * (int) settings.refineSteps.size());

    for (int r = 0; r < refineCount; ++r)
    {
        const auto s = slotOrder[(size_t) r];
        std::map<int, double> bestPerCab;
        for (const auto& [k, c] : candidates)
            if (c.slot == s)
            {
                auto it = bestPerCab.find (c.cab);
                if (it == bestPerCab.end() || c.total() < it->second)
                    bestPerCab[c.cab] = c.total();
            }
        std::vector<std::pair<double, int>> order;
        for (const auto& [cab, t] : bestPerCab)
            order.push_back ({ t, cab });
        std::stable_sort (order.begin(), order.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<int> cabs;
        for (int i = 0; i < juce::jmin (settings.cabsPerAmp, (int) order.size()); ++i)
            cabs.push_back (order[(size_t) i].second);

        for (auto step : settings.refineSteps)
        {
            const auto g0 = bestFor (s)->gainDb;
            std::vector<std::pair<int, double>> jobs;
            for (auto g : { g0 - step, g0 + step })
                if (g >= settings.gainMin && g <= settings.gainMax && renders.find (key (s, g)) == renders.end())
                    jobs.push_back ({ s, g });
            renderAll (jobs, fraction, fraction + fractionStep * 0.6);
            if (cancelled())
                return result;
            std::vector<std::tuple<int, double, std::vector<int>>> evalJobs;
            for (auto g : { g0 - step, g0 + step })
                if (g >= settings.gainMin && g <= settings.gainMax)
                    evalJobs.push_back ({ s, g, cabs });
            evaluate (evalJobs, fraction + fractionStep * 0.6, fraction + fractionStep);
            fraction += fractionStep;
            if (cancelled())
                return result;
        }
    }

    // 3b. The pedals in front of the best amps, at their best cabs (pedalVariants; prototypes/tone_match.py, the
    //     pedal search): the pedalSlots best by the old score and, with a take, the takePedalSlotsByPre best by S_pre.
    //     They join the candidates, so the scores decide whether one is used at all.
    if (settings.searchPedals)
    {
        auto byOld = slots;
        std::stable_sort (byOld.begin(), byOld.end(), [&] (int a, int b) { return bestFor (a)->total() < bestFor (b)->total(); });
        std::vector<int> pedalSlots (byOld.begin(), byOld.begin() + juce::jlimit (0, (int) byOld.size(), settings.pedalSlots));
        if (takeAware)
        {
            auto byPre = slots;
            std::stable_sort (byPre.begin(), byPre.end(), [&] (int a, int b) { return bestFor (a, true)->pre < bestFor (b, true)->pre; });
            for (int i = 0; i < juce::jmin (settings.takePedalSlotsByPre, (int) byPre.size()); ++i)
                if (std::find (pedalSlots.begin(), pedalSlots.end(), byPre[(size_t) i]) == pedalSlots.end())
                    pedalSlots.push_back (byPre[(size_t) i]);
        }

        const auto span = 0.13 / juce::jmax (1, (int) pedalSlots.size());
        for (size_t n = 0; n < pedalSlots.size(); ++n)
        {
            const auto slot = pedalSlots[n];
            const auto gain = bestFor (slot)->gainDb;
            std::map<int, double> bestPerCab;
            for (const auto& [k, c] : candidates)
                if (c.slot == slot)
                {
                    auto it = bestPerCab.find (c.cab);
                    if (it == bestPerCab.end() || c.total() < it->second)
                        bestPerCab[c.cab] = c.total();
                }
            std::vector<std::pair<double, int>> order;
            for (const auto& [cab, t] : bestPerCab)
                order.push_back ({ t, cab });
            std::stable_sort (order.begin(), order.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
            std::vector<int> cabs;
            for (int i = 0; i < juce::jmin (settings.cabsPerAmp, (int) order.size()); ++i)
                cabs.push_back (order[(size_t) i].second);

            const auto variants = pedalVariants (reference, gain, settings);
            std::vector<std::pair<int, double>> jobs;
            std::vector<int> pedalOf;
            std::vector<std::tuple<int, double, std::vector<int>>> evalJobs;
            for (const auto& [pedal, g] : variants)
            {
                pedalOf.push_back ((int) pedals.size());
                pedals.push_back (pedal);
                jobs.push_back ({ slot, g });
                evalJobs.push_back ({ slot, g, cabs });
            }
            const auto from = 0.80 + span * (double) n;
            renderAll (jobs, from, from + span * 0.8, pedalOf);
            if (cancelled())
                return result;
            if (renderFailed)
            {
                result.error = "A capture failed to load.";
                return result;
            }
            evaluate (evalJobs, from + span * 0.8, from + span, -1, pedalOf);
            if (cancelled())
                return result;
        }
    }

    // 4. The best candidate: polish its tone, fit the match EQ. With a play-along take, the shortlist is fitted
    //    completely and the take-aware score (TakeScore.h) picks among it.
    report (0.93, "Fitting the match EQ");
    std::vector<const Candidate*> ranked;
    for (const auto& [k, c] : candidates)
        ranked.push_back (&c);
    std::stable_sort (ranked.begin(), ranked.end(), [] (const Candidate* a, const Candidate* b) { return a->total() < b->total(); });

    struct Finished
    {
        std::array<double, 5> tone {};
        double spectral = 0.0;
        std::vector<double> residual, eqTarget;
        std::array<Equalizer::Band, Equalizer::numParametricBands> eq {};
        take::Score score;
    };
    const auto finish = [&] (const Candidate& c) {
        Finished f;
        std::tie (f.tone, f.spectral) = fitTone (c.residual, target.weights, true);
        const auto tone = toneDb (f.tone);
        f.residual.resize ((size_t) numBands);
        for (size_t b = 0; b < (size_t) numBands; ++b)
            f.residual[b] = c.residual[b] - tone[b];
        std::tie (f.eq, f.eqTarget) = fitMatchEq (f.residual, target.weights);
        return f;
    };

    const Candidate* chosen = ranked.front();
    Finished fin;
    if (takeAware)
    {
        // The shortlist (prototypes/tone_match.py, shortlist): the old score's take::shortlist best and S_pre's
        // takePreShortlist best, in the old score's order, each fitted completely and scored by S.
        auto byPre = ranked;
        std::stable_sort (byPre.begin(), byPre.end(), [] (const Candidate* a, const Candidate* b) { return a->pre < b->pre; });
        const std::set<const Candidate*> keep (byPre.begin(), byPre.begin() + juce::jlimit (0, (int) byPre.size(), settings.takePreShortlist));
        std::vector<const Candidate*> shortlisted;
        for (size_t i = 0; i < ranked.size(); ++i)
            if ((int) i < take::shortlist || keep.count (ranked[i]) > 0)
                shortlisted.push_back (ranked[i]);
        const auto count = (int) shortlisted.size();
        std::vector<Finished> fins ((size_t) count);
        parallelFor (count, threads, [&] (int i, int) {
            if (cancel.load())
                return;
            const auto& c = *shortlisted[(size_t) i];
            fins[(size_t) i] = finish (c);
            fins[(size_t) i].score = take::scoreOf (takeNotes, target, c.analysis.features[1], c.analysis.features[3],
                                                    noteMeasuresOf.at (key (c.slot, c.gainDb, c.pedal)), c.takeLtasBins, fins[(size_t) i].tone,
                                                    &fins[(size_t) i].eq, c.spectral);
        });
        if (cancelled())
            return result;
        std::vector<int> order ((size_t) count);
        std::iota (order.begin(), order.end(), 0);
        std::stable_sort (order.begin(), order.end(), [&] (int a, int b) { return fins[(size_t) a].score.total < fins[(size_t) b].score.total; });
        std::vector<const Candidate*> reranked;
        for (auto i : order)
            reranked.push_back (shortlisted[(size_t) i]);
        const std::set<const Candidate*> scored (shortlisted.begin(), shortlisted.end());
        for (auto* c : ranked)
            if (scored.count (c) == 0)
                reranked.push_back (c);
        chosen = reranked.front();
        fin = std::move (fins[(size_t) order.front()]);
        ranked = std::move (reranked);
        result.takeScored = true;
        result.notePairs = (int) takeNotes.takeOnsets.size();
        result.takeScore = fin.score;
        result.shortlisted = count;
    }
    else
        fin = finish (*chosen);
    const auto& best = *chosen;

    result.tone = fin.tone;
    result.spectralErrorDb = fin.spectral;
    result.residual = fin.residual;
    result.eq = fin.eq;
    result.eqTarget = fin.eqTarget;
    const auto eqCurve = eqDb (result.eq);
    std::vector<double> after ((size_t) numBands);
    for (size_t b = 0; b < (size_t) numBands; ++b)
        after[b] = result.residual[b] - eqCurve[b];
    result.spectralErrorAfterEqDb = weightedRmsCentred (after, target.weights);

    result.slot = best.slot;
    result.model = settings.models[(size_t) best.slot];
    result.gainDb = best.gainDb;
    if (best.pedal >= 0)
        result.pedal = pedals[(size_t) best.pedal];
    result.cab = settings.cabs[(size_t) best.cab];
    result.distortion = best.distortion;
    result.closeness = 100.0 * std::exp (-(result.spectralErrorAfterEqDb + lambda * best.distortion) / scoreScaleDb);
    result.targetFeatures = target.features;
    result.weights = target.weights;
    result.resultFeatures = best.analysis.features;
    result.renders = (int) renders.size();
    result.candidates = (int) candidates.size();
    for (size_t i = 1; i < juce::jmin ((size_t) 4, ranked.size()); ++i)
        result.runnersUp.push_back ({ ranked[i]->slot, ranked[i]->gainDb, settings.cabs[(size_t) ranked[i]->cab], ranked[i]->total() });
    if (settings.mode == Mode::samePart)
        result.alignmentPath = alignments.at (best.slot).path;

    // 4b. The match curve instead of the match EQ (Round 2, item 5): fitted to what's left after the tone knobs (the
    //     winner's take render through its cab, times the tone's |H|^2, against the target), played at the amount.
    if (settings.fitCurve && result.takeScored)
    {
        result.matchCurve = fitMatchCurve (target, [&] {
            auto bins = best.analysis.ltasBins;
            const auto h = linearPowerOnBins (result.tone, nullptr);
            for (size_t k = 0; k < bins.size(); ++k)
                bins[k] *= h[k];
            return bins;
        }());
        result.matchCurveAmountPercent = settings.matchCurveAmountPercent;
        for (auto& b : result.eq)
            b.gainDb = 0.0f;
        result.spectralErrorAfterEqDb = weightedRmsCentred (result.residual, target.weights);
    }

    // 5. The post compressor on the finished match (prototypes/tone_match.py, search_post_comp): the take through
    //    everything as the chain plays it (renderTone), then through the compressor 4 and 8 dB under that render's
    //    playing level (studio mode, RMS, 3:1, 10 ms, 150 ms), each scored with the take-aware score on the whole
    //    render against the same score without it; the lowest wins. The threshold is absolute, so it's set from the
    //    render at the level the chain plays it, which is what Apply sets.
    if (settings.searchPedals && result.takeScored && settings.mode == Mode::anything)
    {
        report (0.97, "Trying the post compressor");
        auto tone = settingsFor (result, settings);
        const auto base = renderTone (tone, reference, cancel);
        if (cancelled() || base.empty())
        {
            if (! result.cancelled)
                result.error = "A capture failed to load.";
            return result;
        }
        const auto fullScore = [&] (const std::vector<float>& y) {
            const auto ya = Analysis::of (y);
            std::vector<double> r ((size_t) numBands);
            for (size_t b = 0; b < r.size(); ++b)
                r[b] = target.ltas[b] - ya.ltas[b];
            return take::score (takeNotes, target, y, y, {}, nullptr, weightedRmsCentred (smoothBands (r), target.weights)).total;
        };
        const auto play = playingLevelDb (base);
        std::vector<Compressor::Settings> comps;
        for (auto below : { 4.0, 8.0 })
        {
            Compressor::Settings c;
            c.mode = Compressor::Mode::studio;
            c.detector = Compressor::Detector::rms;
            c.thresholdDb = (float) (std::round ((play - below) * 100.0) / 100.0);
            c.ratio = 3.0f;
            c.attackMs = 10.0f;
            c.releaseMs = 150.0f;
            c.autoRelease = false;
            c.makeupDb = 0.0f;
            c.autoMakeup = false;
            c.mix = 1.0f;
            comps.push_back (c);
        }
        std::vector<double> scores ((size_t) comps.size() + 1);
        scores[0] = fullScore (base);
        parallelFor ((int) comps.size(), threads, [&] (int i, int) {
            auto y = base;
            if (compress (comps[(size_t) i], y, cancel))
                scores[(size_t) i + 1] = fullScore (y);
        });
        if (cancelled())
            return result;
        const auto k = (size_t) std::distance (scores.begin(), std::min_element (scores.begin(), scores.end()));
        result.postCompressorScores = scores;
        if (k > 0)
        {
            result.postCompressor.on = true;
            result.postCompressor.settings = comps[k - 1];
        }
    }
    result.runtimeSeconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    result.ok = true;
    report (1.0, "Done");
    return result;
}

} // namespace ampsim::tonematch
