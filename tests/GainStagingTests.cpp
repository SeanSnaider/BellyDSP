// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The gain staging audit (Sean's play test, 2026-10-04: "when combined they get too chaotic", "the clipping
// is really high with the gain"). The whole app on the built-in captures and a bundled cab, fed the test
// guitar DI at two interface levels (peaks -12 and -6 dBFS), measured as integrated loudness (BS.1770,
// stereo, LUFS) and sample peak (dBFS):
//   1. each block switched on alone at its defaults, against the same rig with it off;
//   2. the factory presets scene by scene: the output and the level driving the amp.
// The tables go to the log (the "->" lines in build/proof/summary.txt) and to build/proof/gain_staging.txt.

#include "BuiltInCaptures.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "BlockParameters.h"
#include "dsp/Loudness.h"

#include <cmath>

namespace
{
using namespace testing;
using Settings = std::vector<std::pair<juce::String, float>>;

constexpr double renderSeconds = 5.0, skipSeconds = 0.5;
const juce::StringArray slotNames { "Glass", "Ember", "Monolith" };

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    if (auto* param = p.parameters.getParameter (id))
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

float getParam (AmpSimProcessor& p, const juce::String& id)
{
    auto* param = p.parameters.getParameter (id);
    return param != nullptr ? param->convertFrom0to1 (param->getValue()) : 0.0f;
}

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

/// Everything a preset holds back to its default (the global settings stay as they are).
void toDefaults (AmpSimProcessor& p)
{
    for (auto* parameter : p.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter); ranged != nullptr && ! presets::isGlobal (ranged->paramID))
            ranged->setValueNotifyingHost (ranged->getDefaultValue());
}

/// The output safety limiter, where this build has one (the audit reports the rig before it).
void setLimiter (AmpSimProcessor& p, bool on)
{
    setParam (p, "output_limit_on", on ? 1.0f : 0.0f);
}

struct Level
{
    double lufs = -INFINITY, peakDb = -INFINITY;
};

/// The processor, freshly prepared (every fade snapped to its target, every block's state cleared), on the
/// input in 128-sample buffers. Measured after the first half second.
Level measure (AmpSimProcessor& p, const std::vector<float>& input, juce::AudioBuffer<float>* keep = nullptr)
{
    p.runHousekeeping();
    p.prepareToPlay (fs, blockSize);
    std::vector<float> left (input.size()), right (input.size());
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize, left.begin() + (std::ptrdiff_t) start);
        std::copy (buffer.getReadPointer (1), buffer.getReadPointer (1) + blockSize, right.begin() + (std::ptrdiff_t) start);
    }
    const auto skip = (size_t) (skipSeconds * fs);
    const auto n = (int) (input.size() - skip);
    Level level;
    level.lufs = ampsim::loudness::integrated ({ left.data() + skip, right.data() + skip }, n, fs);
    float peak = 0.0f;
    for (size_t i = skip; i < input.size(); ++i)
        peak = juce::jmax (peak, std::abs (left[i]), std::abs (right[i]));
    level.peakDb = toDb (peak);
    if (keep != nullptr)
    {
        keep->setSize (2, (int) input.size());
        keep->copyFrom (0, 0, left.data(), (int) left.size());
        keep->copyFrom (1, 0, right.data(), (int) right.size());
    }
    return level;
}

/// The level reaching the amp: the same rig with the amp and cab bypassed and the post section off, so the
/// output is the amp's input (copied to both sides).
Level ampInput (AmpSimProcessor& p, const std::vector<float>& input)
{
    const auto amp = getParam (p, "amp_bypass"), cab = getParam (p, "cab_bypass"), post = getParam (p, "post_fx_on"), out = getParam (p, "output_gain");
    setParam (p, "amp_bypass", 1.0f);
    setParam (p, "cab_bypass", 1.0f);
    setParam (p, "post_fx_on", 0.0f);
    setParam (p, "output_gain", 0.0f);
    const auto level = measure (p, input);
    setParam (p, "amp_bypass", amp);
    setParam (p, "cab_bypass", cab);
    setParam (p, "post_fx_on", post);
    setParam (p, "output_gain", out);
    return level;
}

