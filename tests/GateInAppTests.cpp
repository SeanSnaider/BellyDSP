// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The noise gate in the app (Sean's play test, 2026-10-04: "all the individual parts seem to function as
// intended, other than the noise gate"). Two questions: does the Amp page's Gate group do anything, and how
// does Gate A at its defaults behave on a realistically noisy guitar (single-coil hum and hiss) through the
// high-gain capture?

#include "BuiltInCaptures.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "platform/AppSettings.h"
#include "ui/AmpView.h"
#include "ui/MainPages.h"
#include "ui/Pages.h"

#include <cmath>
#include <random>

namespace
{
using namespace testing;

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

/// A pickup's noise floor at `rmsDb` dBFS: half its power mains buzz (60 Hz and its harmonics to 900 Hz at
/// 1/k amplitudes, random phases: a single coil's hum is rich in harmonics) and half hiss (white).
std::vector<float> pickupNoise (int numSamples, double rmsDb, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> phase (0.0, juce::MathConstants<double>::twoPi);
    std::normal_distribution<double> gauss (0.0, 1.0);
    std::vector<double> hum ((size_t) numSamples, 0.0), hiss ((size_t) numSamples);
    for (int k = 1; k <= 15; ++k)
    {
        const auto ph = phase (rng);
        for (int n = 0; n < numSamples; ++n)
            hum[(size_t) n] += std::sin (juce::MathConstants<double>::twoPi * 60.0 * k * n / fs + ph) / k;
    }
    for (auto& h : hiss)
        h = gauss (rng);
    const auto power = [] (const std::vector<double>& x)
    {
        double s = 0.0;
        for (auto v : x)
            s += v * v;
        return s / (double) x.size();
    };
    const auto target = std::pow (10.0, rmsDb / 20.0);
    const auto gh = target * std::sqrt (0.5 / power (hum)), gs = target * std::sqrt (0.5 / power (hiss));
    std::vector<float> out ((size_t) numSamples);
    for (size_t n = 0; n < out.size(); ++n)
        out[n] = (float) (gh * hum[n] + gs * hiss[n]);
    return out;
}

struct Run
{
    std::vector<float> left, gain; // the output, and Gate A's gain per sample
    std::vector<float> detectorDb; // Gate A's detector level per buffer
};

Run play (AmpSimProcessor& p, const std::vector<float>& input)
{
    Run r;
    r.left.resize (input.size());
    r.gain.assign (input.size(), 1.0f);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize, r.left.begin() + (long) start);
        const auto& gate = p.getChain().gateA;
        if (gate.getGainCurveLength() == blockSize)
            std::copy (gate.getGainCurve(), gate.getGainCurve() + blockSize, r.gain.begin() + (long) start);
        r.detectorDb.push_back (p.getGateMeter (false).detectorDb);
    }
    return r;
}

double rmsDb (const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0.0;
    for (size_t i = from; i < to; ++i)
        s += (double) x[i] * x[i];
    return 10.0 * std::log10 (s / (double) (to - from) + 1.0e-30);
}

/// Gate A on a riff with gaps: the share of the gaps (from 300 ms after each stop) it holds closed, and the
/// notes it cuts (dips below -6 dB while the riff itself isn't silent, its last millisecond aside: the next
/// pick, which the 0.5 ms attack is still opening on).
struct GapStats
{
    double closedShare = 0.0;
    int falseCloses = 0, dipsInRests = 0;
};

