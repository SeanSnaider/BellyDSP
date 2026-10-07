// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The amp's Gain (BUILD_PLAN "Amp gain", ASSUMPTIONS AG1 to AG14): gain sets (several captures across an amp's
// gain knob, blended), the single capture's loudness-compensated trim, the step models' scheduling and warm-up,
// the factory presets' amp settings, and renders of every preset scene for Sean's ears.

#include "BuiltInCaptures.h"
#include "PluginEditor.h"
#include "Presets.h"
#include "TestHelpers.h"
#include "dsp/GainSet.h"
#include "dsp/Loudness.h"
#include "dsp/NamAmp.h"
#include "platform/AppInfo.h"

#include <NAM/get_dsp.h>

#include <filesystem>
#include <map>
#include <set>

namespace
{
using namespace testing;
using ampsim::NamAmp;
using GainKnob = ampsim::AmpSection::GainKnob;

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 6000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

float getParam (AmpSimProcessor& p, const juce::String& id)
{
    return p.parameters.getRawParameterValue (id)->load();
}

double lufs (const std::vector<float>& x)
{
    return ampsim::loudness::integratedMono (x.data(), (int) x.size(), fs);
}

double crestDb (const std::vector<float>& x, size_t skip = 4800)
{
    float peak = 0.0f;
    for (size_t i = skip; i < x.size(); ++i)
        peak = std::max (peak, std::abs (x[i]));
    return toDb (peak) - toDb (rms (x.data() + skip, x.size() - skip));
}

/// The nonlinear energy ratio (tools/content/make_default_captures.py, nl_ratio_db; ASSUMPTIONS AG2): with the
/// magnitude-squared coherence C(f) = |Pxy|^2 / (Pxx Pyy) between the DI x and the output y (Welch's method as
/// scipy.signal.coherence does it: 4096-point periodic Hann segments, half overlapping, each segment's mean
/// removed), C(f) Pyy(f) is the part of y's power at f that a linear time-invariant filter of x can predict, so
///     NL = sum (1 - C) Pyy / sum Pyy,   60 Hz < f < 12 kHz,
/// in dB, is the share no linear filter explains: about -37 dB for a clean amp on the tests' DI, -4 for a
/// saturated one. Only the ratios matter, so the spectra's scaling is left out.
double nlRatioDb (const std::vector<float>& x, const std::vector<float>& y)
{
    constexpr int order = 12, n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> pxx (n / 2 + 1), pyy (n / 2 + 1);
    std::vector<std::complex<double>> pxy (n / 2 + 1);
    std::vector<float> window ((size_t) n);
    for (int i = 0; i < n; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / n));
    std::vector<std::complex<float>> a ((size_t) n), b ((size_t) n), fa ((size_t) n), fb ((size_t) n);
    const auto length = std::min (x.size(), y.size());
    for (size_t start = 0; start + (size_t) n <= length; start += (size_t) n / 2)
    {
        double mx = 0.0, my = 0.0;
        for (size_t i = 0; i < (size_t) n; ++i)
        {
            mx += x[start + i];
            my += y[start + i];
        }
        mx /= n;
        my /= n;
        for (size_t i = 0; i < (size_t) n; ++i)
        {
            a[i] = { (float) ((x[start + i] - mx) * window[i]), 0.0f };
            b[i] = { (float) ((y[start + i] - my) * window[i]), 0.0f };
        }
        fft.perform (a.data(), fa.data(), false);
        fft.perform (b.data(), fb.data(), false);
        for (size_t k = 0; k <= (size_t) n / 2; ++k)
        {
            const std::complex<double> xa (fa[k].real(), fa[k].imag()), yb (fb[k].real(), fb[k].imag());
            pxx[k] += std::norm (xa);
            pyy[k] += std::norm (yb);
            pxy[k] += std::conj (xa) * yb;
        }
    }
    double unexplained = 0.0, total = 0.0;
    for (size_t k = 0; k <= (size_t) n / 2; ++k)
    {
        const auto f = (double) k * fs / n;
        if (f <= 60.0 || f >= 12000.0 || pxx[k] <= 0.0 || pyy[k] <= 0.0)
            continue;
        const auto c = std::norm (pxy[k]) / (pxx[k] * pyy[k]);
        unexplained += (1.0 - c) * pyy[k];
        total += pyy[k];
    }
    return 10.0 * std::log10 (std::max (unexplained, 1.0e-30) / std::max (total, 1.0e-30));
}

/// A slot's capture engine on its own: `before` runs before each 128-sample buffer (set the Gain there).
std::vector<float> run (NamAmp& amp, const std::vector<float>& input, const std::function<void (size_t block)>& before = {})
{
    const ampsim::BlockContext context;
    size_t block = 0;
    return runInBlocks (input, blockSize, [&] (juce::dsp::AudioBlock<float>& b, size_t)
    {
        if (before)
            before (block);
        amp.process (b.getSubsetChannelBlock (0, 1), context);
        ++block;
    }).left;
}

/// A capture at a fixed Gain from its first sample (the position snaps in prepare()).
std::vector<float> renderAt (const juce::File& capture, float position, const std::vector<float>& input, NamAmp::LoadResult* result = nullptr)
{
    NamAmp amp;
    const auto r = amp.loadModel (capture, true);
    if (result != nullptr)
        *result = r;
    amp.setGain (position);
    amp.prepare (fs, blockSize);
    return run (amp, input);
}

void writeSet (const juce::File& json, const juce::String& name, const std::vector<std::pair<double, juce::String>>& steps, const juce::String& format = ampsim::GainSet::formatName,
               int version = 1)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", format);
    root->setProperty ("version", version);
    root->setProperty ("name", name);
    juce::Array<juce::var> list;
    for (const auto& [gain, file] : steps)
    {
        auto* s = new juce::DynamicObject();
        s->setProperty ("gain", gain);
        s->setProperty ("file", file);
        list.add (juce::var (s));
    }
    root->setProperty ("steps", list);
    json.replaceWithText (juce::JSON::toString (juce::var (root)));
}