juce::String num (double v, int width = 7, int decimals = 1)
{
    if (! std::isfinite (v))
        return juce::String ("-inf").paddedLeft (' ', width);
    return ((v > 0.0 ? "+" : "") + juce::String (v, decimals)).paddedLeft (' ', width);
}

juce::String col (const juce::String& s, int width) { return s.paddedRight (' ', width).substring (0, width); }

std::vector<float> scaled (const std::vector<float>& x, float gain)
{
    auto y = x;
    for (auto& v : y)
        v *= gain;
    return y;
}

juce::File cabFor (int slot)
{
    // The bundled cab each built-in plays through in the factory presets: the vintage 4x12 for Glass and
    // Ember (Modern Prog, Math Rock, Midwest Emo), the modern one for Monolith (Tech Death, Metal).
    const auto factory = presets::factoryPresets();
    return presets::resolve (presets::FileRef::fromVar (factory[slot == 2 ? 2 : 0]["cab"]["mic1"]), "irs").file;
}
} // namespace

class GainStagingTests final : public juce::UnitTest
{
public:
    GainStagingTests() : juce::UnitTest ("Gain staging", "ampsim") {}

    juce::StringArray report;

    void line (const juce::String& text)
    {
        report.add (text);
        logMessage ("  -> " + text);
    }

    void runTest() override
    {
        const auto di6 = guitarDI ((int) (renderSeconds * fs));
        const auto di12 = scaled (di6, 0.5f);
        const std::array<const std::vector<float>*, 2> inputs { &di12, &di6 };

        std::unique_ptr<AmpSimProcessor> owner;
        {
            WithBuiltInCaptures builtIns;
            owner = std::make_unique<AmpSimProcessor>();
        }
        auto& p = *owner;
        p.loadCabIR (0, cabFor (0));
        waitForLoads (p);

        {
            const auto in6 = ampsim::loudness::integratedMono (di6.data(), (int) di6.size(), fs);
            line ("The test DI (Karplus-Strong riff, the loader's reference): peaks -6.0 dBFS, " + juce::String (in6, 1) + " LUFS (mono); the -12 dBFS copy "
                  + juce::String (in6 - 6.02, 1) + " LUFS. Output loudness is stereo BS.1770 (a centred mono signal reads 3 dB above its mono loudness)");
        }

        beginTest ("audit: each block switched on alone at its defaults, against the same rig with it off (DI peaks -12 and -6 dBFS)");
        blockAudit (p, inputs);

        beginTest ("audit: the factory presets, scene by scene: the output, and the level driving the amp");
        presetAudit (p, inputs);

        proofDir().getChildFile ("gain_staging.txt").replaceWithText (report.joinIntoString ("\n") + "\n");
    }

