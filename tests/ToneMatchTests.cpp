// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// Tone match (docs/TONE_MATCH.md), the matcher: the C++ port against the Python prototype
// (prototypes/tone_match.py) on the same files (tests/fixtures/tone_match, written by its "golden"
// command), the private amp section against the real chain, and the search on synthetic cases with
// known settings (as in Stage A), with its runtime.

#include "TestHelpers.h"
#include "dsp/Chain.h"
#include "platform/AppInfo.h"
#include "tonematch/AudioFileInput.h"
#include "tonematch/ToneMatcher.h"

namespace
{
using namespace testing;
using namespace ampsim::tonematch;

juce::File fixtures() { return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/tone_match"); }

std::vector<float> readMono (const juce::File& f)
{
    const auto b = readWav (f);
    return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
}

const juce::var& expected()
{
    static const juce::var v = juce::JSON::parse (fixtures().getChildFile ("expected.json").loadFileAsString());
    return v;
}

std::vector<double> doubles (const juce::var& v)
{
    std::vector<double> out;
    if (const auto* a = v.getArray())
        for (const auto& x : *a)
            out.push_back ((double) x);
    return out;
}

double maxDiff (const std::vector<double>& a, const std::vector<double>& b)
{
    double m = a.size() == b.size() ? 0.0 : 1.0e9;
    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
        m = std::max (m, std::abs (a[i] - b[i]));
    return m;
}

template <size_t N>
std::vector<double> vec (const std::array<double, N>& a) { return std::vector<double> (a.begin(), a.end()); }

juce::File model (int slot)
{
    static const char* names[] = { "Glass", "Ember", "Monolith" };
    // The single captures the fixtures were made with (the built-ins until they became gain sets, 2026-10-04).
    return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/tone_match/captures").getChildFile (juce::String (names[slot]) + ".nam");
}

std::vector<juce::File> builtInCabs()
{
    auto files = platform::factoryContentFolder().getChildFile ("irs").findChildFiles (juce::File::findFiles, true, "*.wav");
    // The prototype's order: sorted by path (folder, then file name), as Python's sorted() on the paths.
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFullPathName() < b.getFullPathName(); });
    return std::vector<juce::File> (files.begin(), files.end());
}

juce::File cabNamed (const juce::String& stem)
{
    for (const auto& f : builtInCabs())
        if (f.getFileNameWithoutExtension() == stem)
            return f;
    return {};
}

MatchSettings settingsFor (Mode mode)
{
    MatchSettings s;
    s.mode = mode;
    for (int i = 0; i < 3; ++i)
        s.models[(size_t) i] = model (i);
    s.cabs = builtInCabs();
    return s;
}

/// The DI through the whole chain, as ampsim_render renders it: this capture in slot 1 with this Gain and
/// tone, this cab IR in close mic 1, the post EQ's parametric bands if any. For the synthetic targets.
std::vector<float> renderChain (const std::vector<float>& di, int slot, float gainDb, const std::array<float, 5>& tone, const juce::File& cab,
                                const std::vector<ampsim::Equalizer::Band>& eq = {})
{
    ampsim::Chain chain;
    chain.amp.slot (0).model.loadModel (model (slot), true);
    if (cab != juce::File())
        chain.cab.loadCloseMic (0, cab);
    chain.amp.slot (0).inputTrim.setGainDecibels (gainDb);
    for (int b = 0; b < 5; ++b)
        chain.amp.slot (0).tone.setGainDb ((ampsim::AmpTone::Band) b, tone[(size_t) b]);
    if (! eq.empty())
    {
        ampsim::Equalizer::Settings settings;
        settings.mode = ampsim::Equalizer::Mode::parametric;
        for (size_t i = 0; i < eq.size(); ++i)
            settings.bands[i] = eq[i];
        chain.postEq.setSettings (settings);
        chain.setBypassed (ampsim::Chain::Slot::postEq, false);
    }
    chain.prepare (fs, blockSize);
    juce::AudioBuffer<float> block (2, blockSize);
    std::vector<float> out (di.size());
    for (size_t start = 0; start < di.size(); start += blockSize)
    {
        const auto len = (int) std::min ((size_t) blockSize, di.size() - start);
        block.copyFrom (0, 0, di.data() + start, len);
        block.clear (1, 0, len);
        chain.process (juce::dsp::AudioBlock<float> (block).getSubBlock (0, (size_t) len));
        std::copy (block.getReadPointer (0), block.getReadPointer (0) + len, out.begin() + (std::ptrdiff_t) start);
    }
    return out;
}

const char* ampName (int slot)
{
    static const char* names[] = { "Glass", "Ember", "Monolith" };
    return names[juce::jlimit (0, 2, slot)];
}

juce::String toneText (const std::array<double, 5>& t)
{
    juce::StringArray s;
    for (auto v : t)
        s.add ((v >= 0 ? "+" : "") + juce::String (v, 1));
    return s.joinIntoString ("/");
}