juce::String positionText (float p) { return juce::String (p, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."); }

/// Every amp parameter of every slot.
juce::StringArray ampParameterIds()
{
    juce::StringArray ids;
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
        for (const auto* name : { "input_trim", "output_trim", "depth", "bass", "mid", "treble", "presence" })
            ids.add (AmpSimProcessor::ampParamId (s, name));
    return ids;
}

/// The processor on the input in 128-sample buffers (this thread is its audio thread).
Stereo play (AmpSimProcessor& p, const std::vector<float>& input)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    Stereo out;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
        out.left.insert (out.left.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
        out.right.insert (out.right.end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + blockSize);
    }
    return out;
}

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

class AmpGainTests final : public juce::UnitTest
{
public:
    AmpGainTests() : juce::UnitTest ("Amp gain", "ampsim") {}

    void runTest() override
    {
        const std::array<const char*, 3> names { "Glass", "Ember", "Monolith" };
        const auto proof = proofDir().getChildFile ("amp_gain");
        proof.createDirectory();

        beginTest ("a gain set's JSON is read and checked: format, version, steps ascending in 0 to 10, every file there");
        {
            const auto dir = tempDir().getChildFile ("gainset_parse");
            dir.deleteRecursively();
            dir.createDirectory();
            exampleModel ("wavenet_a1_standard.nam").copyFileTo (dir.getChildFile ("a.nam"));
            exampleModel ("wavenet.nam").copyFileTo (dir.getChildFile ("b.nam"));
            const auto json = dir.getChildFile ("gainset.json");
            juce::String error;
            ampsim::GainSet set;

            writeSet (json, "Test", { { 0.0, "a.nam" }, { 10.0, "b.nam" } });
            expect (ampsim::GainSet::isGainSet (json) && ampsim::GainSet::isGainSet (dir));
            expect (ampsim::GainSet::read (dir, set, error), error);
            expect (set.steps.size() == 2 && set.steps[1].gain == 10.0 && set.steps[1].file == dir.getChildFile ("b.nam") && set.name == "Test");
            expect (! ampsim::GainSet::isGainSet (dir.getChildFile ("a.nam")));

            juce::StringArray errors;
            const auto bad = [&] (const std::vector<std::pair<double, juce::String>>& steps, const juce::String& format, int version, const juce::String& mustSay)
            {
                writeSet (json, "Bad", steps, format, version);
                const auto ok = ampsim::GainSet::read (json, set, error);
                expect (! ok && error.contains (mustSay), error);
                errors.add (error.fromFirstOccurrenceOf (": ", false, false).upToFirstOccurrenceOf (" (", false, false));
            };
            bad ({ { 0.0, "a.nam" } }, "something-else", 1, "isn't a gain set");
            bad ({ { 0.0, "a.nam" } }, ampsim::GainSet::formatName, 2, "version 2");
            bad ({ { 5.0, "a.nam" }, { 2.5, "b.nam" } }, ampsim::GainSet::formatName, 1, "ascending");
            bad ({ { 0.0, "a.nam" }, { 11.0, "b.nam" } }, ampsim::GainSet::formatName, 1, "0 to 10");
            bad ({ { 0.0, "a.nam" }, { 5.0, "missing.nam" } }, ampsim::GainSet::formatName, 1, "missing.nam is missing");
            std::vector<std::pair<double, juce::String>> twelve;
            for (int i = 0; i < 12; ++i)
                twelve.push_back ({ (double) i * 0.5, "a.nam" });
            bad (twelve, ampsim::GainSet::formatName, 1, "1 to 11 steps");

            // A bad set doesn't load, and says why.
            NamAmp amp;
            const auto r = amp.loadModel (json, true);
            expect (! r.ok && r.message.contains ("steps"), r.message);
            logMessage ("  -> a two-step set reads (by its JSON or its folder); refused with a reason: " + errors.joinIntoString ("; "));
        }

        beginTest ("a single capture: Gain is its input trim, loudness-compensated, so it changes the drive and not the level");
        {
            const auto input = guitarDI ((int) (6.0 * fs));
            juce::StringArray lines;
            for (const auto* file : { "wavenet_a1_standard.nam", "A2.nam", "lstm.nam" })
            {
                NamAmp::LoadResult r;
                std::vector<double> loud, crest;
                for (const auto position : { 0.0f, 2.5f, 5.0f, 7.5f, 10.0f })
                {
                    const auto out = renderAt (exampleModel (file), position, input, &r);
                    loud.push_back (lufs (out));
                    crest.push_back (crestDb (out));
                }
                expect (r.ok && ! r.isGainSet, r.message);
                expectEquals ((int) r.compensationDb.size(), NamAmp::compensationPoints);
                expectWithinAbsoluteError (r.compensationDb[8], 0.0, 1.0e-6); // unity trim: none
                const auto spread = *std::max_element (loud.begin(), loud.end()) - *std::min_element (loud.begin(), loud.end());
                expectLessThan (spread, 1.0, file);
                // Without the compensation the loudness would have moved by minus the compensation (the curve is that
                // change measured on the reference DI): the trims -24 and +24 dB are its first and last points.
                const auto rawSwing = r.compensationDb.front() - r.compensationDb.back();
                lines.add (juce::String (file) + ": LUFS at Gain 0/2.5/5/7.5/10 " + juce::String (loud[0], 1) + " / " + juce::String (loud[1], 1) + " / " + juce::String (loud[2], 1)
                           + " / " + juce::String (loud[3], 1) + " / " + juce::String (loud[4], 1) + " (spread " + juce::String (spread, 2) + " LU; uncompensated it would swing "
                           + juce::String (rawSwing, 1) + " dB), crest " + juce::String (crest[0], 1) + " -> " + juce::String (crest[4], 1) + " dB");
            }
            logMessage ("  -> " + lines.joinIntoString ("; "));
        }

        // The built-in gain sets (content/models/<Amp>/gainset.json, next to this binary).
        // sets are the three slot defaults; allSets every built-in amp (the five more since 2026-10-07 among them).
        std::array<juce::File, 3> sets;
        for (int s = 0; s < 3; ++s)
            sets[(size_t) s] = presets::builtInCapture (s);
        const auto builtIns = presets::builtInGainSets();
        std::vector<juce::File> allSets;
        std::vector<juce::String> allNames;
        for (const auto& b : builtIns)
        {
            allSets.push_back (b.file);
            allNames.push_back (b.name);
        }

        beginTest ("the built-in gain sets: five steps each, with metadata and CC BY 4.0 manifest entries");
        {
            const auto manifest = juce::JSON::parse (juce::File (AMPSIM_SOURCE_DIR).getChildFile ("content/manifest.json").loadFileAsString());
            juce::StringArray lines;
            expectEquals ((int) allSets.size(), 8);
            for (int s = 0; s < 3; ++s)
            {
                expectEquals (presets::builtInCapturePath (s), "factory:models/" + juce::String (names[(size_t) s]) + "/gainset.json");
                expect (allSets[(size_t) s] == sets[(size_t) s], allNames[(size_t) s]);
            }
            for (size_t s = 0; s < allSets.size(); ++s)
            {
                const auto source = s < 3 ? juce::String ("prototypes/amp_sim.py") : juce::String ("prototypes/amp_voicings.py");
                expectEquals (allSets[s].getParentDirectory().getFileName(), allNames[s]);
                ampsim::GainSet set;
                juce::String error;
                expect (ampsim::GainSet::read (allSets[s], set, error), error);
                expectEquals ((int) set.steps.size(), 5);
                juce::StringArray gains;
                for (const auto& step : set.steps)
                {
                    gains.add (positionText ((float) step.gain));
                    const auto m = juce::JSON::parse (step.file.loadFileAsString()).getProperty ("metadata", {});
                    expectEquals (m["modeled_by"].toString(), juce::String ("BellyDSP"));
                    expectEquals ((double) m["input_level_dbu"], 12.0);
                    expectEquals (m["tone_type"].toString(), set.toneType);
                    bool listed = false;
                    if (const auto* files = manifest["files"].getArray())
                        for (const auto& f : *files)
                            if (f["path"].toString() == "models/" + allNames[s] + "/" + step.file.getFileName())
                                listed = f["license"].toString() == "CC BY 4.0" && f["author"].toString() == "Sean Snaider"
                                         && f["notes"].toString().contains (source) && f["notes"].toString().contains ("stand-in");
                    expect (listed, step.file.getFileName());
                }
                expect (gains == juce::StringArray { "0", "2.5", "5", "7.5", "10" }, gains.joinIntoString (", "));
                lines.add (set.name + " (" + set.toneType + ", " + set.description + "): steps at " + gains.joinIntoString (", "));
            }
            logMessage ("  -> " + lines.joinIntoString ("; "));
        }

        beginTest ("the built-in gain sets in the engine: Gain changes the saturation (the nonlinear energy ratio rises step by step), the loudness holds, and the blend's correlation and level");
        {
            const auto input = guitarDI ((int) (6.0 * fs));
            for (size_t s = 0; s < allSets.size(); ++s)
            {
                NamAmp::LoadResult r;
                juce::StringArray rows;
                std::vector<double> loud, crest, nl;
                for (int i = 0; i <= 8; ++i)
                {
                    const auto position = 1.25f * (float) i;
                    NamAmp amp;
                    r = amp.loadModel (allSets[s], true);
                    amp.setGain (position);
                    amp.prepare (fs, blockSize);
                    int maxRunning = 0;
                    const auto out = run (amp, input, [&] (size_t) { maxRunning = std::max (maxRunning, amp.getRunningSteps()); });
                    maxRunning = std::max (maxRunning, amp.getRunningSteps());
                    expectEquals (maxRunning, i % 2 == 0 ? 1 : 2, "one model on a step, two between");
                    loud.push_back (lufs (out));
                    crest.push_back (crestDb (out));
                    nl.push_back (nlRatioDb (input, out));
                    rows.add ("Gain " + positionText (position) + ": " + juce::String (loud.back(), 1) + " LUFS, crest " + juce::String (crest.back(), 1) + " dB, NL " + juce::String (nl.back(), 1) + " dB, "
                              + juce::String (maxRunning) + (maxRunning == 1 ? " model" : " models"));
                }
                expect (r.ok && r.isGainSet, r.message);
                const auto spread = *std::max_element (loud.begin(), loud.end()) - *std::min_element (loud.begin(), loud.end());
                expectLessThan (spread, 1.5, allNames[s]);
                // Gain 0 clearly cleaner than Gain 10, and each step more distorted than the one before it (the sets
                // were voiced in equal NL steps, AG2; 0.3 dB of slack for what the models add). Crest factor isn't the
                // test: a set with power-amp sag (Comet) keeps its crest nearly flat while it saturates.
                expectGreaterThan (nl.back() - nl.front(), 3.0, allNames[s] + ": Gain 0 must be clearly cleaner than Gain 10");
                for (size_t k = 2; k < nl.size(); k += 2)
                    expectGreaterThan (nl[k] - nl[k - 2], -0.3, allNames[s] + " step " + juce::String ((int) k / 2));

                // The blend's level without its correction, from the steps' own renders: the dip of a plain linear
                // crossfade halfway between steps, against the measured correlation's prediction 10 log10((1 + rho) / 2).
                std::vector<std::vector<float>> stepOut;
                for (const auto g : r.stepGains)
                    stepOut.push_back (renderAt (allSets[s], (float) g, input));
                juce::StringArray blend;
                for (size_t k = 0; k + 1 < stepOut.size(); ++k)
                {
                    std::vector<float> mid (input.size());
                    for (size_t n = 0; n < mid.size(); ++n)
                        mid[n] = 0.5f * (stepOut[k][n] + stepOut[k + 1][n]);
                    const auto dip = lufs (mid) - 0.5 * (lufs (stepOut[k]) + lufs (stepOut[k + 1]));
                    const auto predicted = 10.0 * std::log10 ((1.0 + r.stepCorrelation[k]) / 2.0);
                    const auto corrected = loud[2 * k + 1] - 0.5 * (loud[2 * k] + loud[2 * k + 2]);
                    expectGreaterThan (r.stepCorrelation[k], 0.5);
                    expectLessThan (std::abs (corrected), 0.5);
                    blend.add (positionText ((float) r.stepGains[k]) + "-" + positionText ((float) r.stepGains[k + 1]) + ": rho " + juce::String (r.stepCorrelation[k], 3)
                               + ", linear dip " + juce::String (dip, 2) + " LU (predicted " + juce::String (predicted, 2) + "), corrected " + juce::String (corrected, 2) + " LU");
                }
                logMessage ("  -> " + allNames[s] + ": " + rows.joinIntoString ("; "));
                logMessage ("  -> " + allNames[s] + " blend: " + blend.joinIntoString ("; ") + "; loudness spread over Gain " + juce::String (spread, 2) + " LU");
            }
        }

        beginTest ("a step model that starts cold is wrong for its receptive field, then exact: why a model warms up before it's heard");
        {
            ampsim::GainSet set;
            juce::String error;
            expect (ampsim::GainSet::read (sets[2], set, error), error);
            const auto input = guitarDI ((int) (2.0 * fs));
            nam::DspLoadOptions options;
            options.prewarm = false;
            auto always = nam::get_dsp (std::filesystem::path (set.steps[0].file.getFullPathName().toStdString()), options);
            auto restarted = nam::get_dsp (std::filesystem::path (set.steps[0].file.getFullPathName().toStdString()), options);
            always->Reset (fs, blockSize);
            restarted->Reset (fs, blockSize);
            const auto receptiveField = always->GetPrewarmSamples();
            std::vector<float> a (input.size()), b (input.size()), in (input);
            const size_t stopAt = 24000, resumeAt = 38400; // runs 0.5 s, sits out 0.3 s, resumes
            for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
            {
                float* x = in.data() + start;
                float* ya = a.data() + start;
                float* yb = b.data() + start;
                always->process (&x, &ya, blockSize);
                if (start < stopAt || start >= resumeAt)
                    restarted->process (&x, &yb, blockSize);
            }
            double before = 0.0, after = 0.0, peak = 0.0;
            for (size_t n = resumeAt; n < input.size(); ++n)
            {
                const auto d = std::abs ((double) a[n] - b[n]);
                peak = std::max (peak, (double) std::abs (a[n]));
                (n < resumeAt + (size_t) receptiveField ? before : after) = std::max (n < resumeAt + (size_t) receptiveField ? before : after, d);
            }
            expectGreaterThan (before, 1.0e-3 * peak);
            expectLessThan (after, 1.0e-6 * peak);
            logMessage ("  -> " + set.steps[0].file.getFileName() + ": receptive field " + juce::String (receptiveField) + " samples (" + juce::String (1000.0 * receptiveField / fs, 1)
                        + " ms); restarted after 0.3 s off, its output differs from an always-running copy by up to " + dB (toDb (before / peak)) + " of the peak within the receptive field, and "
                        + (after == 0.0 ? juce::String ("not at all (bit-identical)") : "at most " + dB (toDb (after / peak))) + " after it");
        }

        beginTest ("sweeping Gain live across every step: the scheduled models give exactly the blend of always-running ones (no click), at most three at once");
        {
            const auto input = guitarDI ((int) (8.0 * fs));
            const auto numBlocks = input.size() / (size_t) blockSize;
            // The knob, per buffer: still at 0, dragged to 10 over 1 s, held, dragged back to 0 in 0.1 s (faster than
            // the smoothing), a jump to 6.25 (a scene), a jump to 1.25, then a slow wiggle around 5.
            const auto knob = [] (double t) -> float
            {
                if (t < 0.5) return 0.0f;
                if (t < 1.5) return (float) (10.0 * (t - 0.5));
                if (t < 2.5) return 10.0f;
                if (t < 2.6) return (float) (10.0 * (2.6 - t) / 0.1);
                if (t < 3.5) return 0.0f;
                if (t < 4.5) return 6.25f;
                if (t < 5.5) return 1.25f;
                return (float) (5.0 + 4.0 * std::sin (juce::MathConstants<double>::twoPi * (t - 5.5) / 1.5));
            };

            for (size_t s = 0; s < allSets.size(); ++s)
            {
                NamAmp amp;
                const auto r = amp.loadModel (allSets[s], true);
                amp.setGain (0.0f);
                amp.prepare (fs, blockSize);
                std::vector<float> positions;
                std::array<int, 4> runningBlocks {};
                int maxRunning = 0;
                const auto out = run (amp, input, [&] (size_t block)
                {
                    if (block > 0)
                    {
                        const auto& last = amp.getLastPositions();
                        positions.insert (positions.end(), last.begin(), last.begin() + blockSize);
                        const auto running = amp.getRunningSteps();
                        maxRunning = std::max (maxRunning, running);
                        ++runningBlocks[(size_t) juce::jlimit (0, 3, running)];
                    }
                    amp.setGain (knob ((double) block * blockSize / fs));
                });
                const auto& last = amp.getLastPositions();
                positions.insert (positions.end(), last.begin(), last.begin() + blockSize);
                expectEquals (positions.size(), numBlocks * (size_t) blockSize);

                // The reference: every step rendered on its own the whole time (no scheduling, never cold), blended
                // with the same positions and the same law.
                std::vector<std::vector<float>> stepOut;
                for (const auto g : r.stepGains)
                    stepOut.push_back (renderAt (allSets[s], (float) g, input));
                std::vector<float> reference (out.size());
                for (size_t n = 0; n < out.size(); ++n)
                {
                    const auto p = positions[n];
                    size_t k = 0;
                    while (k + 2 < r.stepGains.size() && p >= (float) r.stepGains[k + 1])
                        ++k;
                    const auto a = juce::jlimit (0.0f, 1.0f, (p - (float) r.stepGains[k]) / (float) (r.stepGains[k + 1] - r.stepGains[k]));
                    const auto rho = (float) juce::jlimit (0.0, 1.0, r.stepCorrelation[k]);
                    const auto c = 1.0f / std::sqrt ((1.0f - a) * (1.0f - a) + a * a + 2.0f * a * (1.0f - a) * rho);
                    reference[n] = a <= 0.0f ? stepOut[k][n] : a >= 1.0f ? stepOut[k + 1][n] : c * ((1.0f - a) * stepOut[k][n] + a * stepOut[k + 1][n]);
                }
                const auto peak = std::max (std::abs (*std::max_element (out.begin(), out.end())), std::abs (*std::min_element (out.begin(), out.end())));
                const auto difference = maxAbsDifference (out, reference);
                expectLessThan (difference, 1.0e-5 * (double) peak, allNames[s]);
                expectLessOrEqual (maxRunning, NamAmp::maxRunningSteps);

                // Clicks: the largest sample-to-sample step of the sweep against the largest of any step played alone.
                double staticStep = 0.0;
                for (const auto& o : stepOut)
                    staticStep = std::max (staticStep, maxStep (o));
                const auto sweepStep = maxStep (out);
                expectLessThan (sweepStep, 1.1 * staticStep);

                // How long the position waited for a model to warm up (holding still while the knob was elsewhere).
                int longestWait = 0, wait = 0;
                for (size_t n = 1; n < positions.size(); ++n)
                {
                    const auto target = knob ((double) (n / (size_t) blockSize) * blockSize / fs);
                    wait = (juce::exactlyEqual (positions[n], positions[n - 1]) && std::abs (positions[n] - target) > 0.01f) ? wait + 1 : 0;
                    longestWait = std::max (longestWait, wait);
                }

                juce::AudioBuffer<float> wav (1, (int) out.size());
                wav.copyFrom (0, 0, out.data(), (int) out.size());
                expect (writeWav (proof.getChildFile (allNames[s].toLowerCase() + "_gain_sweep_amp_only.wav"), wav));
                logMessage ("  -> " + allNames[s] + ": live sweep vs the always-running blend: max difference " + dB (toDb (difference / peak))
                            + " of the peak; buffers with 1/2/3 models running: " + juce::String (runningBlocks[1]) + "/" + juce::String (runningBlocks[2]) + "/"
                            + juce::String (runningBlocks[3]) + " (max " + juce::String (maxRunning) + "); largest sample step " + juce::String (sweepStep, 4) + " vs "
                            + juce::String (staticStep, 4) + " for the steps alone (" + juce::String (sweepStep / staticStep, 3) + "x); longest wait for a warm-up "
                            + juce::String (1000.0 * longestWait / fs, 1) + " ms; amp_gain/" + allNames[s].toLowerCase() + "_gain_sweep_amp_only.wav");
            }
        }

        beginTest ("the processor: Gain shows 0 to 10, fresh slots get the gain sets, and old built-in references load the sets");
        {
            AmpSimProcessor p;
            auto* gain = p.parameters.getParameter (AmpSimProcessor::ampParamId (0, "input_trim"));
            expectEquals (gain->getName (64), juce::String ("Amp 1 Gain"));
            expectEquals (gain->getCurrentValueAsText(), juce::String ("5.0"));
            setParam (p, AmpSimProcessor::ampParamId (0, "input_trim"), 4.8f);
            expectEquals (gain->getCurrentValueAsText(), juce::String ("6.0"));
            expectWithinAbsoluteError (gain->convertFrom0to1 (gain->getValueForText ("7.5")), 12.0f, 1.0e-4f);

            WithBuiltInCaptures builtIns;
            AmpSimProcessor fresh;
            waitForLoads (fresh);
            fresh.prepareToPlay (fs, blockSize);
            play (fresh, std::vector<float> (blockSize, 0.0f));
            juce::StringArray status;
            for (int s = 0; s < 3; ++s)
            {
                expectEquals (fresh.parameters.state.getProperty (AmpSimProcessor::modelPathKey (s)).toString(), sets[(size_t) s].getFullPathName());
                expect (fresh.getChain().amp.slot (s).model.hasGainSet());
                expect (fresh.getStatus().model[(size_t) s].contains ("gain set, 5 steps"), fresh.getStatus().model[(size_t) s]);
                status.add (fresh.getStatus().model[(size_t) s]);
            }

            // A preset from before the gain sets names "factory:models/Ember.nam"; a saved state names a path
            // ending in content/models/Monolith.nam.
            const auto oldRef = presets::resolve ({ "factory:models/Ember.nam", "fnv1a64:0123456789abcdef", 297888 }, "models");
            expect (oldRef.found && oldRef.file == sets[1] && ! oldRef.relinked && ! oldRef.changed);
            AmpSimProcessor q;
            juce::MemoryBlock saved;
            q.getStateInformation (saved);
            auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
            xml->setAttribute (AmpSimProcessor::modelPathKey (2).toString(), "/somewhere/BellyDSP.app/Contents/Resources/content/models/Monolith.nam");
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*xml, old);
            AmpSimProcessor restored;
            restored.setStateInformation (old.getData(), (int) old.getSize());
            waitForLoads (restored);
            expectEquals (restored.parameters.state.getProperty (AmpSimProcessor::modelPathKey (2)).toString(), sets[2].getFullPathName());
            expect (! restored.getStatus().modelError[2], restored.getStatus().model[2]);
            logMessage ("  -> Gain reads \"5.0\" at its default and \"6.0\" at +4.8 dB, \"7.5\" types as +12 dB; a fresh start: " + status.joinIntoString ("; ")
                        + "; \"factory:models/Ember.nam\" resolves to Ember's set, a saved .../content/models/Monolith.nam loads Monolith's");
        }

        beginTest ("the Gain knob's step dots: the steps of the slot's set, the one or two it plays lit; none for a single capture (snapshot at 2x)");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            setParam (p, AmpSimProcessor::slotParamId, 2.0f);
            setParam (p, AmpSimProcessor::ampParamId (2, "input_trim"), GainKnob::dbForPosition (6.25f));
            play (p, guitarDI ((int) (0.5 * fs)));
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::amp);
            ed.refresh();
            auto& amp = ed.getAmpView();
            expect (amp.getShownGainSteps() == std::vector<float> { 0.0f, 2.5f, 5.0f, 7.5f, 10.0f });
            expect (amp.getLitGainSteps() == std::vector<bool> { false, false, true, true, false });
            expectEquals (amp.getKnob (2, 0).getValueText(), juce::String (6.25f, 1));
            expect (amp.getModelText() == "Monolith (built in, gain set)", amp.getModelText());
            expectEquals (amp.getVoiceText(), juce::String ("High gain"));
            const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            expect (savePng (image, proof.getChildFile ("editor_amp_monolith_gain_6.25.png")));
            const auto knobArea = editor->getLocalArea (&amp.getKnob (2, 0), amp.getKnob (2, 0).getLocalBounds()).expanded (40);
            expect (savePng (image.getClippedImage (knobArea * 2), proof.getChildFile ("editor_amp_gain_knob_crop.png")));

            setParam (p, AmpSimProcessor::ampParamId (2, "input_trim"), GainKnob::dbForPosition (7.5f));
            ed.refresh();
            const auto onStep = amp.getLitGainSteps();
            expect (onStep == std::vector<bool> { false, false, false, true, false });

            setParam (p, AmpSimProcessor::slotParamId, 0.0f);
            p.loadModel (0, exampleModel ("wavenet_a1_standard.nam"));
            waitForLoads (p);
            p.runHousekeeping();
            ed.refresh();
            expect (amp.getShownGainSteps().empty());
            const auto single = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            expect (savePng (single, proof.getChildFile ("editor_amp_single_capture.png")));
            logMessage ("  -> Monolith at 6.25: dots at 0, 2.5, 5, 7.5, 10 with 5 and 7.5 lit, the knob reads \"" + amp.getKnob (2, 0).getValueText() + "\", the info row \"" + amp.getModelText()
                        + "\"; at 7.5 only 7.5 is lit; a single capture shows no dots. amp_gain/editor_amp_monolith_gain_6.25.png, editor_amp_gain_knob_crop.png, "
                          "editor_amp_single_capture.png");
        }

        beginTest ("factory presets: every slot a preset plays has intentional amp settings, and loading the preset and each scene sets exactly what it stores");
        {
            const auto ids = ampParameterIds();
            juce::StringArray lines;
            for (const auto& preset : presets::factoryPresets())
            {
                const auto name = preset["name"].toString();
                const auto stored = preset["parameters"];
                AmpSimProcessor p;
                expect (p.loadPreset (preset).ok);
                waitForLoads (p);
                p.runHousekeeping();

                // The slots it plays: the preset's own, and every scene's.
                std::set<int> used { juce::roundToInt ((double) stored.getProperty (AmpSimProcessor::slotParamId, 0)) };
                if (const auto* list = preset["scenes"]["list"].getArray())
                    for (const auto& scene : *list)
                        if (scene.isObject() && scene["values"].hasProperty (AmpSimProcessor::slotParamId))
                            used.insert (juce::roundToInt ((double) scene["values"][AmpSimProcessor::slotParamId.toRawUTF8()]));
                for (const auto s : used)
                    for (const auto* knob : { "input_trim", "output_trim", "depth", "bass", "mid", "treble", "presence" })
                        expect (stored.hasProperty (AmpSimProcessor::ampParamId (s, knob)), name + " plays slot " + juce::String (s + 1) + " but doesn't set its " + knob);

                // After loading: exactly the stored value, or the default for what it doesn't store.
                for (const auto& id : ids)
                {
                    auto* param = p.parameters.getParameter (id);
                    const auto expected = stored.hasProperty (id) ? (float) (double) stored[id.toRawUTF8()] : param->convertFrom0to1 (param->getDefaultValue());
                    expectWithinAbsoluteError (getParam (p, id), expected, 0.051f, name + " " + id);
                }

                // Each scene: what it stores is set; the rest of the amp stays as the preset left it.
                juce::StringArray sceneText;
                for (int i = 0; i < Scenes::count; ++i)
                {
                    const auto& scene = p.getScenes().get (i);
                    if (! scene.stored)
                        continue;
                    std::map<juce::String, float> before;
                    for (const auto& id : ids)
                        before[id] = getParam (p, id);
                    expect (p.recallScene (i));
                    int ampValues = 0;
                    for (const auto& id : ids)
                    {
                        if (const auto it = scene.values.find (id); it != scene.values.end())
                        {
                            expectWithinAbsoluteError (getParam (p, id), it->second, 0.051f, name + " scene " + scene.name + " " + id);
                            ++ampValues;
                        }
                        else
                            expectEquals (getParam (p, id), before[id], name + " scene " + scene.name + " " + id);
                    }
                    const auto slot = juce::roundToInt (getParam (p, AmpSimProcessor::slotParamId));
                    sceneText.add (scene.name + " on " + names[(size_t) slot] + " at Gain "
                                   + juce::String (GainKnob::positionForDb (getParam (p, AmpSimProcessor::ampParamId (slot, "input_trim"))), 1)
                                   + (ampValues > 0 ? " (scene sets " + juce::String (ampValues) + " amp knobs)" : juce::String()));
                }
                lines.add (name + ": " + sceneText.joinIntoString (", "));
            }
            logMessage ("  -> " + lines.joinIntoString ("; "));
        }

        beginTest ("user presets round-trip every amp parameter, scenes included");
        {
            const auto ids = ampParameterIds();
            AmpSimProcessor p;
            juce::Random random (7);
            const auto randomise = [&]
            {
                for (const auto& id : ids)
                {
                    auto* param = p.parameters.getParameter (id);
                    param->setValueNotifyingHost (random.nextFloat());
                }
            };
            p.getScenes().setChosen (AmpSimProcessor::ampParamId (1, "input_trim"), true);
            p.getScenes().setChosen (AmpSimProcessor::ampParamId (2, "mid"), true);
            p.getScenes().setChosen (AmpSimProcessor::ampParamId (2, "output_trim"), true);
            randomise();
            p.getScenes().store (0, p.parameters);
            const auto sceneA = p.getScenes().get (0).values;
            randomise();
            p.getScenes().store (1, p.parameters);
            randomise();
            std::map<juce::String, float> now;
            for (const auto& id : ids)
                now[id] = getParam (p, id);

            const auto text = juce::JSON::toString (p.capturePreset ("Round trip"));
            AmpSimProcessor q;
            expect (q.loadPreset (juce::JSON::parse (text)).ok);
            for (const auto& id : ids)
                expectWithinAbsoluteError (getParam (q, id), now[id], 1.0e-4f, id);
            expect (q.getScenes().getChosen().contains (AmpSimProcessor::ampParamId (1, "input_trim")) && q.getScenes().getChosen().size() == 3);
            expect (q.getScenes().get (0).values == sceneA);
            expect (q.recallScene (0));
            for (const auto& id : { AmpSimProcessor::ampParamId (1, "input_trim"), AmpSimProcessor::ampParamId (2, "mid"), AmpSimProcessor::ampParamId (2, "output_trim") })
                expectWithinAbsoluteError (getParam (q, id), sceneA.at (id), 1.0e-4f, id);
            logMessage ("  -> 21 amp parameters (7 per slot) and two scenes holding three of them survive capture -> JSON -> load; recalling scene 1 restores them");
        }

        beginTest ("listening renders: every factory preset's scenes through the whole app with the factory cabs, peaking at or below -1 dBFS on the -6 dBFS DI");
        {
            const auto input = guitarDI ((int) (8.0 * fs));
            const auto folder = proofDir().getChildFile ("presets");
            folder.createDirectory();
            juce::Array<juce::var> levels;
            juce::StringArray table { "| Preset | Scene | Amp | Gain | Bass | Mid | Treble | Presence | Depth | Master | LUFS | Peak dBFS | LUFS, DI at -12 |",
                                      "|---|---|---|---|---|---|---|---|---|---|---|---|---|" };
            for (const auto& preset : presets::factoryPresets())
            {
                const auto name = preset["name"].toString();
                for (int i = 0; i < Scenes::count; ++i)
                {
                    AmpSimProcessor p;
                    expect (p.loadPreset (preset).ok);
                    waitForLoads (p);
                    p.runHousekeeping();
                    if (! p.getScenes().get (i).stored)
                        continue;
                    const auto sceneName = p.getScenes().get (i).name;
                    setParam (p, "output_limit_on", 0.0f); // the safety limiter (a global setting) off: the raw level
                    expect (p.recallScene (i));
                    p.prepareToPlay (fs, blockSize);
                    play (p, std::vector<float> ((size_t) fs, 0.0f)); // the preset's fade in, the knobs' smoothing
                    p.runHousekeeping();
                    const auto out = play (p, input);
                    juce::AudioBuffer<float> stereo (2, (int) out.left.size());
                    stereo.copyFrom (0, 0, out.left.data(), (int) out.left.size());
                    stereo.copyFrom (1, 0, out.right.data(), (int) out.right.size());
                    const auto file = folder.getChildFile (name.removeCharacters (" ") + "_" + sceneName.removeCharacters (" ") + ".wav");
                    expect (writeWav (file, stereo));
                    const auto peak = stereo.getMagnitude (0, stereo.getNumSamples());
                    const auto loud = ampsim::loudness::integrated ({ out.left.data(), out.right.data() }, (int) out.left.size(), fs);
                    expectLessOrEqual ((double) toDb (peak), -1.0, name + " " + sceneName);

                    // The same at -12 dBFS peaks (a lighter touch, or a lower pickup): its loudness, after the
                    // first second (the louder pass's echoes).
                    auto softer = input;
                    for (auto& v : softer)
                        v *= 0.5f;
                    const auto quiet = play (p, softer);
                    const auto skip = (size_t) fs;
                    const auto loud12 = ampsim::loudness::integrated ({ quiet.left.data() + skip, quiet.right.data() + skip }, (int) (quiet.left.size() - skip), fs);

                    const auto slot = juce::roundToInt (getParam (p, AmpSimProcessor::slotParamId));
                    const auto knob = [&] (const char* id, float range) { return juce::String ((getParam (p, AmpSimProcessor::ampParamId (slot, id)) + range) / (2.0f * range) * 10.0f, 1); };
                    table.add ("| " + name + " | " + sceneName + " | " + names[(size_t) slot] + " | " + knob ("input_trim", 24.0f) + " | " + knob ("bass", 12.0f) + " | "
                               + knob ("mid", 12.0f) + " | " + knob ("treble", 12.0f) + " | " + knob ("presence", 12.0f) + " | " + knob ("depth", 12.0f) + " | "
                               + knob ("output_trim", 24.0f) + " | " + juce::String (loud, 1) + " | " + juce::String (toDb (peak), 1) + " | " + juce::String (loud12, 1) + " |");
                    auto* row = new juce::DynamicObject();
                    row->setProperty ("preset", name);
                    row->setProperty ("scene", sceneName);
                    row->setProperty ("slot", slot);
                    row->setProperty ("lufs", loud);
                    row->setProperty ("peak_dbfs", toDb (peak));
                    row->setProperty ("lufs_at_minus_12", loud12);
                    levels.add (juce::var (row));
                }
            }
            for (const auto& row : table)
                logMessage ("  " + row);
            folder.getChildFile ("levels.json").replaceWithText (juce::JSON::toString (juce::var (levels)));
            logMessage ("  -> build/proof/presets/<Preset>_<Scene>.wav: 8 s of the synthetic DI (peaks -6 dBFS) through each scene, the output limiter off; levels.json");
        }
    }
};