    void blockAudit (AmpSimProcessor& p, const std::array<const std::vector<float>*, 2>& inputs)
    {
        // Pre-amp blocks change what the amp hears, so each is measured on all three captures, and at the amp's
        // input; post-amp blocks are measured on Ember.
        struct Config
        {
            juce::String name;
            Settings settings;
            bool pre;
        };
        const std::vector<Config> configs {
            { "Gate A", { { "gate_a_on", 1 } }, true },
            { "Pre comp", { { "comp_pre_on", 1 } }, true },
            { "Boost Clean", { { "boost_on", 1 }, { "boost_mode", 0 } }, true },
            { "Boost Tight", { { "boost_on", 1 }, { "boost_mode", 1 } }, true },
            { "Boost Screamer", { { "boost_on", 1 }, { "boost_mode", 2 } }, true },
            { "OD Mid Drive", { { "od_on", 1 }, { "od_mode", 0 } }, true },
            { "OD Distortion", { { "od_on", 1 }, { "od_mode", 1 } }, true },
            { "OD Transparent", { { "od_on", 1 }, { "od_mode", 2 } }, true },
            { "OD Fuzz", { { "od_on", 1 }, { "od_mode", 3 } }, true },
            { "Pre EQ (flat, on)", { { "eq_pre_on", 1 } }, true },
            { "Gate B", { { "gate_b_on", 1 } }, false },
            { "Post EQ (flat, on)", { { "eq_post_on", 1 } }, false },
            { "Post comp", { { "comp_post_on", 1 } }, false },
            { "Harmonizer", { { "harm_on", 1 } }, false },
            { "Multivoicer", { { "mv_on", 1 } }, false },
            { "Bloom bitcrush", { { "bloom_on", 1 }, { "bloom_crush_on", 1 } }, false },
            { "Bloom phaser", { { "bloom_on", 1 }, { "bloom_phaser_on", 1 } }, false },
            { "Bloom flanger", { { "bloom_on", 1 }, { "bloom_flanger_on", 1 } }, false },
            { "Chorus Classic", { { "chorus_on", 1 }, { "chorus_mode", 0 } }, false },
            { "Chorus Dimension", { { "chorus_on", 1 }, { "chorus_mode", 1 } }, false },
            { "Chorus Tri", { { "chorus_on", 1 }, { "chorus_mode", 2 } }, false },
            { "Delay Digital", { { "delay_on", 1 }, { "delay_mode", 0 } }, false },
            { "Delay Analog", { { "delay_on", 1 }, { "delay_mode", 1 } }, false },
            { "Delay Tape", { { "delay_on", 1 }, { "delay_mode", 2 } }, false },
            { "Reverb Room", { { "reverb_on", 1 }, { "reverb_engine", 0 } }, false },
            { "Reverb Hall", { { "reverb_on", 1 }, { "reverb_engine", 1 } }, false },
            { "Reverb Plate", { { "reverb_on", 1 }, { "reverb_engine", 2 } }, false },
            { "Reverb Hall + shimmer 50%", { { "reverb_on", 1 }, { "reverb_engine", 1 }, { "reverb_shimmer", 50 } }, false },
        };

        // The bare rig (effects at their defaults: everything off but the flat EQs), per amp and level.
        std::array<std::array<Level, 2>, 3> base;
        std::array<Level, 2> baseInput;
        for (int s = 0; s < 3; ++s)
            for (int l = 0; l < 2; ++l)
            {
                toDefaults (p);
                setLimiter (p, false);
                setParam (p, AmpSimProcessor::slotParamId, (float) s);
                base[(size_t) s][(size_t) l] = measure (p, *inputs[(size_t) l]);
                if (s == 0)
                    baseInput[(size_t) l] = ampInput (p, *inputs[(size_t) l]);
            }

        line ("");
        line ("BLOCK AUDIT. The bare rig (every effect at its default: off, the EQs on and flat; Master and Gain 5; the vintage 4x12 cab):");
        for (int s = 0; s < 3; ++s)
            line ("  " + col (slotNames[s], 9) + "DI -12: " + num (base[(size_t) s][0].lufs) + " LUFS, peak " + num (base[(size_t) s][0].peakDb)
                  + " dBFS;   DI -6: " + num (base[(size_t) s][1].lufs) + " LUFS, peak " + num (base[(size_t) s][1].peakDb) + " dBFS");
        line ("Each block on alone, change against the bare rig (dLUFS / dPeak in dB). Pre-amp blocks: the change at the amp's input, then on each capture.");
        line (col ("block", 27) + "| amp input -12 | amp input -6  | Glass -12     | Glass -6      | Ember -12     | Ember -6      | Monolith -12  | Monolith -6");

        for (const auto& c : configs)
        {
            auto row = col (c.name, 27);
            const auto apply = [&] (int slot)
            {
                toDefaults (p);
                setLimiter (p, false);
                setParam (p, AmpSimProcessor::slotParamId, (float) slot);
                for (const auto& [id, value] : c.settings)
                    setParam (p, id, value);
            };
            const auto cell = [] (const Level& on, const Level& off) { return "|" + num (on.lufs - off.lufs) + num (on.peakDb - off.peakDb) + " "; };

            if (c.pre)
            {
                for (int l = 0; l < 2; ++l)
                {
                    apply (0);
                    const auto in = ampInput (p, *inputs[(size_t) l]);
                    row << cell (in, baseInput[(size_t) l]);
                    // At its defaults a block changes the amp's drive by at most about 1 dB; a drive pedal at
                    // unity by at most 3.5 dB across -12 to -6 dBFS (clipping compresses); Tight its mid push.
                    const auto drive = c.name.startsWith ("OD") || c.name.contains ("Screamer");
                    const auto limit = drive ? 3.5 : c.name.contains ("Tight") ? 1.6 : 1.1;
                    expectLessThan (std::abs (in.lufs - baseInput[(size_t) l].lufs), limit, c.name);
                }
            }
            else
            {
                row << "|      -        |      -        ";
            }

            for (int s = 0; s < 3; ++s)
                for (int l = 0; l < 2; ++l)
                {
                    if (! c.pre && s != 1)
                    {
                        row << "|      -        ";
                        continue;
                    }
                    apply (s);
                    const auto on = measure (p, *inputs[(size_t) l]);
                    expect (std::isfinite (on.peakDb), c.name);
                    if (! c.pre) // post-amp blocks at their defaults: within about 1 dB of the bare rig's loudness
                        expectLessThan (std::abs (on.lufs - base[(size_t) s][(size_t) l].lufs), 1.1, c.name);
                    row << cell (on, base[(size_t) s][(size_t) l]);
                }
            line (row);
        }
    }