GapStats gapStats (const Run& on, const std::vector<float>& riff, size_t phrase, size_t cycle, int cycles)
{
    GapStats r;
    size_t closed = 0, total = 0;
    for (size_t c = 0; c < (size_t) cycles; ++c)
    {
        for (size_t i = c * cycle + phrase + (size_t) (0.3 * fs); i < (c + 1) * cycle; ++i, ++total)
            closed += on.gain[i] < 0.01f ? 1 : 0;
        size_t run = 0;
        const auto first = c * cycle + (size_t) (0.05 * fs), last = c * cycle + phrase - (size_t) (0.03 * fs);
        for (size_t i = first; i <= last; ++i)
        {
            if (i < last && on.gain[i] < 0.5f)
            {
                ++run;
                continue;
            }
            if (run > 0)
            {
                const auto spanEnd = i - std::min (run, (size_t) 48);
                double energy = 0.0;
                for (size_t k = i - run; k < spanEnd; ++k)
                    energy += (double) riff[k - c * cycle] * riff[k - c * cycle];
                const auto rmsDb = 10.0 * std::log10 (energy / (double) std::max ((size_t) 1, spanEnd - (i - run)) + 1.0e-24);
                (rmsDb < -40.0 ? r.dipsInRests : r.falseCloses) += 1;
                run = 0;
            }
        }
    }
    r.closedShare = (double) closed / (double) std::max ((size_t) 1, total);
    return r;
}

void pumpPreset (AmpSimProcessor& p)
{
    waitForLoads (p);
    for (int i = 0; i < 100 && p.isChangingPreset(); ++i)
    {
        juce::Thread::sleep (10);
        p.runHousekeeping();
    }
    waitForLoads (p);
    p.runHousekeeping();
}

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> at)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                             juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, at, now, 1, false);
}

/// A plain left click in the middle of a component, as the mouse delivers it.
void click (juce::Component& c)
{
    const auto centre = c.getLocalBounds().getCentre().toFloat();
    c.mouseDown (mouseEvent (c, centre));
    c.mouseUp (mouseEvent (c, centre));
}

/// The switch attached to `id` inside `root` (the parameterId tag every attached control carries).
ui::Switch* findSwitch (juce::Component& root, const juce::String& id)
{
    for (auto* child : root.getChildren())
    {
        if (auto* sw = dynamic_cast<ui::Switch*> (child); sw != nullptr && child->getProperties()[ui::parameterIdProperty].toString() == id)
            return sw;
        if (auto* found = findSwitch (*child, id))
            return found;
    }
    return nullptr;
}

/// Runs the processor's timer until `done` or the time runs out (the auto-Learn's lead-in is wall-clock time).
template <typename Done>
bool housekeepUntil (AmpSimProcessor& p, double timeoutMs, Done done)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
    {
        juce::Thread::sleep (10);
        p.runHousekeeping();
    }
    return done();
}
} // namespace

class GateInAppTests final : public juce::UnitTest
{
public:
    GateInAppTests() : juce::UnitTest ("Gate in the app", "ampsim") {}