static AmpGainTests ampGainTests;

/// Gain sets across the three always-running slots: only the selected slot blends (BUILD_PLAN "Amp gain",
/// ASSUMPTIONS AG15 to AG18).
class AmpGainSlotTests final : public juce::UnitTest
{
public:
    AmpGainSlotTests() : juce::UnitTest ("Amp gain (slot switching)", "ampsim") {}

    void runTest() override
    {
        const std::array<const char*, 3> names { "Glass", "Ember", "Monolith" };
        const auto proof = proofDir().getChildFile ("amp_gain");
        proof.createDirectory();
        std::array<juce::File, 3> sets;
        for (int s = 0; s < 3; ++s)
            sets[(size_t) s] = presets::builtInCapture (s);

        beginTest ("only the selected slot blends: the others play their nearest step alone, and a switch reaches the exact blend after a warm-up, bit-identical, with no click");
        {
            // The three built-in sets in the three slots, every Gain between two steps, and slot switches: slow
            // ones, a run of switches every 13 to 32 ms (inside the 85 ms warm-up), Gain changes on two slots that
            // aren't heard (as a scene or a preset makes: one two steps up, one two steps down), and switches to them.
            const auto input = guitarDI ((int) (9.0 * fs));
            const auto numBlocks = input.size() / (size_t) blockSize;
            const auto blockAt = [] (double seconds) { return (size_t) (seconds * fs / blockSize); };
            const std::array<float, 3> startGain { 6.0f, 3.5f, 8.25f }; // nearest steps 5, 2.5, 7.5
            const auto knob = [&] (int s, size_t block) -> float
            {
                if (block >= blockAt (5.5) && s == 0) return 9.0f; // nearest 10
                if (block >= blockAt (5.5) && s == 2) return 3.0f; // nearest 2.5
                return startGain[(size_t) s];
            };
            std::map<size_t, int> switches { { blockAt (1.0), 1 }, { blockAt (2.0), 2 }, { blockAt (3.0), 0 }, { blockAt (4.5), 1 },
                                             { blockAt (6.5), 0 }, { blockAt (7.5), 2 } };
            const auto rapid = blockAt (3.6);
            for (const auto& [offset, slot] : std::initializer_list<std::pair<size_t, int>> { { 0, 1 }, { 10, 2 }, { 15, 1 }, { 27, 0 }, { 35, 2 }, { 47, 0 }, { 52, 1 }, { 64, 2 } })
                switches[rapid + offset] = slot;

            ampsim::AmpSection section;
            std::array<NamAmp, 3> exact; // per slot: a lone NamAmp blending exactly the whole time, on the same knob
            std::array<NamAmp::LoadResult, 3> info;
            for (int s = 0; s < 3; ++s)
            {
                info[(size_t) s] = section.slot (s).model.loadModel (sets[(size_t) s], true);
                section.slot (s).inputTrim.setPosition (startGain[(size_t) s]);
                exact[(size_t) s].loadModel (sets[(size_t) s], true);
                exact[(size_t) s].setGain (startGain[(size_t) s]);
                exact[(size_t) s].prepare (fs, blockSize);
            }
            section.prepare (fs, blockSize);

            // Every step of every set rendered alone the whole time (always running, never cold).
            std::array<std::vector<std::vector<float>>, 3> stepOut;
            for (int s = 0; s < 3; ++s)
                for (const auto g : info[(size_t) s].stepGains)
                    stepOut[(size_t) s].push_back (renderAt (sets[(size_t) s], (float) g, input));
            const auto nearestOf = [&] (int s, float p)
            {
                size_t k = 0;
                for (size_t i = 1; i < info[(size_t) s].stepGains.size(); ++i)
                    if (std::abs (info[(size_t) s].stepGains[i] - p) < std::abs (info[(size_t) s].stepGains[k] - p))
                        k = i;
                return k;
            };

            std::vector<float> out (numBlocks * (size_t) blockSize);
            std::array<std::vector<float>, 3> exactOut;
            for (auto& e : exactOut)
                e.resize (out.size());
            std::vector<bool> switching (numBlocks);
            std::vector<int> selectedAt (numBlocks);
            std::array<int, 16> histogram {};
            std::array<int, 3> slotMax {};
            int globalMax = 0, switchingMax = 0, unheardMax = 0, nearestBuffers = 0, nearestWrong = 0, holdBuffers = 0, holdWrong = 0;

            // Per stretch between switches, for the selected slot: from which buffer on its output was bit-identical
            // to the exact reference's to the end of the stretch.
            struct Stretch { size_t from = 0, to = 0; int slot = 0; size_t identicalFrom = SIZE_MAX; float distance = 0.0f; };
            std::vector<Stretch> stretches { { 0, 0, 0 } };
            std::array<bool, 3> endedExact {}; // the slot's last stretch selected ended on its exact blend
            std::array<bool, 3> wasMoving { true, true, true }; // after the last buffer: moving, or not in nearest mode
            juce::AudioBuffer<float> buffer (1, blockSize);
            const ampsim::BlockContext context;

            for (size_t b = 0; b < numBlocks; ++b)
            {
                if (const auto it = switches.find (b); it != switches.end())
                {
                    auto& last = stretches.back();
                    last.to = b;
                    endedExact[(size_t) last.slot] = last.identicalFrom < b;
                    section.selectSlot (it->second);
                    const auto p = knob (it->second, b);
                    stretches.push_back ({ b, 0, it->second, SIZE_MAX, (float) std::abs (p - (float) info[(size_t) it->second].stepGains[nearestOf (it->second, p)]) });
                }
                const auto start = b * (size_t) blockSize;
                for (int s = 0; s < 3; ++s)
                {
                    section.slot (s).inputTrim.setPosition (knob (s, b));
                    exact[(size_t) s].setGain (knob (s, b));
                    buffer.copyFrom (0, 0, input.data() + start, blockSize);
                    exact[(size_t) s].process (juce::dsp::AudioBlock<float> (buffer), context);
                    std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize, exactOut[(size_t) s].begin() + (std::ptrdiff_t) start);
                }
                buffer.copyFrom (0, 0, input.data() + start, blockSize);
                section.process (juce::dsp::AudioBlock<float> (buffer), context);
                std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize, out.begin() + (std::ptrdiff_t) start);
                switching[b] = section.isSwitching();
                selectedAt[b] = section.getSelectedSlot();

                int total = 0;
                const auto selected = section.getSelectedSlot();
                for (int s = 0; s < 3; ++s)
                {
                    const auto& model = section.slot (s).model;
                    const auto running = model.getRunningSteps();
                    total += running;
                    slotMax[(size_t) s] = std::max (slotMax[(size_t) s], running);
                    const auto* y = section.getSlotOutput (s);
                    const auto sameAs = [&] (const std::vector<float>& ref) { return std::equal (y, y + blockSize, ref.begin() + (std::ptrdiff_t) start); };

                    if (s != selected)
                        unheardMax = std::max (unheardMax, running);
                    if (s == selected)
                    {
                        auto& now = stretches.back();
                        if (! sameAs (exactOut[(size_t) s]))
                            now.identicalFrom = SIZE_MAX;
                        else if (now.identicalFrom == SIZE_MAX)
                            now.identicalFrom = b;
                    }
                    else if (model.getBlend() == NamAmp::Blend::hold)
                    {
                        // Fading out after a switch away: it must keep playing exactly what it played (its blend).
                        if (endedExact[(size_t) s])
                        {
                            ++holdBuffers;
                            holdWrong += sameAs (exactOut[(size_t) s]) ? 0 : 1;
                        }
                    }
                    else if (! model.isGainMoving() && ! wasMoving[(size_t) s])
                    {
                        // Not heard and settled (since before this buffer: the buffer the position lands on a step
                        // still ran its neighbour): one model, the nearest step, exactly that step alone.
                        const auto k = nearestOf (s, knob (s, b));
                        ++nearestBuffers;
                        nearestWrong += (running == 1 && model.isStepRunning ((int) k) && sameAs (stepOut[(size_t) s][k])) ? 0 : 1;
                    }
                }
                for (int s = 0; s < 3; ++s)
                    wasMoving[(size_t) s] = section.slot (s).model.isGainMoving() || section.slot (s).model.getBlend() != NamAmp::Blend::nearest;
                ++histogram[(size_t) juce::jlimit (0, 15, total)];
                globalMax = std::max (globalMax, total);
                if (b < blockAt (5.5) || b >= blockAt (6.5))
                    switchingMax = std::max (switchingMax, total); // outside the second after the Gain changes
            }
            stretches.back().to = numBlocks;