    void presetAudit (AmpSimProcessor& p, const std::array<const std::vector<float>*, 2>& inputs)
    {
        line ("");
        line ("FACTORY PRESETS, scene by scene (the output before the safety limiter; the amp's input; the DI at -12 and -6 dBFS peaks):");
        line (col ("preset / scene", 28) + col ("amp", 10) + "| out -12: LUFS  peak | out -6: LUFS   peak | amp in -12 peak | amp in -6 peak");
        for (const auto& preset : presets::factoryPresets())
        {
            expect (p.loadPreset (preset).ok);
            waitForLoads (p);
            for (int i = 0; i < 200 && p.isChangingPreset(); ++i)
            {
                juce::Thread::sleep (10);
                p.runHousekeeping();
            }
            waitForLoads (p);
            setLimiter (p, false);
            for (int scene = 0; scene < Scenes::count; ++scene)
            {
                if (! p.recallScene (scene))
                    continue;
                const auto slot = juce::roundToInt (getParam (p, AmpSimProcessor::slotParamId));
                auto row = col (preset["name"].toString() + " / " + p.getScenes().get (scene).name, 28) + col (slotNames[slot], 10);
                std::array<Level, 2> out, in;
                for (int l = 0; l < 2; ++l)
                    out[(size_t) l] = measure (p, *inputs[(size_t) l]);
                for (int l = 0; l < 2; ++l)
                    in[(size_t) l] = ampInput (p, *inputs[(size_t) l]);
                row << "|" << num (out[0].lufs, 12) << num (out[0].peakDb, 7) << " |" << num (out[1].lufs, 12) << num (out[1].peakDb, 7) << " |"
                    << num (in[0].peakDb, 16) << " |" << num (in[1].peakDb, 15);
                line (row);
            }
        }
    }
};

static GainStagingTests gainStagingTests;

namespace
{
/// A mono block on the DI in 128-sample buffers (the DI as its context too); the output's mono loudness
/// minus the input's, both after the first half second.
double loudnessChange (ampsim::Block& block, const std::vector<float>& input)
{
    block.prepare (fs, blockSize);
    auto out = input;
    for (size_t start = 0; start + blockSize <= out.size(); start += blockSize)
    {
        float* channels[] = { out.data() + start };
        juce::dsp::AudioBlock<float> view (channels, 1, (size_t) blockSize);
        const ampsim::BlockContext context { input.data() + start, blockSize };
        block.process (view, context);
    }
    const auto skip = (size_t) (skipSeconds * fs);
    const auto n = (int) (input.size() - skip);
    return ampsim::loudness::integratedMono (out.data() + skip, n, fs) - ampsim::loudness::integratedMono (input.data() + skip, n, fs);
}
} // namespace