    void runTest() override
    {
        beginTest ("the Amp page's Gate knobs are Gate A's, which starts off: turned while it's off they change nothing; switched on, they act");
        {
            const auto input = [&]
            {
                auto x = guitarDI ((int) (3.0 * fs));
                const auto noise = pickupNoise ((int) x.size(), -62.0, 1);
                for (size_t i = (size_t) (2.0 * fs); i < x.size(); ++i)
                    x[i] = 0.0f; // the last second: strings muted
                for (size_t i = 0; i < x.size(); ++i)
                    x[i] += noise[i];
                return x;
            }();
            const auto render = [&] (bool on, float thresholdDb, float releaseMs)
            {
                AmpSimProcessor p; // empty slots, no cab: the gate's own effect, plainly
                setParam (p, "gate_a_on", on ? 1.0f : 0.0f);
                setParam (p, "gate_a_threshold", thresholdDb);
                setParam (p, "gate_a_release", releaseMs);
                p.prepareToPlay (fs, blockSize);
                return play (p, input).left;
            };
            AmpSimProcessor fresh;
            const auto defaultOn = fresh.parameters.getRawParameterValue ("gate_a_on")->load();
            const auto offDefault = render (false, -55.0f, 250.0f), offTurned = render (false, -30.0f, 2000.0f);
            const auto onDefault = render (true, -55.0f, 250.0f), onTurned = render (true, -30.0f, 2000.0f);
            expectEquals (defaultOn, 0.0f);
            expect (offDefault == offTurned);
            expect (onDefault != onTurned);
            const auto gapFrom = (size_t) (2.5 * fs), gapTo = input.size();
            logMessage ("  -> gate_a_on starts at " + juce::String (defaultOn, 0) + " (off). Off: Threshold -55 -> -30 dB and Release 250 -> 2000 ms give the same output bit for bit. "
                        "On: the muted second measures " + juce::String (rmsDb (onDefault, gapFrom, gapTo), 1) + " dBFS at the defaults and "
                        + juce::String (rmsDb (onTurned, gapFrom, gapTo), 1) + " dBFS turned (" + juce::String (rmsDb (offDefault, gapFrom, gapTo), 1)
                        + " dBFS off). So until now the strip's knobs did nothing unless Gate A was switched on from the Pre FX page");
        }

        beginTest ("the defaults: both gates open at -45 dBFS (8 dB of hysteresis: close at -53), in the block and the parameters, IDs unchanged");
        {
            AmpSimProcessor p;
            const ampsim::Gate::Settings d;
            expectEquals (d.thresholdDb, -45.0f);
            expectEquals (d.hysteresisDb, 8.0f);
            for (const auto* id : { "gate_a_threshold", "gate_b_threshold" })
            {
                auto* param = p.parameters.getParameter (id);
                expect (param != nullptr, id);
                expectWithinAbsoluteError (param->convertFrom0to1 (param->getDefaultValue()), -45.0f, 1.0e-4f);
                expectWithinAbsoluteError (p.parameters.getRawParameterValue (id)->load(), -45.0f, 1.0e-4f);
            }
            ampsim::Gate g;
            g.prepare (fs, blockSize);
            expectEquals (g.getOpenThresholdDb(), -45.0f);
            expectEquals (g.getCloseThresholdDb(), -53.0f);
            logMessage ("  -> Gate::Settings and gate_a_threshold / gate_b_threshold default to -45 dBFS (was -55); a fresh gate's meters read open -45, close -53");
        }

        beginTest ("Gate A at the old default (-55), the new one (-45), and after Learn, on a noisy pickup (hum and hiss at -70, -65, -60 dBFS RMS) through Monolith: gaps, chatter, playing");
        {
            std::unique_ptr<AmpSimProcessor> owner;
            {
                WithBuiltInCaptures builtIns;
                owner = std::make_unique<AmpSimProcessor>();
            }
            auto& p = *owner;
            const auto cab = presets::resolve (presets::FileRef::fromVar (presets::factoryPresets()[2]["cab"]["mic1"]), "irs").file;
            p.loadCabIR (0, cab);
            waitForLoads (p);
            setParam (p, AmpSimProcessor::ampModelParamId, 2.0f);

            // Three times: the 2 s riff (peaks -6 dBFS), then 1.5 s of muted strings; the noise under all of it.
            const auto phrase = (size_t) (2.0 * fs), gap = (size_t) (1.5 * fs), cycle = phrase + gap;
            const auto riff = guitarDI ((int) phrase);
            juce::StringArray rows, newDefaultClosed;
            int totalFalseCloses = 0;
            for (const auto noiseDb : { -70.0, -65.0, -60.0 })
            {
                std::vector<float> input (3 * cycle, 0.0f);
                const auto noise = pickupNoise ((int) input.size(), noiseDb, 7);
                for (size_t c = 0; c < 3; ++c)
                    std::copy (riff.begin(), riff.end(), input.begin() + (long) (c * cycle));
                for (size_t i = 0; i < input.size(); ++i)
                    input[i] += noise[i];

                const auto measure = [&] (const juce::String& label, const Run& on, const Run& off)
                {
                    // In the gaps (from 300 ms after each stop, so the release is over): how often the gate is
                    // closed, how often it opens (chatter), and the noise left at the output against the gate off.
                    size_t closed = 0, total = 0, opens = 0, cutWhilePlaying = 0, playing = 0, dipsInRests = 0, falseCloses = 0, longestDip = 0;
                    float riffAtDipDb = -200.0f;
                    double gapOn = 0.0, gapOff = 0.0;
                    for (size_t c = 0; c < 3; ++c)
                    {
                        const auto from = c * cycle + phrase + (size_t) (0.3 * fs), to = (c + 1) * cycle;
                        for (size_t i = from; i < to; ++i)
                        {
                            closed += on.gain[i] < 0.01f ? 1 : 0;
                            opens += (on.gain[i] >= 0.5f && on.gain[i - 1] < 0.5f) ? 1 : 0;
                            gapOn += (double) on.left[i] * on.left[i];
                            gapOff += (double) off.left[i] * off.left[i];
                            ++total;
                        }
                        // While playing (the riff, up to 30 ms before its stop): samples the gate turned down by 6 dB or more,
                        // and each dip (a run of them) sorted by what the riff itself does there: its noise-free RMS under
                        // the dip. Under -40 dBFS (a released note's last few ms and the 30 ms rests between the palm-muted
                        // chugs), the dip is the gate doing its job; otherwise it's a false close, cutting a note.
                        size_t run = 0;
                        const auto first = c * cycle + (size_t) (0.05 * fs), last = c * cycle + phrase - (size_t) (0.03 * fs);
                        for (size_t i = first; i <= last; ++i)
                        {
                            const auto down = i < last && on.gain[i] < 0.5f;
                            if (down)
                            {
                                ++cutWhilePlaying;
                                ++run;
                                ++playing;
                                continue;
                            }
                            if (run > 0)
                            {
                                // The riff's RMS under the dip, leaving out its last millisecond: the next pick's
                                // first samples, which the gate's 0.5 ms raised-cosine attack is still opening on.
                                const auto spanEnd = i - std::min (run, (size_t) 48);
                                double energy = 0.0;
                                for (size_t k = i - run; k < spanEnd; ++k)
                                    energy += (double) riff[k - c * cycle] * riff[k - c * cycle];
                                const auto rmsDb = (float) (10.0 * std::log10 (energy / (double) std::max ((size_t) 1, spanEnd - (i - run)) + 1.0e-24));
                                (rmsDb < -40.0f ? dipsInRests : falseCloses) += 1;
                                longestDip = std::max (longestDip, run);
                                riffAtDipDb = std::max (riffAtDipDb, rmsDb);
                                run = 0;
                            }
                            playing += i < last ? 1 : 0;
                        }
                    }
                    std::vector<float> detector;
                    for (size_t b = 0; b < on.detectorDb.size(); ++b)
                    {
                        const auto i = b * blockSize;
                        if (i % cycle > phrase + (size_t) (0.3 * fs))
                            detector.push_back (on.detectorDb[b]);
                    }
                    std::sort (detector.begin(), detector.end());
                    const auto median = detector.empty() ? -200.0f : detector[detector.size() / 2];
                    const auto p95 = detector.empty() ? -200.0f : detector[(size_t) (0.95 * (double) (detector.size() - 1))];
                    const auto closedShare = (double) closed / (double) juce::jmax ((size_t) 1, total);
                    rows.add ("noise " + juce::String (noiseDb, 0) + " dBFS, " + label + ": detector in the gaps median " + juce::String (median, 1) + " / 95% "
                              + juce::String (p95, 1) + " dBFS; gate closed " + juce::String (100.0 * closedShare, 2) + "% of the gaps, opened "
                              + juce::String (opens) + " times in them; output noise in the gaps " + juce::String (10.0 * std::log10 (gapOn / (double) total + 1.0e-30), 1)
                              + " dBFS vs " + juce::String (10.0 * std::log10 (gapOff / (double) total + 1.0e-30), 1) + " off; turned down while playing "
                              + juce::String (100.0 * (double) cutWhilePlaying / (double) playing, 2) + "% of the time: " + juce::String ((int) dipsInRests)
                              + " dips in the riff's own rests, " + juce::String ((int) falseCloses) + " false closes (the longest dip "
                              + juce::String (1000.0 * (double) longestDip / fs, 1) + " ms, the riff's loudest RMS under any dip " + juce::String (riffAtDipDb, 1) + " dBFS)");
                    totalFalseCloses += (int) falseCloses;
                    return closedShare;
                };

                const auto runWith = [&] (bool gateOn)
                {
                    setParam (p, "gate_a_on", gateOn ? 1.0f : 0.0f);
                    p.runHousekeeping();
                    p.prepareToPlay (fs, blockSize);
                    return play (p, input);
                };
                setParam (p, "gate_a_threshold", -55.0f);
                const auto off = runWith (false);
                const auto atOldDefault = measure ("old default threshold -55", runWith (true), off);
                setParam (p, "gate_a_threshold", -45.0f);
                const auto atNewDefault = measure ("new default threshold -45", runWith (true), off);
                newDefaultClosed.add (juce::String (noiseDb, 0) + " dBFS: " + juce::String (100.0 * atNewDefault, 2) + "% (old default "
                                      + juce::String (100.0 * atOldDefault, 2) + "%)");

                // Learn: 2 s of the muted strings, then the threshold it wrote.
                setParam (p, "gate_a_on", 1.0f);
                p.prepareToPlay (fs, blockSize);
                p.learnGates();
                const auto muted = std::vector<float> (noise.begin(), noise.begin() + (long) (2.2 * fs));
                play (p, muted);
                p.runHousekeeping();
                const auto learned = p.parameters.getRawParameterValue ("gate_a_threshold")->load();
                const auto afterLearn = measure ("after Learn (threshold " + juce::String (learned, 1) + ")", runWith (true), off);
                expectGreaterThan (afterLearn, 0.9);
                if (noiseDb <= -70.0)
                    expectGreaterThan (atNewDefault, 0.9, "the new default must close the gaps on a -70 dBFS RMS floor");
            }
            setParam (p, "gate_a_threshold", -45.0f);
            logMessage ("  -> " + rows.joinIntoString ("\n  -> "));
            expectEquals (totalFalseCloses, 0, "no setting may cut a note while playing");
            logMessage ("  -> closed in the gaps at the new default: " + newDefaultClosed.joinIntoString ("; ") + "; false closes while playing, all runs: "
                        + juce::String (totalFalseCloses));
        }

        beginTest ("factory presets that switch a gate on keep the threshold they were made with (-55 dBFS, now explicit): it closes their gaps on a quiet pickup and cuts no notes");
        {
            const auto phrase = (size_t) (2.0 * fs), gap = (size_t) (1.5 * fs), cycle = phrase + gap;
            const auto riff = guitarDI ((int) phrase);
            juce::StringArray lines;
            int checked = 0;
            for (const auto& preset : presets::factoryPresets())
            {
                // Where the preset has Gate A on: its own settings, or the first scene that switches it on.
                const auto name = preset["name"].toString();
                int scene = -1;
                if ((double) preset["parameters"].getProperty ("gate_a_on", 0.0) < 0.5)
                    if (const auto* list = preset["scenes"]["list"].getArray())
                        for (int i = 0; i < list->size() && scene < 0; ++i)
                            if ((*list)[i].isObject() && (double) (*list)[i]["values"].getProperty ("gate_a_on", 0.0) >= 0.5)
                                scene = i;
                const auto gateOn = (double) preset["parameters"].getProperty ("gate_a_on", 0.0) >= 0.5 || scene >= 0;
                const auto stores = preset["parameters"].hasProperty ("gate_a_threshold");
                if (! gateOn)
                {
                    lines.add (name + ": no gate");
                    continue;
                }
                ++checked;
                expect (stores, name + " switches a gate on, so it must store its threshold");

                std::unique_ptr<AmpSimProcessor> owner;
                {
                    WithBuiltInCaptures builtIns;
                    owner = std::make_unique<AmpSimProcessor>();
                }
                auto& p = *owner;
                expect (p.loadPreset (preset).ok);
                pumpPreset (p);
                if (scene >= 0)
                    expect (p.recallScene (scene));
                p.runHousekeeping();
                expectEquals (p.parameters.getRawParameterValue ("gate_a_threshold")->load(), -55.0f, name);
                expectEquals (p.parameters.getRawParameterValue ("gate_b_threshold")->load(), -55.0f, name);
                expect (p.parameters.getRawParameterValue ("gate_a_on")->load() >= 0.5f, name);

                juce::StringArray parts;
                for (const auto noiseDb : { -75.0, -65.0 })
                {
                    std::vector<float> input (3 * cycle, 0.0f);
                    const auto noise = pickupNoise ((int) input.size(), noiseDb, 11);
                    for (size_t c = 0; c < 3; ++c)
                        std::copy (riff.begin(), riff.end(), input.begin() + (long) (c * cycle));
                    for (size_t i = 0; i < input.size(); ++i)
                        input[i] += noise[i];
                    for (const auto threshold : { -55.0f, -45.0f })
                    {
                        setParam (p, "gate_a_threshold", threshold);
                        p.prepareToPlay (fs, blockSize);
                        const auto stats = gapStats (play (p, input), riff, phrase, cycle, 3);
                        if (threshold < -50.0f)
                            expectEquals (stats.falseCloses, 0, name + ": a note cut");
                        parts.add ("noise " + juce::String (noiseDb, 0) + " dBFS at " + juce::String (threshold, 0) + ": closed " + juce::String (100.0 * stats.closedShare, 2)
                                   + "% of the gaps, " + juce::String (stats.falseCloses) + " notes cut");
                    }
                    setParam (p, "gate_a_threshold", -55.0f);
                }
                lines.add (name + (scene >= 0 ? " (scene " + juce::String (scene + 1) + ")" : juce::String()) + ": stores -55; " + parts.joinIntoString ("; "));
            }
            expectEquals (checked, 3);
            logMessage ("  -> " + lines.joinIntoString ("\n  -> "));
        }

        beginTest ("the gate's first switch-on from the UI runs Learn once, with the prompt; never from a preset, a scene, MIDI, undo, or the host; the flag persists per install");
        {
            const auto settingsFile = tempDir().getChildFile ("gate_auto_learn_settings.json");
            settingsFile.deleteFile();
            platform::settings::setFileForTests (settingsFile); // a fresh install

            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::amp);
            ed.refresh();
            const auto quiet = pickupNoise ((int) (0.3 * fs), -70.0, 3);
            const auto nothingStarted = [&] (const juce::String& what) {
                // Past the lead-in, nothing may be learning or pending, and no prompt.
                juce::Thread::sleep ((int) AmpSimProcessor::autoLearnLeadInMs + 100);
                p.runHousekeeping();
                play (p, quiet);
                p.runHousekeeping();
                expectEquals (p.getAutoLearnCount(), 0, what);
                expect (! p.isLearningGates(), what);
                expect (p.getAutoLearnPrompt().isEmpty(), what);
                expect (! platform::settings::getBool (platform::settings::gateAutoLearnDone, false), what);
            };

            // A preset that switches the gate on (Metal), a scene that does, a footswitch, the host, and undo.
            expect (p.loadPreset (presets::factoryPresets()[3]).ok);
            pumpPreset (p);
            expect (p.parameters.getRawParameterValue ("gate_a_on")->load() >= 0.5f);
            nothingStarted ("a preset");
            setParam (p, "gate_a_on", 0.0f);
            p.storeScene (5);
            setParam (p, "gate_a_on", 1.0f);
            p.storeScene (6);
            p.recallScene (5);
            p.recallScene (6);
            expect (p.parameters.getRawParameterValue ("gate_a_on")->load() >= 0.5f);
            nothingStarted ("a scene");
            setParam (p, "gate_a_on", 0.0f);
            p.getMidiMap().set ({ 85, MidiMapping::Action::toggle, "gate_a_on" });
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;
                midi.addEvent (juce::MidiMessage::controllerEvent (1, 85, 127), 0);
                buffer.clear();
                p.processBlock (buffer, midi);
                p.runHousekeeping();
            }
            expect (p.parameters.getRawParameterValue ("gate_a_on")->load() >= 0.5f, "the footswitch must have switched it on");
            nothingStarted ("MIDI");
            setParam (p, "gate_a_on", 0.0f);
            p.parameters.copyState();
            p.undoManager.beginNewTransaction();
            setParam (p, "gate_a_on", 1.0f); // the host
            nothingStarted ("the host");
            p.parameters.copyState();
            p.undoManager.beginNewTransaction();
            setParam (p, "gate_a_on", 0.0f);
            p.parameters.copyState();
            p.undoManager.beginNewTransaction();
            p.undoManager.undo();
            expect (p.parameters.getRawParameterValue ("gate_a_on")->load() >= 0.5f, "undo must have switched it back on");
            nothingStarted ("undo");