            // How long the warm-up is: the steps' receptive field, rounded up to whole buffers, and the cross from
            // the nearest step to the blend at the Gain's slew rate.
            nam::DspLoadOptions options;
            options.prewarm = false;
            ampsim::GainSet monolith;
            juce::String error;
            expect (ampsim::GainSet::read (sets[2], monolith, error), error);
            const auto receptiveField = nam::get_dsp (std::filesystem::path (monolith.steps[0].file.getFullPathName().toStdString()), options)->GetPrewarmSamples();
            const auto warmBlocks = (size_t) ((receptiveField + blockSize - 1) / blockSize);

            juce::StringArray reached;
            int longStretches = 0;
            double longestDelayMs = 0.0;
            for (const auto& st : stretches)
            {
                if (st.from == 0)
                {
                    expectEquals ((int) st.identicalFrom, 0, "the slot selected from the start blends exactly from its first buffer");
                    continue;
                }
                const auto crossBlocks = (size_t) std::ceil (st.distance / NamAmp::gainSlewPerSecond * fs / blockSize);
                const auto bound = warmBlocks + crossBlocks + 1;
                if (st.to - st.from <= bound + 2)
                    continue; // a rapid switch: left before it could get there
                ++longStretches;
                expect (st.identicalFrom != SIZE_MAX && st.identicalFrom - st.from <= bound,
                        "slot " + juce::String (st.slot + 1) + " selected at buffer " + juce::String ((int) st.from) + " never reached its exact blend in time");
                const auto delayMs = st.identicalFrom == SIZE_MAX ? -1.0 : 1000.0 * (double) (st.identicalFrom - st.from) * blockSize / fs;
                longestDelayMs = std::max (longestDelayMs, delayMs);
                reached.add (juce::String (names[(size_t) st.slot]) + " " + juce::String (delayMs, 1) + " ms (bound " + juce::String (1000.0 * (double) bound * blockSize / fs, 1) + ")");
            }
            expectEquals (longStretches, 7);
            expectEquals (nearestWrong, 0, "a slot that isn't heard must run its nearest step alone");
            expectGreaterThan (nearestBuffers, (int) numBlocks); // most buffers have two such slots
            expectEquals (holdWrong, 0, "a slot fading out must keep playing its blend");
            expectGreaterThan (holdBuffers, 30);
            for (int s = 0; s < 3; ++s)
                expectLessOrEqual (slotMax[(size_t) s], NamAmp::maxRunningSteps);
            expectLessOrEqual (unheardMax, NamAmp::maxRunningStepsUnheard);

