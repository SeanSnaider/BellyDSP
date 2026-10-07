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

        beginTest ("golden: the match curve's fit (Round 2, item 5) on three candidates, and the search with the curve instead of the match EQ");
        {
            const auto* cands = e["candidates"].getArray();
            const auto ta = Analysis::of (target);
            double worstPoint = 0.0;
            int points = 0;
            for (const auto& c : *cands)
            {
                const auto amp = ToneMatcher::renderAmp (gainSet ((int) c["slot"]), {}, takeDi, (double) c["gain"], noCancel);
                const auto ir = ToneMatcher::loadIR (platform::factoryContentFolder().getChildFile ("irs").getChildFile (c["cab"].toString()));
                std::array<double, 5> tone {};
                const auto tv = doubles (c["tone"]);
                for (size_t b = 0; b < 5; ++b)
                    tone[b] = tv[b];
                auto bins = Analysis::of (ToneMatcher::convolve (amp, ir)).ltasBins;
                const auto h = ToneMatcher::linearPowerOnBins (tone, nullptr);
                for (size_t k = 0; k < bins.size(); ++k)
                    bins[k] *= h[k];
                const auto curve = ToneMatcher::fitMatchCurve (ta, bins);
                const auto* py = c["curve"].getArray();
                expect (py != nullptr && py->size() == (int) curve.points.size());
                for (int i = 0; py != nullptr && i < py->size() && i < (int) curve.points.size(); ++i)
                {
                    worstPoint = std::max (worstPoint, std::abs (curve.points[(size_t) i].db - (double) (*py)[i][1]));
                    worstPoint = std::max (worstPoint, std::abs (curve.points[(size_t) i].hz - (double) (*py)[i][0]) * 1.0e-3);
                    ++points;
                }
            }
            expectLessThan (worstPoint, 1.0e-3);

            MatchSettings s;
            s.mode = Mode::anything;
            for (int i = 0; i < 3; ++i)
                s.models.push_back (gainSet (i));
            s.cabs = builtInCabs();
            s.takeIsLinedUp = true;
            s.searchPedals = true;
            s.fitCurve = true;
            const auto r = ToneMatcher::match (target, takeDi, s, noCancel);
            expect (r.ok && ! r.usesMatchEq(), r.error);
            const auto& m = e["match_curve"];
            expectEquals (r.slot, (int) m["slot"]);
            expectWithinAbsoluteError (r.gainDb, (double) m["gain"], 1.0e-9);
            expectEquals (r.cab.getFileName(), m["cab"].toString());
            expectWithinAbsoluteError (r.matchCurveAmountPercent, (double) m["amount"], 1.0e-9);
            const auto* mp = m["points"].getArray();
            double worstMatch = 0.0;
            for (int i = 0; mp != nullptr && i < mp->size() && i < (int) r.matchCurve.points.size(); ++i)
                worstMatch = std::max (worstMatch, std::abs (r.matchCurve.points[(size_t) i].db - (double) (*mp)[i][1]));
            expect (mp != nullptr && mp->size() == (int) r.matchCurve.points.size());
            expectLessThan (worstMatch, 1.0e-3);
            expect (r.postCompressor.on == m["post_comp"].isObject());
            const auto pyScores = doubles (m["post_comp_scores"]);
            expectLessThan (worst (r.postCompressorScores, pyScores), 0.05);
            for (const auto& b : r.eq)
                expect (juce::exactlyEqual (b.gainDb, 0.0f), "the match EQ is flat when the curve replaces it");
            expectWithinAbsoluteError (r.spectralErrorAfterEqDb, (double) m["spectral_after"], 1.0e-3);

            // The render plays the curve: the result through renderTone against the same without it.
            auto settings = ToneMatcher::settingsFor (r, s);
            expect (settings.matchCurveOn && ! settings.postEqOn);
            const auto with = ToneMatcher::renderTone (settings, takeDi, noCancel);
            settings.matchCurveOn = false;
            const auto without = ToneMatcher::renderTone (settings, takeDi, noCancel);
            double diff = 0.0, energy = 0.0;
            for (size_t i = 0; i < with.size(); ++i)
            {
                diff += ((double) with[i] - without[i]) * ((double) with[i] - without[i]);
                energy += (double) without[i] * without[i];
            }
            expectGreaterThan (diff / energy, 1.0e-4);
            juce::String scores;
            for (auto v : r.postCompressorScores)
                scores << juce::String (v, 3) << " ";
            logMessage ("  -> the fit on " + juce::String (cands->size()) + " candidates: " + juce::String (points) + " points, the worst "
                        + juce::String (worstPoint, 6) + " dB from the prototype's; the search with the curve: slot " + juce::String (r.slot + 1) + " at "
                        + juce::String (r.gainDb, 1) + " dB, " + r.cab.getFileNameWithoutExtension() + ", the curve's " + juce::String ((int) r.matchCurve.points.size())
                        + " points within " + juce::String (worstMatch, 6) + " dB, at " + juce::String (r.matchCurveAmountPercent, 0) + "%, post compressor scores "
                        + scores + "(prototype " + juce::String (pyScores.size() > 0 ? pyScores[0] : 0.0, 3) + " ...); the curve changes the render by "
                        + juce::String (10.0 * std::log10 (diff / energy), 1) + " dB relative");
        }

        beginTest ("golden: the whole search with the take-aware score finds the prototype's amp, Gain, and cab");
        {
            MatchSettings s;
            s.mode = Mode::anything;
            for (int i = 0; i < 3; ++i)
                s.models.push_back (gainSet (i));
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
            // With the pedals and the post compressor in the search (prototypes/tone_match.py, pedal_variants and
            // search_post_comp): the playing level, the variants, and the result.
            const auto play = ToneMatcher::playingLevelDb (takeDi);
            expectWithinAbsoluteError (play, (double) e["playing_level_db"], 1.0e-4);
            const auto variants = ToneMatcher::pedalVariants (takeDi, 18.0, s);
            const auto* pv = e["pedal_variants"].getArray();
            expect (pv != nullptr && pv->size() == (int) variants.size());
            int variantsMatching = 0;
            for (int i = 0; pv != nullptr && i < pv->size() && i < (int) variants.size(); ++i)
            {
                const auto& ev = (*pv)[i][0];
                const auto& [pedal, g] = variants[(size_t) i];
                const auto kind = ev["kind"].toString();
                bool same = std::abs (g - (double) (*pv)[i][1]) < 1.0e-9;
                if (kind == "boost")
                    same = same && pedal.kind == Pedal::Kind::boost && std::abs (pedal.boost.levelDb - (double) ev["level"]) < 1.0e-6;
                else if (kind == "comp")
                    same = same && pedal.kind == Pedal::Kind::compressor && std::abs (pedal.compressor.thresholdDb - (double) ev["threshold"]) < 1.0e-4
                           && std::abs (pedal.compressor.makeupDb - (double) ev["makeup"]) < 1.0e-4;
                else
                {
                    static const juce::StringArray modes { "mid", "distortion", "transparent", "fuzz" };
                    same = same && pedal.kind == Pedal::Kind::overdrive && (int) pedal.overdrive.mode == modes.indexOf (kind)
                           && std::abs (pedal.overdrive.drive - (double) ev["drive"]) < 1.0e-6;
                }
                variantsMatching += same ? 1 : 0;
            }
            expectEquals (variantsMatching, (int) variants.size());

            s.searchPedals = true;
            const auto fx = ToneMatcher::match (target, takeDi, s, noCancel);
            expect (fx.ok && fx.takeScored, fx.error);
            const auto& mf = e["match_fx"];
            expectEquals (fx.slot, (int) mf["slot"]);
            expectWithinAbsoluteError (fx.gainDb, (double) mf["gain"], 1.0e-9);
            expectEquals (fx.cab.getFileName(), mf["cab"].toString());
            const auto& ep = mf["pedal"];
            juce::String pedalLine = "none";
            if (ep.isObject())
            {
                const auto kind = ep["kind"].toString();
                if (kind == "comp")
                {
                    expect (fx.pedal.kind == Pedal::Kind::compressor);
                    expectWithinAbsoluteError ((double) fx.pedal.compressor.thresholdDb, (double) ep["threshold"], 1.0e-4);
                    pedalLine = "pre compressor at " + juce::String (fx.pedal.compressor.thresholdDb, 1) + " dB";
                }
                else if (kind == "boost")
                {
                    expect (fx.pedal.kind == Pedal::Kind::boost);
                    expectWithinAbsoluteError ((double) fx.pedal.boost.levelDb, (double) ep["level"], 1.0e-6);
                    pedalLine = "boost +" + juce::String (fx.pedal.boost.levelDb, 0) + " dB";
                }
                else
                {
                    expect (fx.pedal.kind == Pedal::Kind::overdrive);
                    expectWithinAbsoluteError ((double) fx.pedal.overdrive.drive, (double) ep["drive"], 1.0e-6);
                    pedalLine = "overdrive " + kind + " " + juce::String (fx.pedal.overdrive.drive, 1);
                }
            }
            else
                expect (fx.pedal.kind == Pedal::Kind::none);
            expect (fx.postCompressor.on == mf["post_comp"].isObject());
            const auto pyScores = doubles (mf["post_comp_scores"]);
            expectLessThan (worst (fx.postCompressorScores, pyScores), 0.02);
            expectWithinAbsoluteError (fx.takeScore.total, (double) mf["terms"]["total"], 0.05);
            juce::String scores;
            for (size_t i = 0; i < fx.postCompressorScores.size(); ++i)
                scores << juce::String (fx.postCompressorScores[i], 3) << " (" << juce::String (pyScores[i], 3) << ") ";
            logMessage ("  -> the playing level " + juce::String (play, 4) + " dB (prototype " + juce::String ((double) e["playing_level_db"], 4) + "), "
                        + juce::String (variantsMatching) + " of " + juce::String ((int) variants.size()) + " variants the prototype's; with pedals: slot "
                        + juce::String (fx.slot + 1) + " at " + juce::String (fx.gainDb, 1) + " dB, " + fx.cab.getFileNameWithoutExtension() + ", " + pedalLine
                        + ", post compressor " + (fx.postCompressor.on ? "on" : "off") + " (scores without, -4, -8 dB: " + scores + "), S "
                        + juce::String (fx.takeScore.total, 4) + " (" + juce::String ((double) mf["terms"]["total"], 4) + "), " + juce::String (fx.renders)
                        + " renders, " + juce::String (fx.runtimeSeconds, 1) + " s");
            s.searchPedals = false;

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
