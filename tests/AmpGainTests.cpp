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
        std::array<juce::File, 3> sets;
        for (int s = 0; s < 3; ++s)
            sets[(size_t) s] = presets::builtInCapture (s);

        beginTest ("the built-in gain sets: five steps each, with metadata and CC BY 4.0 manifest entries");
        {
            const auto manifest = juce::JSON::parse (juce::File (AMPSIM_SOURCE_DIR).getChildFile ("content/manifest.json").loadFileAsString());
            juce::StringArray lines;
            for (int s = 0; s < 3; ++s)
            {
                expectEquals (presets::builtInCapturePath (s), "factory:models/" + juce::String (names[(size_t) s]) + "/gainset.json");
                ampsim::GainSet set;
                juce::String error;
                expect (ampsim::GainSet::read (sets[(size_t) s], set, error), error);
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
                            if (f["path"].toString() == "models/" + juce::String (names[(size_t) s]) + "/" + step.file.getFileName())
                                listed = f["license"].toString() == "CC BY 4.0" && f["author"].toString() == "Sean Snaider"
                                         && f["notes"].toString().contains ("prototypes/amp_sim.py") && f["notes"].toString().contains ("stand-in");
                    expect (listed, step.file.getFileName());
                }
                expect (gains == juce::StringArray { "0", "2.5", "5", "7.5", "10" }, gains.joinIntoString (", "));
                lines.add (set.name + " (" + set.toneType + ", " + set.description + "): steps at " + gains.joinIntoString (", "));
            }
            logMessage ("  -> " + lines.joinIntoString ("; "));
        }

        beginTest ("the built-in gain sets in the engine: Gain changes the saturation, the loudness holds, and the blend's correlation and level");
        {
            const auto input = guitarDI ((int) (6.0 * fs));
            for (int s = 0; s < 3; ++s)
            {
                NamAmp::LoadResult r;
                juce::StringArray rows;
                std::vector<double> loud, crest;
                for (int i = 0; i <= 8; ++i)
                {
                    const auto position = 1.25f * (float) i;
                    NamAmp amp;
                    r = amp.loadModel (sets[(size_t) s], true);
                    amp.setGain (position);
                    amp.prepare (fs, blockSize);
                    int maxRunning = 0;
                    const auto out = run (amp, input, [&] (size_t) { maxRunning = std::max (maxRunning, amp.getRunningSteps()); });
                    maxRunning = std::max (maxRunning, amp.getRunningSteps());
                    expectEquals (maxRunning, i % 2 == 0 ? 1 : 2, "one model on a step, two between");
                    loud.push_back (lufs (out));
                    crest.push_back (crestDb (out));
                    rows.add ("Gain " + positionText (position) + ": " + juce::String (loud.back(), 1) + " LUFS, crest " + juce::String (crest.back(), 1) + " dB, "
                              + juce::String (maxRunning) + (maxRunning == 1 ? " model" : " models"));
                }
                expect (r.ok && r.isGainSet, r.message);
                const auto spread = *std::max_element (loud.begin(), loud.end()) - *std::min_element (loud.begin(), loud.end());
                expectLessThan (spread, 1.5, names[(size_t) s]);
                expectGreaterThan (crest.front() - crest.back(), 1.0, "Gain 0 must be clearly cleaner than Gain 10");

                // The blend's level without its correction, from the steps' own renders: the dip of a plain linear
                // crossfade halfway between steps, against the measured correlation's prediction 10 log10((1 + rho) / 2).
                std::vector<std::vector<float>> stepOut;
                for (const auto g : r.stepGains)
                    stepOut.push_back (renderAt (sets[(size_t) s], (float) g, input));
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
                logMessage ("  -> " + juce::String (names[(size_t) s]) + ": " + rows.joinIntoString ("; "));
                logMessage ("  -> " + juce::String (names[(size_t) s]) + " blend: " + blend.joinIntoString ("; ") + "; loudness spread over Gain " + juce::String (spread, 2) + " LU");
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
                        + " ms); restarted after 0.3 s off, its output differs from an always-running copy by up to " + dB (before / peak) + " of the peak within the receptive field, and "
                        + (after == 0.0 ? juce::String ("not at all (bit-identical)") : "at most " + dB (after / peak)) + " after it");
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

            for (int s = 0; s < 3; ++s)
            {
                NamAmp amp;
                const auto r = amp.loadModel (sets[(size_t) s], true);
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
                    stepOut.push_back (renderAt (sets[(size_t) s], (float) g, input));
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
                expectLessThan (difference, 1.0e-5 * (double) peak, names[(size_t) s]);
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
                expect (writeWav (proof.getChildFile (juce::String (names[(size_t) s]).toLowerCase() + "_gain_sweep_amp_only.wav"), wav));
                logMessage ("  -> " + juce::String (names[(size_t) s]) + ": live sweep vs the always-running blend: max difference " + dB (difference / peak)
                            + " of the peak; buffers with 1/2/3 models running: " + juce::String (runningBlocks[1]) + "/" + juce::String (runningBlocks[2]) + "/"
                            + juce::String (runningBlocks[3]) + " (max " + juce::String (maxRunning) + "); largest sample step " + juce::String (sweepStep, 4) + " vs "
                            + juce::String (staticStep, 4) + " for the steps alone (" + juce::String (sweepStep / staticStep, 3) + "x); longest wait for a warm-up "
                            + juce::String (1000.0 * longestWait / fs, 1) + " ms; amp_gain/" + juce::String (names[(size_t) s]).toLowerCase() + "_gain_sweep_amp_only.wav");
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
            expectEquals (amp.getKnob (2, 0).getValueText(), juce::String ("6.3"));
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
            logMessage ("  -> Monolith at 6.25: dots at 0, 2.5, 5, 7.5, 10 with 5 and 7.5 lit, the knob reads 6.3, the info row \"" + amp.getModelText()
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
            juce::StringArray table { "| Preset | Scene | Amp | Gain | Bass | Mid | Treble | Presence | Depth | Master | LUFS | Peak dBFS |",
                                      "|---|---|---|---|---|---|---|---|---|---|---|---|" };
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
                    const auto slot = juce::roundToInt (getParam (p, AmpSimProcessor::slotParamId));
                    const auto knob = [&] (const char* id, float range) { return juce::String ((getParam (p, AmpSimProcessor::ampParamId (slot, id)) + range) / (2.0f * range) * 10.0f, 1); };
                    table.add ("| " + name + " | " + sceneName + " | " + names[(size_t) slot] + " | " + knob ("input_trim", 24.0f) + " | " + knob ("bass", 12.0f) + " | "
                               + knob ("mid", 12.0f) + " | " + knob ("treble", 12.0f) + " | " + knob ("presence", 12.0f) + " | " + knob ("depth", 12.0f) + " | "
                               + knob ("output_trim", 24.0f) + " | " + juce::String (loud, 1) + " | " + juce::String (toDb (peak), 1) + " |");
                }
            }
            for (const auto& row : table)
                logMessage ("  " + row);
            logMessage ("  -> build/proof/presets/<Preset>_<Scene>.wav: 8 s of the synthetic DI (peaks -6 dBFS) through each scene, the limiter (if any) off");
        }
    }
};

static AmpGainTests ampGainTests;
} // namespace
