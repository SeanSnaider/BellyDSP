// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "ToneMatcher.h"

#include "../dsp/AmpSection.h"
#include "../dsp/CabIR.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <map>
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

std::vector<float> ToneMatcher::renderAmp (const juce::File& model, const NamAmp::Calibration& calibration, const std::vector<float>& di,
                                           double gainDb, const std::atomic<bool>& cancel)
{
    // A private amp section, built here and dropped at the end: the chain's own amp block, the capture in
    // its first slot (selected), the other slots empty. Settings go in before prepare(), which snaps the
    // smoothers and installs the model with no fade, as ampsim_render does.
    auto section = std::make_unique<AmpSection>();
    if (! section->slot (0).model.loadModel (model, true, calibration).ok)
        return {};

    section->slot (0).inputTrim.setGainDecibels ((float) gainDb);
    section->prepare (sampleRate, renderBlock);

    std::vector<float> out (di.size());
    juce::AudioBuffer<float> buffer (1, renderBlock);
    for (size_t start = 0; start < di.size(); start += renderBlock)
    {
        if (cancel.load (std::memory_order_relaxed))
            return {};
        const auto len = (int) std::min ((size_t) renderBlock, di.size() - start);
        buffer.copyFrom (0, 0, di.data() + start, len);
        BlockContext context { buffer.getReadPointer (0), len };
        section->process (juce::dsp::AudioBlock<float> (buffer).getSubBlock (0, (size_t) len), context);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + len, out.begin() + (std::ptrdiff_t) start);
    }
    return out;
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
                                           const Analysis& target, Mode mode, const Alignment* alignment)
{
    Candidate c;
    c.slot = slot;
    c.gainDb = gainDb;
    c.cab = cab;
    c.analysis = Analysis::of (convolve (ampOutput, ir)); // exactly what the cab does, up to its level
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
    if (! target.ok)
    {
        result.error = "The target has too little playing in it to analyse.";
        return result;
    }

    std::vector<int> slots;
    for (int s = 0; s < 3; ++s)
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
        irs.push_back (loadIR (f));
        cabBins.push_back (cabPowerOnBins (irs.back()));
    }

    // Renders, keyed by (slot, Gain in thousandths of a dB).
    std::map<std::pair<int, int>, std::vector<float>> renders;
    auto key = [] (int s, double g) { return std::make_pair (s, (int) std::lround (g * 1000.0)); };
    std::atomic<bool> renderFailed { false };

    auto renderAll = [&] (const std::vector<std::pair<int, double>>& jobs, double fromFraction, double toFraction) {
        std::vector<std::vector<float>> outs (jobs.size());
        std::atomic<int> done { 0 };
        parallelFor ((int) jobs.size(), threads, [&] (int i, int) {
            outs[(size_t) i] = renderAmp (settings.models[(size_t) jobs[(size_t) i].first], settings.calibration, reference,
                                          jobs[(size_t) i].second, cancel);
            if (outs[(size_t) i].empty() && ! cancel.load())
                renderFailed = true;
            report (fromFraction + (toFraction - fromFraction) * ++done / (double) jobs.size(), "Rendering your DI through the amps");
        });
        for (size_t i = 0; i < jobs.size(); ++i)
            renders[key (jobs[i].first, jobs[i].second)] = std::move (outs[i]);
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
        for (auto s : slots)
        {
            alignments[s] = align (target, Analysis::of (renders.at (key (s, 0.0))));
            if (cancelled())
                return result;
        }
    }

    // 2. Candidates.
    std::map<std::tuple<int, int, int>, Candidate> candidates;
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

    auto evaluate = [&] (const std::vector<std::tuple<int, double, std::vector<int>>>& jobs, double fromFraction, double toFraction) {
        // Expand (slot, Gain, cabs or empty for "screen first") into one job per candidate. The screens
        // run in parallel; the maps are only read while the workers run.
        std::vector<std::vector<int>> chosenCabs (jobs.size());
        parallelFor ((int) jobs.size(), threads, [&] (int i, int) {
            const auto& [s, g, cabs] = jobs[(size_t) i];
            chosenCabs[(size_t) i] = cabs.empty() ? screen (s, g) : cabs;
        });
        std::vector<std::tuple<int, double, int>> work;
        for (size_t j = 0; j < jobs.size(); ++j)
        {
            const auto& [s, g, cabs] = jobs[j];
            for (auto c : chosenCabs[j])
                if (candidates.find ({ s, (int) std::lround (g * 1000.0), c }) == candidates.end())
                    work.push_back ({ s, g, c });
        }
        std::atomic<int> done { 0 };
        parallelFor ((int) work.size(), threads, [&] (int i, int) {
            if (cancel.load())
                return;
            const auto [s, g, c] = work[(size_t) i];
            auto cand = score (s, g, c, renders.at (key (s, g)), irs[(size_t) c], target, settings.mode,
                               settings.mode == Mode::samePart ? &alignments.at (s) : nullptr);
            {
                const std::lock_guard<std::mutex> lock (candidatesLock); // worker threads only, never audio
                candidates[{ s, (int) std::lround (g * 1000.0), c }] = std::move (cand);
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
    auto bestFor = [&] (int slot) -> const Candidate* {
        const Candidate* best = nullptr;
        for (const auto& [k, c] : candidates)
            if (c.slot == slot && (best == nullptr || c.total() < best->total()))
                best = &c;
        return best;
    };

    auto slotOrder = slots;
    std::stable_sort (slotOrder.begin(), slotOrder.end(), [&] (int a, int b) { return bestFor (a)->total() < bestFor (b)->total(); });
    const auto refineCount = juce::jmin (settings.refineSlots, (int) slotOrder.size());
    double fraction = 0.70;
    const auto fractionStep = 0.22 / juce::jmax (1, refineCount * (int) settings.refineSteps.size());

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

    // 4. The best candidate: polish its tone, fit the match EQ.
    report (0.93, "Fitting the match EQ");
    std::vector<const Candidate*> ranked;
    for (const auto& [k, c] : candidates)
        ranked.push_back (&c);
    std::stable_sort (ranked.begin(), ranked.end(), [] (const Candidate* a, const Candidate* b) { return a->total() < b->total(); });
    const auto& best = *ranked.front();

    std::tie (result.tone, result.spectralErrorDb) = fitTone (best.residual, target.weights, true);
    const auto tone = toneDb (result.tone);
    result.residual.resize ((size_t) numBands);
    for (size_t b = 0; b < (size_t) numBands; ++b)
        result.residual[b] = best.residual[b] - tone[b];
    std::tie (result.eq, result.eqTarget) = fitMatchEq (result.residual, target.weights);
    const auto eqCurve = eqDb (result.eq);
    std::vector<double> after ((size_t) numBands);
    for (size_t b = 0; b < (size_t) numBands; ++b)
        after[b] = result.residual[b] - eqCurve[b];
    result.spectralErrorAfterEqDb = weightedRmsCentred (after, target.weights);

    result.slot = best.slot;
    result.gainDb = best.gainDb;
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
    result.runtimeSeconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    result.ok = true;
    report (1.0, "Done");
    return result;
}

} // namespace ampsim::tonematch