/// Measures a matched result the way the prototype's verify() does: the DI rendered with every setting
/// through the real chain, against the target (long-term or aligned spectral error, and distortion distance).
std::pair<double, double> verify (const MatchResult& r, const std::vector<float>& target, const std::vector<float>& di)
{
    std::vector<ampsim::Equalizer::Band> eq (r.eq.begin(), r.eq.end());
    const auto y = renderChain (di, r.slot, (float) r.gainDb,
                                { (float) r.tone[0], (float) r.tone[1], (float) r.tone[2], (float) r.tone[3], (float) r.tone[4] }, r.cab, eq);
    const auto ta = Analysis::of (target), ya = Analysis::of (y);
    auto nl = nonlinearDistance (ta.features, ya.features);
    std::vector<double> d ((size_t) numBands);
    if (r.mode == Mode::samePart)
    {
        const auto a = align (ta, ya);
        const auto diff = alignedDifference (ta, ya, a);
        nl += diff.envelope / envelopeScaleDb;
        return { weightedRmsCentred (diff.residual, ta.weights), nl };
    }
    for (size_t b = 0; b < d.size(); ++b)
        d[b] = ta.ltas[b] - ya.ltas[b];
    return { weightedRmsCentred (smoothBands (d), ta.weights), nl };
}

class ToneMatchTests final : public juce::UnitTest
{
public:
    ToneMatchTests() : juce::UnitTest ("Tone match", "ampsim") {}