            // The exact references themselves against the steps rendered alone and blended by hand (the sweep test's
            // brute force), over the first second (every slot at its starting Gain).
            double bruteForce = 0.0;
            for (int s = 0; s < 3; ++s)
            {
                const auto& g = info[(size_t) s].stepGains;
                const auto p = startGain[(size_t) s];
                size_t k = 0;
                while (k + 2 < g.size() && p >= (float) g[k + 1])
                    ++k;
                const auto a = (p - (float) g[k]) / (float) (g[k + 1] - g[k]);
                const auto rho = (float) juce::jlimit (0.0, 1.0, info[(size_t) s].stepCorrelation[k]);
                const auto c = 1.0f / std::sqrt ((1.0f - a) * (1.0f - a) + a * a + 2.0f * a * (1.0f - a) * rho);
                double peak = 0.0, worst = 0.0;
                for (size_t n = 0; n < (size_t) fs; ++n)
                {
                    const auto brute = c * ((1.0f - a) * stepOut[(size_t) s][k][n] + a * stepOut[(size_t) s][k + 1][n]);
                    worst = std::max (worst, (double) std::abs (exactOut[(size_t) s][n] - brute));
                    peak = std::max (peak, (double) std::abs (brute));
                }
                bruteForce = std::max (bruteForce, worst / std::max (1.0e-9, peak));
            }
            expectLessThan (bruteForce, 1.0e-5);