class GainStagingCalibrationTests final : public juce::UnitTest
{
public:
    GainStagingCalibrationTests() : juce::UnitTest ("Gain staging calibration", "ampsim") {}

    void runTest() override
    {
        const auto di6 = guitarDI ((int) (renderSeconds * fs));
        const auto di9 = scaled (di6, juce::Decibels::decibelsToGain (-3.0f));
        const auto di12 = scaled (di6, 0.5f);

        beginTest ("pre compressor: auto makeup restored at -21 dBFS (where the DI sits) holds the DI's loudness within 1 dB at its defaults");
        {
            juce::StringArray rows;
            for (const auto reference : { -12.0, -15.0, -18.0, -20.0, -21.0, -22.0, -24.0 })
            {
                ampsim::Compressor::Settings s;
                s.autoMakeupReferenceDb = (float) reference;
                juce::String row = "reference " + juce::String (reference, 0) + " dBFS (makeup "
                                   + juce::String (ampsim::Compressor::autoMakeupDb (-24.0, 4.0, 6.0, reference), 1) + " dB):";
                for (const auto* in : { &di12, &di9, &di6 })
                {
                    ampsim::Compressor c (false);
                    c.setSettings (s);
                    const auto change = loudnessChange (c, *in);
                    row << " " << num (change);
                    if (juce::exactlyEqual (reference, ampsim::Compressor::preAmpMakeupReferenceDb))
                        expectLessThan (std::abs (change), 1.05);
                }
                rows.add (row);
            }
            logMessage ("  -> pre comp at its defaults, the DI's loudness change at peaks -12 / -9 / -6 dBFS, by auto makeup reference: "
                        + rows.joinIntoString ("; ") + ". The app uses " + juce::String (ampsim::Compressor::preAmpMakeupReferenceDb, 0)
                        + " dBFS for the pre instance (it was -12, the post instance's, before the audit)");
        }

        beginTest ("drive: every mode at its defaults with its unity trim leaves a -9 dBFS DI's loudness within 0.3 dB (Level 0 dB is as loud as bypassed)");
        {
            juce::StringArray rows;
            const auto row = [&] (const juce::String& name, const std::function<std::unique_ptr<ampsim::Block> (bool)>& make)
            {
                juce::String text = name + ":";
                for (const bool trim : { false, true })
                {
                    text << (trim ? "  trimmed" : " bare");
                    for (const auto* in : { &di12, &di9, &di6 })
                    {
                        const auto change = loudnessChange (*make (trim), *in);
                        text << " " << num (change).trim();
                        if (trim && in == &di9)
                            expectLessThan (std::abs (change), 0.3, name);
                        if (trim)
                            expectLessThan (std::abs (change), 3.5, name);
                    }
                }
                rows.add (text);
            };
            for (int m = 0; m < 4; ++m)
                row (params::OverdriveParameters::modeNames()[m], [m] (bool trim)
                {
                    auto od = std::make_unique<ampsim::Overdrive>();
                    ampsim::Overdrive::Settings s;
                    s.mode = (ampsim::Overdrive::Mode) m;
                    s.unityTrim = trim;
                    od->setSettings (s);
                    return std::unique_ptr<ampsim::Block> (std::move (od));
                });
            row ("Boost Screamer", [] (bool trim)
            {
                auto b = std::make_unique<ampsim::Boost>();
                ampsim::Boost::Settings s;
                s.mode = ampsim::Boost::Mode::screamer;
                s.unityTrim = trim;
                b->setSettings (s);
                return std::unique_ptr<ampsim::Block> (std::move (b));
            });
            logMessage ("  -> loudness change (dB) at DI peaks -12 / -9 / -6 dBFS, bare circuit and with the unity trim: " + rows.joinIntoString ("; "));
        }
    }
};

static GainStagingCalibrationTests gainStagingCalibrationTests;
