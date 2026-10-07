// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The take-aware score (docs/TONE_MATCH.md, "Round 2: the objective"; src/tonematch/TakeScore.*) against the
// prototype (prototypes/tone_match.py, take_score) on the same files: tests/fixtures/take_score/expected.json, written
// by "tone_match.py take-golden" from the informed mask's fixture (the crunch rig's record and the player's take of
// its notes). The paired notes and the target's measures, three fixed candidates' terms, and the whole search with
// the take-aware score.

#include "TestHelpers.h"
#include "platform/AppInfo.h"
#include "tonematch/InformedMask.h"
#include "tonematch/TakeScore.h"
#include "tonematch/ToneMatcher.h"

namespace
{
using namespace testing;
using namespace ampsim::tonematch;

juce::File fixtures() { return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures"); }

std::vector<float> readMono (const juce::File& f)
{
    const auto b = readWav (f);
    return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
}

std::vector<double> doubles (const juce::var& v)
{
    std::vector<double> out;
    if (const auto* a = v.getArray())
        for (const auto& x : *a)
            out.push_back ((double) x);
    return out;
}

double worst (const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.size() != b.size())
        return 1.0e9;
    double w = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        w = std::max (w, std::abs (a[i] - b[i]));
    return w;
}

juce::File gainSet (int slot)
{
    static const char* names[] = { "Glass", "Ember", "Monolith" };
    return platform::factoryContentFolder().getChildFile ("models").getChildFile (names[slot]).getChildFile ("gainset.json");
}

std::array<ampsim::Equalizer::Band, ampsim::Equalizer::numParametricBands> eqOf (const juce::var& bands)
{
    std::array<ampsim::Equalizer::Band, ampsim::Equalizer::numParametricBands> eq {};
    int i = 0;
    if (const auto* a = bands.getArray())
        for (const auto& b : *a)
        {
            const auto kind = b[0].toString();
            const auto type = kind == "lowShelf" ? ampsim::Equalizer::BandType::lowShelf
                                                 : kind == "highShelf" ? ampsim::Equalizer::BandType::highShelf : ampsim::Equalizer::BandType::peak;
            eq[(size_t) i++] = { type, (float) (double) b[1], (float) (double) b[2], (float) (double) b[3] };
        }
    return eq;
}

std::vector<juce::File> builtInCabs()
{
    auto files = platform::factoryContentFolder().getChildFile ("irs").findChildFiles (juce::File::findFiles, true, "*.wav");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFullPathName() < b.getFullPathName(); });
    return std::vector<juce::File> (files.begin(), files.end());
}
} // namespace

class ToneMatchTakeScoreTests final : public juce::UnitTest
{
public:
    ToneMatchTakeScoreTests() : juce::UnitTest ("Tone match / take-aware score", "ampsim") {}