    void runTest() override
    {
        const auto& e = expected();
        const auto reference = readMono (fixtures().getChildFile ("reference_di.wav"));

        beginTest ("golden: the sixth-octave bands, their weights, and Nelder-Mead on Rosenbrock's function match the prototype");
        {
            const auto centres = doubles (e["constants"]["bands"]), weights = doubles (e["constants"]["weights"]);
            std::vector<double> c, w;
            for (int b = 0; b < numBands; ++b)
            {
                c.push_back (bands().centre[(size_t) b]);
                w.push_back (bands().weight[(size_t) b]);
            }
            expectEquals ((int) centres.size(), numBands);
            expect (maxDiff (c, centres) < 1.0e-9 && maxDiff (w, weights) < 1.0e-12);

            auto rosen = [] (const std::vector<double>& v) { return (1.0 - v[0]) * (1.0 - v[0]) + 100.0 * (v[1] - v[0] * v[0]) * (v[1] - v[0] * v[0]); };
            const auto [x, f] = nelderMead (rosen, { -1.2, 1.0 }, { 0.5, 0.5 }, { -5.0, -5.0 }, { 5.0, 5.0 }, 400, 1.0e-12);
            const auto ex = doubles (e["nelder_mead"]["x"]);
            const auto dx = maxDiff (x, ex);
            expect (dx < 1.0e-9, juce::String (dx));
            expectWithinAbsoluteError (f, (double) e["nelder_mead"]["f"], 1.0e-12);
            logMessage ("  -> " + juce::String (numBands) + " bands, " + dB (c.front()).upToFirstOccurrenceOf (" ", false, false) + " to "
                        + juce::String (c.back(), 0) + " Hz, identical; Nelder-Mead ends at (" + juce::String (x[0], 6) + ", " + juce::String (x[1], 6)
                        + "), f " + juce::String (f, 12) + ", " + juce::String (dx, 15) + " from the prototype's point");
        }

        beginTest ("golden: the analysis (long-term spectrum, weights, four distortion features, chroma) of the fixtures matches the prototype");
        {
            juce::StringArray rows;
            auto check = [&] (const juce::String& name, const std::vector<float>& x, const juce::var& ex) {
                const auto a = Analysis::of (x);
                expect (a.ok);
                expectEquals (a.numFrames, (int) ex["frames"]);
                int active = 0;
                for (auto v : a.active)
                    active += v;
                expect (std::abs (active - (int) ex["active"]) <= 1, name + " active " + juce::String (active));
                const auto ltasDiff = maxDiff (a.ltas, doubles (ex["ltas"]));
                const auto featDiff = maxDiff (vec (a.features), doubles (ex["features"]));
                std::vector<double> wr, we = doubles (ex["weights"]);
                double wRel = 0.0;
                for (size_t b = 0; b < we.size(); ++b)
                    wRel = std::max (wRel, std::abs (a.weights[b] - we[b]) / we[b]);
                expect (ltasDiff < 0.01, name + " ltas " + juce::String (ltasDiff));
                expect (featDiff < 0.02, name + " features " + juce::String (featDiff));
                expect (wRel < 1.0e-3, name + " weights " + juce::String (wRel));
                double chromaDiff = 0.0, levelDiff = 0.0;
                if (ex.hasProperty ("chroma_row_10"))
                {
                    chromaDiff = maxDiff (std::vector<double> (a.chroma.begin() + 120, a.chroma.begin() + 132), doubles (ex["chroma_row_10"]));
                    levelDiff = maxDiff (std::vector<double> (a.level.begin(), a.level.begin() + 40), doubles (ex["level_first"]));
                    expect (chromaDiff < 1.0e-4 && levelDiff < 0.01, name + " chroma " + juce::String (chromaDiff) + " level " + juce::String (levelDiff));
                }
                rows.add (name + ": " + juce::String (a.numFrames) + " frames, " + juce::String (active) + " playing; long-term spectrum within "
                          + juce::String (ltasDiff, 5) + " dB, features within " + juce::String (featDiff, 5) + " (C++: spread "
                          + juce::String (a.features[0], 2) + " dB, flux " + juce::String (a.features[1], 3) + ", brightness "
                          + juce::String (a.features[2], 2) + " dB, crest " + juce::String (a.features[3], 2) + " dB)"
                          + (ex.hasProperty ("chroma_row_10") ? ", chroma within " + juce::String (chromaDiff, 7) + ", level within " + juce::String (levelDiff, 5) + " dB" : ""));
            };
            check ("reference DI", reference, e["reference_analysis"]);
            check ("target (anything)", readMono (fixtures().getChildFile ("target_anything.wav")), e["anything"]["target_analysis"]);
            check ("target (same part)", readMono (fixtures().getChildFile ("target_same.wav")), e["same"]["target_analysis"]);
            for (const auto& r : rows)
                logMessage ("  -> " + r);
        }

        beginTest ("the matcher's private amp section renders what the whole chain plays with effects off and no cab (ampsim_render's path)");
        {
            const std::atomic<bool> noCancel { false };
            juce::StringArray rows;
            for (int s = 0; s < 3; ++s)
            {
                const auto viaSection = ToneMatcher::renderAmp (model (s), {}, reference, 4.5, noCancel);
                const auto viaChain = renderChain (reference, s, 4.5f, {}, {});
                const auto err = relativeErrorDb (viaSection, viaChain);
                const auto maxErr = maxAbsDifference (viaSection, viaChain);
                expect (viaSection.size() == viaChain.size() && maxErr < 1.0e-6, juce::String (maxErr));
                rows.add (juce::String (ampName (s)) + " max |diff| " + juce::String (maxErr, 9) + " (" + dB (err) + " re. its RMS)");
            }
            logMessage ("  -> Gain +4.5 dB: " + rows.joinIntoString ("; "));
        }

        beginTest ("the cab by FFT convolution equals direct convolution, and the cab block itself, within -100 dB");
        {
            const auto ir = ToneMatcher::loadIR (cabNamed ("Modern 4x12, dynamic, 75 W, var. 2"));
            const auto x = std::vector<float> (reference.begin(), reference.begin() + 30000);
            const auto fast = ToneMatcher::convolve (x, ir);
            const auto direct = directConvolution (x, std::vector<double> (ir.begin(), ir.end()));
            std::vector<double> head (direct.begin(), direct.begin() + (std::ptrdiff_t) x.size());
            const auto vsDirect = relativeErrorDb (fast, head);
            expect (vsDirect < -100.0, dB (vsDirect));

            // The chain's cab (JUCE's convolution with its loudness normalization) is this times a constant.
            const auto amp = ToneMatcher::renderAmp (model (1), {}, reference, 0.0, std::atomic<bool> { false });
            const auto viaCab = renderChain (reference, 1, 0.0f, {}, cabNamed ("Modern 4x12, dynamic, 75 W, var. 2"));
            const auto viaConv = ToneMatcher::convolve (amp, ToneMatcher::loadIR (cabNamed ("Modern 4x12, dynamic, 75 W, var. 2")));
            double num = 0.0, den = 0.0;
            for (size_t i = 0; i < viaCab.size(); ++i)
            {
                num += (double) viaCab[i] * viaConv[i];
                den += (double) viaConv[i] * viaConv[i];
            }
            const auto g = num / den;
            std::vector<float> scaled (viaConv.size());
            for (size_t i = 0; i < scaled.size(); ++i)
                scaled[i] = (float) (g * viaConv[i]);
            const auto vsCab = relativeErrorDb (scaled, viaCab);
            expect (vsCab < -100.0, dB (vsCab));
            logMessage ("  -> 21 built-in cabs available; FFT overlap-add vs direct: " + dB (vsDirect) + "; the whole chain with the cab vs the amp section "
                        "convolved here, times its normalization gain " + juce::String (g, 4) + ": " + dB (vsCab));
        }

        for (const auto* modeName : { "anything", "same" })
        {
            const auto mode = juce::String (modeName) == "same" ? Mode::samePart : Mode::anything;
            const auto& ex = e[modeName];
            const auto target = readMono (fixtures().getChildFile ("target_" + juce::String (modeName) + ".wav"));
            const auto ta = Analysis::of (target);

            beginTest ("golden (" + juce::String (modeName) + "): one candidate scored as the search scores it, its tone fits, and the match EQ, against the prototype");
            {
                const auto& c = ex["candidate"];
                const auto slot = (int) c["slot"];
                const auto gain = (double) c["gain"];
                const std::atomic<bool> noCancel { false };
                const auto y = ToneMatcher::renderAmp (model (slot), {}, reference, gain, noCancel);
                std::unique_ptr<Alignment> alignment;
                juce::String alignmentText;
                if (mode == Mode::samePart)
                {
                    alignment = std::make_unique<Alignment> (align (ta, Analysis::of (ToneMatcher::renderAmp (model (slot), {}, reference, 0.0, noCancel))));
                    const auto& ea = ex["alignment"];
                    long long checksum = 0;
                    for (const auto& [i, j] : alignment->path)
                        checksum += i * 3 + j * 7;
                    expectEquals ((int) alignment->path.size(), (int) ea["length"]);
                    expectEquals ((juce::int64) checksum, (juce::int64) ea["checksum"]);
                    expectWithinAbsoluteError (alignment->meanCost, (double) ea["mean_cost"], 1.0e-5);
                    alignmentText = "DTW path " + juce::String ((int) alignment->path.size()) + " steps (prototype " + ea["length"].toString()
                                    + "), path checksum " + juce::String (checksum) + " (prototype " + ea["checksum"].toString() + "), mean cost "
                                    + juce::String (alignment->meanCost, 6) + " (prototype " + juce::String ((double) ea["mean_cost"], 6) + "); ";
                }
                const auto cand = ToneMatcher::score (slot, gain, 0, y, ToneMatcher::loadIR (cabNamed (c["cab"].toString())), ta, mode, alignment.get());
                const auto featDiff = maxDiff (vec (cand.analysis.features), doubles (c["features"]));
                // Up to a constant: since 2026-10-04 a single capture's Gain is loudness-compensated in the engine
                // (BUILD_PLAN "Amp gain"), which the prototype's renders weren't, so away from Gain 5 the residual
                // carries a level offset. The score ignores level, so only the shape is compared.
                auto residual = cand.residual;
                const auto expectedResidual = doubles (c["residual"]);
                if (residual.size() == expectedResidual.size() && ! residual.empty())
                {
                    double offset = 0.0;
                    for (size_t i = 0; i < residual.size(); ++i)
                        offset += (expectedResidual[i] - residual[i]) / (double) residual.size();
                    for (auto& r : residual)
                        r += offset;
                }
                const auto resDiff = maxDiff (residual, expectedResidual);
                const auto toneDiff = maxDiff (vec (cand.tone), doubles (c["tone_linear"]));
                expect (featDiff < 0.02, juce::String (featDiff));
                expect (resDiff < 0.02, juce::String (resDiff));
                expect (toneDiff < 0.05, juce::String (toneDiff));
                expectWithinAbsoluteError (cand.spectral, (double) c["spectral"], 0.01);
                expectWithinAbsoluteError (cand.distortion, (double) c["distortion"], 0.05);

                const auto [polished, polishedError] = fitTone (cand.residual, ta.weights, true);
                expectWithinAbsoluteError (polishedError, (double) c["tone_polished_error"], 0.01);

                // The match EQ on the prototype's own input residual: the fitted curves and errors agree.
                const auto eqInput = doubles (c["eq_input"]);
                const auto [eq, eqTarget] = fitMatchEq (eqInput, ta.weights);
                const auto curve = eqDb (eq);
                const auto curveDiff = maxDiff (vec (curve), doubles (c["eq_curve"]));
                std::vector<double> after (eqInput.size());
                for (size_t b = 0; b < after.size(); ++b)
                    after[b] = eqInput[b] - curve[b];
                const auto eqError = weightedRmsCentred (after, ta.weights);
                expect (curveDiff < 0.3, "EQ curve " + juce::String (curveDiff));
                expectWithinAbsoluteError (eqError, (double) c["eq_error"], 0.05);

                logMessage ("  -> " + alignmentText + juce::String (ampName (slot)) + " at " + juce::String (gain, 1) + " dB through " + c["cab"].toString()
                            + ": features within " + juce::String (featDiff, 4) + ", residual within " + juce::String (resDiff, 4)
                            + " dB, linear tone within " + juce::String (toneDiff, 4) + " dB, spectral error " + juce::String (cand.spectral, 3)
                            + " dB (prototype " + juce::String ((double) c["spectral"], 3) + "), distortion " + juce::String (cand.distortion, 3)
                            + " (prototype " + juce::String ((double) c["distortion"], 3) + "), polished tone error " + juce::String (polishedError, 3)
                            + " dB (prototype " + juce::String ((double) c["tone_polished_error"], 3) + "); match EQ curve within "
                            + juce::String (curveDiff, 3) + " dB of the prototype's, error after it " + juce::String (eqError, 3) + " dB (prototype "
                            + juce::String ((double) c["eq_error"], 3) + ")");
            }

            beginTest ("golden (" + juce::String (modeName) + "): the whole search on the fixtures finds the prototype's amp with a comparable score");
            {
                const std::atomic<bool> noCancel { false };
                int progressCalls = 0;
                double lastFraction = -1.0;
                bool monotonic = true;
                const auto r = ToneMatcher::match (target, reference, settingsFor (mode), noCancel, [&] (double f, const juce::String&) {
                    // Called from worker threads: only counting here.
                    ++progressCalls;
                    juce::ignoreUnused (f);
                });
                juce::ignoreUnused (lastFraction, monotonic);
                const auto& em = ex["match"];
                expect (r.ok, r.error);
                expectEquals (r.slot, (int) em["slot"]);
                const auto combined = r.spectralErrorAfterEqDb + lambda * r.distortion;
                expect (std::abs (combined - (double) em["combined"]) < 0.5, juce::String (combined) + " vs " + em["combined"].toString());
                expect (progressCalls > 10);
                logMessage ("  -> true: " + juce::String (ampName ((int) ex["true"]["slot"])) + " " + ex["true"]["gain"].toString() + " dB, "
                            + ex["true"]["cab"].toString() + "; C++: " + ampName (r.slot) + " " + juce::String (r.gainDb, 1) + " dB, tone "
                            + toneText (r.tone) + ", " + r.cab.getFileNameWithoutExtension() + ", spectral " + juce::String (r.spectralErrorAfterEqDb, 2)
                            + " dB, distortion " + juce::String (r.distortion, 2) + ", combined " + juce::String (combined, 2) + ", closeness "
                            + juce::String (r.closeness, 0) + "; prototype: " + em["amp"].toString() + " " + em["gain_db"].toString() + " dB, "
                            + em["cab"].toString().upToLastOccurrenceOf (".", false, false) + ", combined " + juce::String ((double) em["combined"], 2)
                            + "; " + juce::String (r.renders) + " renders, " + juce::String (r.candidates) + " candidates, "
                            + juce::String (r.runtimeSeconds, 1) + " s on " + juce::String (juce::jmax (1, (int) std::thread::hardware_concurrency() - 1))
                            + " threads, " + juce::String (progressCalls) + " progress reports");
            }
        }

        beginTest ("the search on synthetic cases with known settings (each amp, other Gains, cabs, tone, an EQ): right amp, better than the defaults");
        {
            struct Case
            {
                int slot;
                float gain;
                std::array<float, 5> tone;
                const char* cab;
                std::vector<ampsim::Equalizer::Band> eq;
            };
            using BT = ampsim::Equalizer::BandType;
            const std::vector<Case> cases {
                { 0, 9.0f, { 0, 2, -1, 3, 1 }, "Vintage 4x12, dynamic, upper, var. 3", {} },
                { 1, -7.0f, { 0, 0, 0, 0, 0 }, "Modern 4x12, blend, 75 W + bright 60 W, 2", { { BT::highShelf, 6000.0f, -4.0f, 0.7071f } } },
                { 2, 3.0f, { 4, -2, -5, 2, 1 }, "Modern 4x12, dynamic, dark 60 W", {} },
            };
            const auto performances = std::array<std::pair<Mode, juce::String>, 2> { { { Mode::anything, "target_di_anything.wav" },
                                                                                       { Mode::samePart, "target_di_same.wav" } } };
            juce::StringArray rows;
            int right = 0, better = 0, total = 0;
            const auto defaultCab = cabNamed ("Vintage 4x12, dynamic, upper, var. 2");
            for (const auto& [mode, diFile] : performances)
            {
                const auto targetDi = readMono (fixtures().getChildFile (diFile));
                for (const auto& c : cases)
                {
                    const auto target = renderChain (targetDi, c.slot, c.gain, c.tone, cabNamed (c.cab), c.eq);
                    const std::atomic<bool> noCancel { false };
                    const auto r = ToneMatcher::match (target, reference, settingsFor (mode), noCancel);
                    expect (r.ok, r.error);
                    const auto [spectral, distortion] = verify (r, target, reference);
                    const auto combined = spectral + lambda * distortion;

                    double bestDefault = 1.0e9;
                    for (int s = 0; s < 3; ++s)
                    {
                        MatchResult d;
                        d.mode = mode;
                        d.slot = s;
                        d.cab = defaultCab;
                        const auto [ds, dn] = verify (d, target, reference);
                        bestDefault = std::min (bestDefault, ds + lambda * dn);
                    }
                    right += r.slot == c.slot ? 1 : 0;
                    better += combined < bestDefault ? 1 : 0;
                    ++total;
                    rows.add (juce::String (mode == Mode::samePart ? "same part" : "anything") + " | " + ampName (c.slot) + " " + juce::String (c.gain, 0)
                              + " dB, " + c.cab + (c.eq.empty() ? "" : ", EQ") + " | " + ampName (r.slot) + " " + juce::String (r.gainDb, 1) + " dB, tone "
                              + toneText (r.tone) + ", " + r.cab.getFileNameWithoutExtension() + " | spectral " + juce::String (spectral, 2)
                              + " dB, distortion " + juce::String (distortion, 2) + ", combined " + juce::String (combined, 2) + " vs best default "
                              + juce::String (bestDefault, 2) + " | " + juce::String (r.runtimeSeconds, 1) + " s");
                }
            }
            // Every match must beat the defaults. The amp: Stage A got 17 of 18 dry cases right with a 17 s
            // reference; this reference DI is 9 s, and the misses land on the neighbouring amp (a cranked
            // Ember for a low-gain Monolith, a pushed Glass for a light Ember), so at least two thirds.
            expect (right * 3 >= total * 2, juce::String (right));
            expectEquals (better, total);
            logMessage ("  -> mode | true | matched | verified through the chain | time");
            for (const auto& row : rows)
                logMessage ("  -> " + row);
            logMessage ("  -> amp right " + juce::String (right) + "/" + juce::String (total) + ", better than the best of the three amps at default "
                        + juce::String (better) + "/" + juce::String (total));
        }

        // ---- Play along (docs/TONE_MATCH.md, "Play along"): a take recorded lined up with the target ----
        const auto playAlongTargetDi = [&] {
            auto x = readMono (fixtures().getChildFile ("target_di_playalong.wav"));
            x.resize (std::min (x.size(), reference.size()));
            return x;
        }();
        // How far a path strays from the true alignment (the diagonal, shifted by `shift` frames when the DI
        // is late), in frames, over the target's playing frames (in silence any path costs the same).
        const auto alignmentError = [] (const std::vector<std::pair<int, int>>& path, const Analysis& target, double shift) {
            double sum = 0.0, worst = 0.0;
            int count = 0;
            for (const auto& [i, j] : path)
                if (target.active[(size_t) i])
                {
                    const auto off = std::abs ((double) j - ((double) i + shift));
                    sum += off;
                    worst = std::max (worst, off);
                    ++count;
                }
            return std::pair<double, double> { sum / std::max (1, count), worst };
        };

        beginTest ("golden (play along): DTW within the 0.5 s band on the play-along fixture finds the prototype's path, and unbanded too");
        {
            const juce::var pe = juce::JSON::parse (fixtures().getChildFile ("expected_playalong.json").loadFileAsString());
            const auto bandFrames = (int) std::ceil (playAlongBandSeconds * sampleRate / hop);
            expectEquals (bandFrames, (int) pe["band_frames"]);
            const auto target = renderChain (playAlongTargetDi, 2, 3.0f, { 4, -2, -5, 2, 1 }, cabNamed ("Modern 4x12, dynamic, dark 60 W"));
            const std::atomic<bool> noCancel { false };
            const auto ta = Analysis::of (target);
            const auto ca = Analysis::of (ToneMatcher::renderAmp (model (2), {}, std::vector<float> (reference.begin(), reference.begin() + (long) playAlongTargetDi.size()), 0.0, noCancel));
            juce::StringArray lines;
            for (const auto* which : { "banded", "full" })
            {
                const auto banded = juce::String (which) == "banded";
                const auto a = align (ta, ca, banded ? bandFrames : 0);
                long long checksum = 0;
                for (const auto& [i, j] : a.path)
                    checksum += i * 3 + j * 7;
                const auto& golden = pe[which];
                expectEquals ((int) a.path.size(), (int) golden["length"]);
                expectEquals ((juce::int64) checksum, (juce::int64) golden["checksum"]);
                expectWithinAbsoluteError (a.meanCost, (double) golden["mean_cost"], 1.0e-6);
                // Inside the band: every pair within bandFrames of the diagonal.
                int outside = 0;
                if (banded)
                    for (const auto& [i, j] : a.path)
                        outside += std::abs ((double) j - (double) i * (ca.numFrames - 1) / (double) (ta.numFrames - 1)) > bandFrames ? 1 : 0;
                expectEquals (outside, 0);
                const auto [mean, worst] = alignmentError (a.path, ta, 0.0);
                lines.add (juce::String (which) + ": " + juce::String ((int) a.path.size()) + " pairs, checksum " + juce::String (checksum) + ", mean cost "
                           + juce::String (a.meanCost, 5) + " (prototype " + juce::String ((double) golden["mean_cost"], 5) + "), off the true alignment by "
                           + juce::String (mean, 2) + " frames on average, " + juce::String (worst, 0) + " at most");
            }
            for (const auto& l : lines)
                logMessage ("  -> " + l);
        }

        beginTest ("play along, measured: same part on a take at the target's tempo, with the 0.5 s band and without, lined up and 80 ms late");
        {
            struct Case
            {
                int slot;
                float gain;
                std::array<float, 5> tone;
                const char* cab;
                std::vector<ampsim::Equalizer::Band> eq;
            };
            using BT = ampsim::Equalizer::BandType;
            const std::vector<Case> cases {
                { 0, 9.0f, { 0, 2, -1, 3, 1 }, "Vintage 4x12, dynamic, upper, var. 3", {} },
                { 1, -7.0f, { 0, 0, 0, 0, 0 }, "Modern 4x12, blend, 75 W + bright 60 W, 2", { { BT::highShelf, 6000.0f, -4.0f, 0.7071f } } },
                { 2, 3.0f, { 4, -2, -5, 2, 1 }, "Modern 4x12, dynamic, dark 60 W", {} },
            };
            const auto di = std::vector<float> (reference.begin(), reference.begin() + (long) playAlongTargetDi.size());
            const auto lateSamples = (size_t) (0.080 * sampleRate);
            auto late = std::vector<float> (lateSamples, 0.0f);
            late.insert (late.end(), di.begin(), di.end() - (long) lateSamples);
            juce::StringArray rows;
            int sameResult = 0, total = 0, right = 0, beatDefault = 0, runs = 0;
            double worstBanded = 0.0;
            for (const auto& [label, reference2, shift] : std::initializer_list<std::tuple<const char*, const std::vector<float>*, double>> {
                     { "lined up", &di, 0.0 }, { "80 ms late", &late, (double) lateSamples / hop } })
            {
                for (const auto& c : cases)
                {
                    const auto target = renderChain (playAlongTargetDi, c.slot, c.gain, c.tone, cabNamed (c.cab), c.eq);
                    const auto ta = Analysis::of (target);
                    double bestDefault = 1.0e9;
                    for (int s = 0; s < 3; ++s)
                    {
                        MatchResult d;
                        d.mode = Mode::samePart;
                        d.slot = s;
                        d.cab = cabNamed ("Vintage 4x12, dynamic, upper, var. 2");
                        const auto [ds, dn] = verify (d, target, *reference2);
                        bestDefault = std::min (bestDefault, ds + lambda * dn);
                    }
                    MatchResult results[2];
                    for (int banded = 0; banded < 2; ++banded)
                    {
                        auto settings = settingsFor (Mode::samePart);
                        settings.alignmentBandSeconds = banded ? playAlongBandSeconds : 0.0;
                        const std::atomic<bool> noCancel { false };
                        const auto r = ToneMatcher::match (target, *reference2, settings, noCancel);
                        expect (r.ok, r.error);
                        const auto [spectral, distortion] = verify (r, target, *reference2);
                        const auto [mean, worst] = alignmentError (r.alignmentPath, ta, shift);
                        if (banded)
                            worstBanded = std::max (worstBanded, worst);
                        right += r.slot == c.slot ? 1 : 0;
                        beatDefault += spectral + lambda * distortion < bestDefault ? 1 : 0;
                        ++runs;
                        rows.add (juce::String (label) + " | " + ampName (c.slot) + " " + juce::String (c.gain, 0) + " dB | " + (banded ? "band 0.5 s" : "full DTW")
                                  + " | " + ampName (r.slot) + " " + juce::String (r.gainDb, 1) + " dB, " + r.cab.getFileNameWithoutExtension()
                                  + " | off the true alignment " + juce::String (mean, 2) + " frames mean, " + juce::String (worst, 0) + " max | spectral "
                                  + juce::String (spectral, 2) + " dB, distortion " + juce::String (distortion, 2) + ", combined "
                                  + juce::String (spectral + lambda * distortion, 2) + " vs best default " + juce::String (bestDefault, 2) + " | "
                                  + juce::String (r.runtimeSeconds, 1) + " s");
                        results[banded] = r;
                    }
                    sameResult += results[0].slot == results[1].slot && std::abs (results[0].gainDb - results[1].gainDb) < 1.0e-9 && results[0].cab == results[1].cab ? 1 : 0;
                    ++total;
                }
            }
            // The band keeps the path within 0.5 s (12 frames) of the recorded alignment, plus the 80 ms the DI
            // is late; and every match beats the defaults, as the synthetic search test asks.
            expectLessOrEqual (worstBanded, 12.0 + (double) lateSamples / hop + 0.5);
            expectEquals (beatDefault, runs);
            logMessage ("  -> mode | true | alignment | matched | path against the truth | verified through the chain | time");
            for (const auto& row : rows)
                logMessage ("  -> " + row);
            logMessage ("  -> the band changed the matched amp, Gain, or cab in " + juce::String (total - sameResult) + " of " + juce::String (total)
                        + " cases; amp right " + juce::String (right) + "/" + juce::String (runs) + "; better than the best default " + juce::String (beatDefault)
                        + "/" + juce::String (runs));
        }

        beginTest ("cancel: a match stops within a second of the flag (renders check it every 128 samples, and no capture loads after it) and says so");
        {
            // With the default worker count, and with one worker (a starved runner, where the grid's queued
            // renders would each have loaded their capture before seeing the flag).
            juce::StringArray report;
            const auto target = readMono (fixtures().getChildFile ("target_anything.wav"));
            for (const auto threads : { 0, 1 })
            {
                std::atomic<bool> cancel { false };
                double cancelledAt = 0.0;
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                std::thread canceller ([&] {
                    juce::Thread::sleep (300);
                    cancelledAt = juce::Time::getMillisecondCounterHiRes();
                    cancel = true;
                });
                auto settings = settingsFor (Mode::anything);
                settings.threads = threads;
                const auto r = ToneMatcher::match (target, reference, settings, cancel);
                const auto returnedAt = juce::Time::getMillisecondCounterHiRes();
                canceller.join();
                expect (! r.ok && r.cancelled);
                const auto lag = returnedAt - cancelledAt;
                // The renders check the flag every 128 samples; loading a capture (with its loudness measurement)
                // and analysing a candidate can't be interrupted, so allow one of those per worker.
                expect (lag < 1000.0 * testing::cpuBudgetScale(), juce::String (lag)); // CPU-bound: scaled on CI
                report.add (juce::String (threads == 0 ? "default workers" : "1 worker") + ": returned " + juce::String (lag, 1)
                            + " ms after the flag (" + juce::String (juce::roundToInt (returnedAt - t0)) + " ms in all), \"" + r.error + "\"");
            }
            logMessage ("  -> cancelled 300 ms in; " + report.joinIntoString ("; "));
        }

        beginTest ("file input: every format this computer decodes reads back as 48 kHz mono");
        {
            const auto dir = tempDir().getChildFile ("tone_match_formats");
            dir.deleteRecursively();
            dir.createDirectory();
            const auto source = std::vector<float> (reference.begin(), reference.begin() + 96000);
            juce::AudioBuffer<float> stereo (2, (int) source.size());
            stereo.copyFrom (0, 0, source.data(), (int) source.size());
            stereo.copyFrom (1, 0, source.data(), (int) source.size());

            juce::StringArray rows;
            auto writeWith = [&] (juce::AudioFormat& format, const juce::String& name, double rate, int bits) {
                const auto f = dir.getChildFile (name);
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
                auto options = juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (bits);
                auto writer = format.createWriterFor (stream, options);
                if (writer == nullptr)
                    return juce::File();
                juce::AudioBuffer<float> data = stereo;
                if (std::abs (rate - 48000.0) > 1.0)
                {
                    const auto r = AudioFileInput::resampleTo48k (source, 48000.0 * 48000.0 / rate); // the inverse ratio: 48 k -> rate
                    data.setSize (2, (int) r.size());
                    data.copyFrom (0, 0, r.data(), (int) r.size());
                    data.copyFrom (1, 0, r.data(), (int) r.size());
                }
                writer->writeFromAudioSampleBuffer (data, 0, data.getNumSamples());
                return f;
            };
            juce::WavAudioFormat wav;
            juce::AiffAudioFormat aiff;
            juce::FlacAudioFormat flac;
            std::vector<juce::File> files { writeWith (wav, "a.wav", 48000.0, 24), writeWith (wav, "b44.wav", 44100.0, 16),
                                            writeWith (aiff, "c.aiff", 48000.0, 24), writeWith (flac, "d.flac", 44100.0, 24) };
           #if JUCE_MAC
            // M4A (AAC), encoded by macOS's afconvert, decoded by JUCE's CoreAudioFormat.
            {
                const auto out = dir.getChildFile ("e.m4a");
                juce::ChildProcess p;
                juce::String log;
                if (p.start (juce::StringArray { "/usr/bin/afconvert", "-f", "m4af", "-d", "aac", "-b", "192000", files[1].getFullPathName(), out.getFullPathName() }))
                    log = p.readAllProcessOutput();
                expect (out.existsAsFile(), log);
                files.push_back (out);
            }
           #endif
            // MP3: macOS decodes it but can't encode it, so the fixture holds one (the reference's first 2 s,
            // encoded by the prototype's "golden" command with LAME at 128 kbps, 44.1 kHz stereo).
            files.push_back (fixtures().getChildFile ("excerpt.mp3"));
            int decoded = 0;
            for (const auto& f : files)
            {
                if (f == juce::File())
                    continue;
                const auto in = AudioFileInput::read (f);
                expect (in.ok, in.error);
                if (! in.ok)
                    continue;
                ++decoded;
                // Same content: the level of the decoded file within 0.5 dB of the source's, 1 dB for the lossy codecs
                // (which drop some top end; they also pad and delay, so no sample compare).
                const auto levelDiff = toDb (rms (in.samples)) - toDb (rms (source));
                const auto lossy = f.hasFileExtension ("mp3;m4a");
                expect (std::abs (levelDiff) < (lossy ? 1.0 : 0.5), f.getFileName() + " " + juce::String (levelDiff));
                expect (std::abs (in.seconds() - 2.0) < 0.1, f.getFileName() + " " + juce::String (in.seconds()));
                rows.add (f.getFileExtension() + " (" + in.formatName + ", " + juce::String (in.sourceSampleRate, 0) + " Hz, "
                          + juce::String (in.sourceChannels) + " ch) -> " + juce::String (in.seconds(), 3) + " s mono at 48 kHz, level "
                          + juce::String (levelDiff, 2) + " dB from the source");
            }
           #if JUCE_MAC
            expectEquals (decoded, 6); // WAV twice, AIFF, FLAC, M4A, MP3
           #else
            expect (decoded >= 4);
           #endif
            const auto bad = AudioFileInput::read (dir.getChildFile ("missing.mp3"));
            expect (! bad.ok && bad.error.isNotEmpty());
            for (const auto& r : rows)
                logMessage ("  -> " + r);
            logMessage ("  -> the file chooser offers: " + AudioFileInput::wildcard());
        }
    }
};

static ToneMatchTests toneMatchTests;
} // namespace