            // Clicks: the largest sample-to-sample step during the slot fades and outside them (where the newly
            // selected slot crosses from its nearest step to the blend, and the Gain changes land), against the
            // largest of any step played alone; the tolerances are the slot-switch redirect test's (1.6x) and the
            // Gain sweep's (1.1x).
            // Outside the fades only the selected slot is heard, so its steps are compared with its own amp's.
            std::array<double, 3> ampStep {};
            for (int s = 0; s < 3; ++s)
                for (const auto& o : stepOut[(size_t) s])
                    ampStep[(size_t) s] = std::max (ampStep[(size_t) s], maxStep (o));
            const auto staticStep = *std::max_element (ampStep.begin(), ampStep.end());
            double duringFades = 0.0, outsideRatio = 0.0;
            for (size_t n = 1; n < out.size(); ++n)
            {
                const auto d = (double) std::abs (out[n] - out[n - 1]);
                const auto b = n / (size_t) blockSize;
                if (switching[b] || switching[(n - 1) / (size_t) blockSize])
                    duringFades = std::max (duringFades, d);
                else
                    outsideRatio = std::max (outsideRatio, d / ampStep[(size_t) selectedAt[b]]);
            }
            expectLessThan (duringFades, 1.6 * staticStep);
            expectLessThan (outsideRatio, 1.1);