    void runTest() override
    {
        const auto e = juce::JSON::parse (fixtures().getChildFile ("take_score/expected.json").loadFileAsString());
        const auto target = readMono (fixtures().getChildFile ("informed_mask/record.wav"));
        const auto takeDi = readMono (fixtures().getChildFile ("informed_mask/di.wav"));
        std::atomic<bool> noCancel { false };
        const auto bandFrames = (int) std::ceil (playAlongBandSeconds * sampleRate / hop);
        const auto tn = take::TakeNotes::of (target, informed::alignNotes (takeDi, target, bandFrames, noCancel));

        beginTest ("golden: the paired notes, the target's per-note attack and level, its ERB spectrum and loudness weights");
        {
            const auto o = doubles (e["notes"]["o"]), t = doubles (e["notes"]["t"]), L = doubles (e["notes"]["L"]);
            std::vector<double> co (tn.takeOnsets.begin(), tn.takeOnsets.end()), ct (tn.targetOnsets.begin(), tn.targetOnsets.end()),
                cl (tn.lengths.begin(), tn.lengths.end());
            expect (worst (co, o) == 0.0 && worst (ct, t) == 0.0 && worst (cl, L) == 0.0, "the same notes, sample for sample");
            expectEquals ((int) tn.takeOnsets.size(), (int) o.size());
            expect (tn.usable());
            const auto dA = worst (tn.target.attack, doubles (e["attack"])), dL = worst (tn.target.level, doubles (e["level"]));
            const auto dC = worst (take::erbBands().centre, doubles (e["erb_centres"]));
            const auto dE = worst (tn.erbDb, doubles (e["erb_db"])), dW = worst (tn.erbWeights, doubles (e["erb_weights"]));
            expectLessThan (dA, 1.0e-4);
            expectLessThan (dL, 1.0e-4);
            expectLessThan (dC, 1.0e-6);
            expectLessThan (dE, 1.0e-3);
            expectLessThan (dW, 1.0e-6);
            logMessage ("  -> " + juce::String ((int) o.size()) + " note pairs, identical; worst differences: attack " + juce::String (dA, 7) + " dB, level "
                        + juce::String (dL, 7) + " dB, ERB centres " + juce::String (dC, 9) + " Hz (" + juce::String (take::erbBands().size())
                        + " bands), ERB spectrum " + juce::String (dE, 6) + " dB, loudness weights " + juce::String (dW, 9));
        }

        beginTest ("golden: three candidates' terms (flux, crest, attack, note-level spread, ERB distance) and total");
        {
            const auto* cands = e["candidates"].getArray();
            expect (cands != nullptr && cands->size() == 3);
            double worstTerm = 0.0, worstTotal = 0.0;
            for (const auto& c : *cands)
            {
                const auto slot = (int) c["slot"];
                const auto amp = ToneMatcher::renderAmp (gainSet (slot), {}, takeDi, (double) c["gain"], noCancel);
                const auto ir = ToneMatcher::loadIR (platform::factoryContentFolder().getChildFile ("irs").getChildFile (c["cab"].toString()));
                std::array<double, 5> tone {};
                const auto tv = doubles (c["tone"]);
                for (size_t b = 0; b < 5; ++b)
                    tone[b] = tv[b];
                const auto eq = eqOf (c["eq"]);
                const auto s = take::score (tn, Analysis::of (target), amp, ToneMatcher::convolve (amp, ir), tone, &eq, (double) c["spectral"]);
                const auto& terms = c["terms"];
                const std::pair<const char*, double> pairs[] { { "flux", s.flux }, { "crest", s.crest }, { "attack", s.attack },
                                                               { "spread", s.spread }, { "erb", s.erb } };
                juce::String line;
                for (const auto& [name, v] : pairs)
                {
                    const auto d = std::abs (v - (double) terms[name]);
                    worstTerm = std::max (worstTerm, d);
                    line << name << " " << juce::String (v, 4) << " (" << juce::String ((double) terms[name], 4) << ") ";
                }
                worstTotal = std::max (worstTotal, std::abs (s.total - (double) c["total"]));
                logMessage ("  -> slot " + juce::String (slot + 1) + " at " + juce::String ((double) c["gain"], 1) + " dB: " + line + "total "
                            + juce::String (s.total, 4) + " (" + juce::String ((double) c["total"], 4) + ")");
            }
            expectLessThan (worstTerm, 2.0e-3);
            expectLessThan (worstTotal, 2.0e-2);
            logMessage ("  -> worst term " + juce::String (worstTerm, 6) + ", worst total " + juce::String (worstTotal, 5));
        }

        beginTest ("golden: the whole search with the take-aware score finds the prototype's amp, Gain, and cab");
        {
            MatchSettings s;
            s.mode = Mode::anything;
            for (int i = 0; i < 3; ++i)
                s.models[(size_t) i] = gainSet (i);
            s.cabs = builtInCabs();
            s.takeIsLinedUp = true;
            const auto r = ToneMatcher::match (target, takeDi, s, noCancel);
            expect (r.ok, r.error);
            expect (r.takeScored);
            const auto& m = e["match"];
            expectEquals (r.slot, (int) m["slot"]);
            expectWithinAbsoluteError (r.gainDb, (double) m["gain"], 1.0e-9);
            expectEquals (r.cab.getFileName(), m["cab"].toString());
            const auto dS = std::abs (r.takeScore.total - (double) m["terms"]["total"]);
            expectLessThan (dS, 0.05);
            logMessage ("  -> slot " + juce::String (r.slot + 1) + " at " + juce::String (r.gainDb, 1) + " dB, " + r.cab.getFileNameWithoutExtension() + ", "
                        + juce::String (r.notePairs) + " note pairs, S " + juce::String (r.takeScore.total, 4) + " (prototype "
                        + juce::String ((double) m["terms"]["total"], 4) + "), in " + juce::String (r.runtimeSeconds, 1) + " s");
            // Without the flag the old score decides, as before.
            s.takeIsLinedUp = false;
            const auto old = ToneMatcher::match (target, takeDi, s, noCancel);
            expect (old.ok && ! old.takeScored);
            logMessage ("  -> the old score alone: slot " + juce::String (old.slot + 1) + " at " + juce::String (old.gainDb, 1) + " dB, "
                        + old.cab.getFileNameWithoutExtension());
        }
    }
};

static ToneMatchTakeScoreTests toneMatchTakeScoreTests;