            // Now from the UI: the Amp page strip's switch, off, then on with a click.
            setParam (p, "gate_a_on", 0.0f);
            ed.refresh();
            auto& gateSwitch = ed.getAmpView().getGateSwitch();
            click (gateSwitch);
            const auto clickMs = juce::Time::getMillisecondCounterHiRes();
            expect (p.parameters.getRawParameterValue ("gate_a_on")->load() >= 0.5f);
            ed.refresh();
            const auto promptAtClick = p.getAutoLearnPrompt();
            const auto statusAtClick = ed.getStatusText();
            expectEquals (promptAtClick, AmpSimProcessor::autoLearnPromptText);
            expectEquals (statusAtClick, AmpSimProcessor::autoLearnPromptText);
            expect (platform::settings::getBool (platform::settings::gateAutoLearnDone, false), "the flag is set at the click");
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), proofDir().getChildFile ("gate_auto_learn_prompt_amp.png")));
            const auto& amp = ed.getAmpView();
            expect (savePng (editor->createComponentSnapshot (amp.getBoundsInParent().withTrimmedBottom (amp.getHeight() - 70).translated (0, 0), true, 3.0f),
                             proofDir().getChildFile ("gate_auto_learn_prompt_crop.png")));
            expect (! p.isLearningGates(), "the lead-in: not measuring yet");
            const auto started = housekeepUntil (p, 3000.0, [&] { return p.isLearningGates(); });
            const auto leadIn = juce::Time::getMillisecondCounterHiRes() - clickMs;
            expect (started);
            expectEquals (p.getAutoLearnCount(), 1);

            // The measurement: 2.2 s of the muted strings' noise (-70 dBFS RMS), then the result line.
            play (p, pickupNoise ((int) (1.0 * fs), -70.0, 5));
            ed.refresh();
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), proofDir().getChildFile ("gate_auto_learn_measuring_amp.png")));
            ed.showPage (ui::PageId::preFx);
            ed.selectBlock (ui::BlockId::gateA);
            ed.refresh();
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), proofDir().getChildFile ("gate_auto_learn_gate_page.png")));
            play (p, pickupNoise ((int) (1.2 * fs), -70.0, 6));
            housekeepUntil (p, 1000.0, [&] { return ! p.isLearningGates(); });
            p.runHousekeeping();
            const auto learned = p.parameters.getRawParameterValue ("gate_a_threshold")->load();
            const auto resultLine = p.getAutoLearnPrompt();
            expect (! p.isLearningGates());
            expectGreaterThan (learned, -55.0f);
            expectLessThan (learned, -40.0f);
            expect (resultLine.startsWith ("Noise floor learned"), resultLine);
            ed.showPage (ui::PageId::amp);
            ed.refresh();
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), proofDir().getChildFile ("gate_auto_learn_result_amp.png")));

            // Exactly once: off and on again from the strip, and from the gate page's own switch: nothing more.
            click (gateSwitch);
            click (gateSwitch);
            expect (p.parameters.getRawParameterValue ("gate_a_on")->load() >= 0.5f);
            ed.showPage (ui::PageId::preFx);
            ed.selectBlock (ui::BlockId::gateA);
            setParam (p, "gate_a_on", 0.0f);
            ed.refresh();
            auto* pageSwitchPointer = findSwitch (ed.getPage (ui::BlockId::gateA), "gate_a_on");
            expect (pageSwitchPointer != nullptr, "the gate page's own switch");
            auto& pageSwitch = *pageSwitchPointer;
            click (pageSwitch);
            expect (p.parameters.getRawParameterValue ("gate_a_on")->load() >= 0.5f);
            juce::Thread::sleep ((int) AmpSimProcessor::autoLearnLeadInMs + 100);
            p.runHousekeeping();
            expectEquals (p.getAutoLearnCount(), 1, "Learn must run once only");
            expect (! p.isLearningGates());

            // The flag persists: it's in the settings file, read back fresh (as the next launch would).
            const auto fileText = settingsFile.loadFileAsString();
            platform::settings::setFileForTests (settingsFile);
            const auto persisted = platform::settings::getBool (platform::settings::gateAutoLearnDone, false);
            expect (persisted);
            expect (fileText.contains ("\"gateAutoLearnDone\": true") || fileText.contains ("\"gateAutoLearnDone\":true"), fileText);
            AmpSimProcessor next;
            next.switchedByUser ("gate_a_on", true);
            juce::Thread::sleep ((int) AmpSimProcessor::autoLearnLeadInMs + 100);
            next.runHousekeeping();
            expectEquals (next.getAutoLearnCount(), 0, "a later session must not learn again");

            // A new install (no settings file) learns again, from the gate page's switch this time.
            settingsFile.deleteFile();
            platform::settings::setFileForTests (settingsFile);
            setParam (p, "gate_a_on", 0.0f);
            click (pageSwitch);
            const auto pagePrompt = p.getAutoLearnPrompt();
            expectEquals (pagePrompt, AmpSimProcessor::autoLearnPromptText);
            expect (housekeepUntil (p, 3000.0, [&] { return p.isLearningGates(); }));
            expectEquals (p.getAutoLearnCount(), 2);
            play (p, pickupNoise ((int) (2.2 * fs), -70.0, 7));
            p.runHousekeeping();

            logMessage ("  -> a preset (Metal), a scene, a MIDI footswitch, the host, and undo each switched Gate A on: no Learn, no prompt, the flag still unset");
            logMessage ("  -> a click on the Amp page's Gate switch: the prompt \"" + promptAtClick + "\" at once (the status line: \"" + statusAtClick
                        + "\"), the flag set at the click, Learn started " + juce::String (leadIn, 0) + " ms later; after 2.2 s of -70 dBFS RMS noise the threshold is "
                        + juce::String (learned, 1) + " dBFS and the line reads \"" + resultLine + "\"");
            logMessage ("  -> off and on twice more (the strip, the gate page): Learn ran once; settings.json: " + fileText.trim() + "; a later session doesn't learn; "
                        "a fresh install learns again from the gate page's switch");
            logMessage ("  -> snapshots: build/proof/gate_auto_learn_prompt_amp.png, gate_auto_learn_prompt_crop.png, gate_auto_learn_measuring_amp.png, gate_auto_learn_gate_page.png, gate_auto_learn_result_amp.png");
            platform::settings::setFileForTests (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ampsim_tests_settings.json"));
        }
    }
};

static GateInAppTests gateInAppTests;