            juce::StringArray hist;
            for (int m = 0; m < 16; ++m)
                if (histogram[(size_t) m] > 0)
                    hist.add (juce::String (m) + ": " + juce::String (histogram[(size_t) m]));
            // Bounds: the selected slot 2 (its blend, or its nearest step and the next warming), the one fading out or
            // dropping to its nearest step 2, the third 1; and with the Gains of both slots that aren't heard changing
            // at once, 2 each.
            expectLessOrEqual (switchingMax, 5);
            expectLessOrEqual (globalMax, 6);

            juce::AudioBuffer<float> wav (1, (int) out.size());
            wav.copyFrom (0, 0, out.data(), (int) out.size());
            expect (writeWav (proof.getChildFile ("slot_switching_gain_sets.wav"), wav));
            logMessage ("  -> " + juce::String ((int) switches.size()) + " switches across Glass 6, Ember 3.5, Monolith 8.25 (then Glass 9, Monolith 3 while not heard), "
                        "8 of them 13 to 32 ms apart: after each switch it stayed on, the new slot was bit-identical to a slot that had blended the whole time after "
                        + reached.joinIntoString (", ") + " (warm-up " + juce::String (receptiveField) + " samples, " + juce::String ((int) warmBlocks) + " buffers, plus the cross at "
                        + juce::String (NamAmp::gainSlewPerSecond, 0) + " positions/s)");
            logMessage ("  -> slots not heard: " + juce::String (nearestBuffers) + " settled slot-buffers, every one a single model at its nearest step, bit-identical to that step alone; "
                        "fading out after a switch: " + juce::String (holdBuffers) + " slot-buffers bit-identical to the blend it had; the references vs the steps blended by hand "
                        + dB (toDb (bruteForce)) + " of the peak");
            logMessage ("  -> models running per buffer, all three slots (count: buffers): " + hist.joinIntoString (", ") + "; max " + juce::String (switchingMax) + " while switching, "
                        + juce::String (globalMax) + " while both unheard slots' Gains changed"
                        + " (per slot " + juce::String (slotMax[0]) + "/" + juce::String (slotMax[1]) + "/" + juce::String (slotMax[2]) + "; a slot not selected at most " + juce::String (unheardMax) + ")");
            logMessage ("  -> largest sample step during the slot fades " + juce::String (duringFades, 4) + ", " + juce::String (duringFades / staticStep, 3)
                        + "x the largest of any step alone (" + juce::String (staticStep, 4) + "); outside them, against the playing amp's steps alone (Glass "
                        + juce::String (ampStep[0], 4) + ", Ember " + juce::String (ampStep[1], 4) + ", Monolith " + juce::String (ampStep[2], 4) + "), at most "
                        + juce::String (outsideRatio, 3) + "x; amp_gain/slot_switching_gain_sets.wav");
        }
    }
};

static AmpGainSlotTests ampGainSlotTests;
} // namespace
