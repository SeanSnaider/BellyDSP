// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// Cleaning up the target with the take (docs/TONE_MATCH.md, "Cleaning up the target with your take"): the
// C++ informed mask (src/tonematch/InformedMask.*) against the Python prototype (prototypes/learn_tone.py,
// align_notes and informed_mask) on the same files (tests/fixtures/informed_mask, written by its
// "informed-golden" command), its STFT round trip, cancel, and the end-to-end effect on the matcher.

#include "TestHelpers.h"
#include "platform/AppInfo.h"
#include "tonematch/InformedMask.h"
#include "tonematch/ToneMatcher.h"

#include <thread>

namespace
{
using namespace testing;
using namespace ampsim::tonematch;
namespace im = ampsim::tonematch::informed;

juce::File fixtures() { return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/informed_mask"); }

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

std::vector<int> ints (const juce::var& v)
{
    std::vector<int> out;
    for (auto d : doubles (v))
        out.push_back ((int) std::llround (d));
    return out;
}

double cents (double a, double b) { return a > 0.0 && b > 0.0 ? 1200.0 * std::log2 (a / b) : (juce::exactlyEqual (a, b) ? 0.0 : 1.0e9); }

/// 10 log10(|a|^2 / |a - b|^2), dB.
double snrDb (const std::vector<float>& reference, const std::vector<float>& estimate)
{
    double s = 0.0, e = 0.0;
    for (size_t i = 0; i < std::min (reference.size(), estimate.size()); ++i)
    {
        s += (double) reference[i] * reference[i];
        e += ((double) reference[i] - estimate[i]) * ((double) reference[i] - estimate[i]);
    }
    return 10.0 * std::log10 (s / std::max (e, 1.0e-30));
}

/// Scale-invariant SDR (Le Roux et al. 2019), as the prototype's si_sdr.
double siSdr (const std::vector<float>& reference, const std::vector<float>& estimate)
{
    double se = 0.0, ss = 0.0;
    for (size_t i = 0; i < reference.size(); ++i)
    {
        se += (double) estimate[i] * reference[i];
        ss += (double) reference[i] * reference[i];
    }
    const auto alpha = se / ss;
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < reference.size(); ++i)
    {
        const auto s = alpha * reference[i];
        num += s * s;
        den += (s - estimate[i]) * (s - estimate[i]);
    }
    return 10.0 * std::log10 (num / den);
}

/// The prototype's spectral_distance (tone match's long-term spectral error, anything mode): the weighted
/// RMS of the smoothed difference of the long-term spectra, mean removed, weights from the reference.
double spectralDistance (const std::vector<float>& reference, const std::vector<float>& y)
{
    const auto a = Analysis::of (reference), b = Analysis::of (y);
    std::vector<double> d (a.ltas.size());
    for (size_t i = 0; i < d.size(); ++i)
        d[i] = a.ltas[i] - b.ltas[i];
    return weightedRmsCentred (smoothBands (d), a.weights);
}

std::vector<juce::File> builtInCabs()
{
    auto files = platform::factoryContentFolder().getChildFile ("irs").findChildFiles (juce::File::findFiles, true, "*.wav");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFullPathName() < b.getFullPathName(); });
    return std::vector<juce::File> (files.begin(), files.end());
}

/// The matcher as the app runs it: the built-in gain sets in the three slots, the 21 built-in cabs.
MatchSettings appSettings (Mode mode, double bandSeconds)
{
    MatchSettings s;
    s.mode = mode;
    static const char* names[] = { "Glass", "Ember", "Monolith" };
    for (int i = 0; i < 3; ++i)
        s.models[(size_t) i] = platform::factoryContentFolder().getChildFile ("models").getChildFile (names[i]).getChildFile ("gainset.json");
    s.cabs = builtInCabs();
    s.alignmentBandSeconds = mode == Mode::samePart ? bandSeconds : 0.0;
    return s;
}

/// The take through what Apply would set for this result (renderTone: the slot's gain set, Gain, tone, the
/// cab as it plays, the match EQ as the post EQ).
std::vector<float> renderResult (const MatchResult& r, const MatchSettings& s, const std::vector<float>& di)
{
    ToneSettings t;
    t.model = s.models[(size_t) r.slot];
    t.gainDb = (float) r.gainDb;
    for (size_t b = 0; b < 5; ++b)
        t.tone[b] = (float) r.tone[b];
    t.cabIR = ToneMatcher::irAsPlayed (r.cab);
    t.postEqOn = true;
    t.postEq.mode = ampsim::Equalizer::Mode::parametric;
    for (size_t b = 0; b < r.eq.size(); ++b)
        t.postEq.bands[b] = r.eq[b];
    t.postEq.lowCut.on = false;
    t.postEq.highCut.on = false;
    std::atomic<bool> noCancel { false };
    return ToneMatcher::renderTone (t, di, noCancel);
}

juce::String describe (const MatchResult& r, const MatchSettings& s)
{
    return s.models[(size_t) r.slot].getParentDirectory().getFileName() + " " + juce::String (r.gainDb, 1) + ", "
           + r.cab.getFileNameWithoutExtension();
}
} // namespace

class InformedMaskTests final : public juce::UnitTest
{
public:
    InformedMaskTests() : juce::UnitTest ("Tone match / cleanup", "ampsim") {}

    void runTest() override
    {
        const auto& e = expected();
        const auto di = readMono (fixtures().getChildFile ("di.wav"));
        const auto target = readMono (fixtures().getChildFile ("target.wav"));
        std::atomic<bool> noCancel { false };

        beginTest ("golden: the take's onsets (flux peaks, refined to the sample), the flux lag, and each note's pitch match the prototype");
        {
            const auto peaks = im::fluxPeaks (di);
            const auto onsets = im::detectOnsets (di, peaks);
            const auto ePeaks = ints (e["flux_peaks"]), eOnsets = ints (e["onsets"]);
            expect (peaks == ePeaks, "flux peaks: " + juce::String ((int) peaks.size()) + " against " + juce::String ((int) ePeaks.size()));
            int exact = 0, worst = 0;
            expectEquals ((int) onsets.size(), (int) eOnsets.size());
            for (size_t k = 0; k < std::min (onsets.size(), eOnsets.size()); ++k)
            {
                exact += onsets[k] == eOnsets[k] ? 1 : 0;
                worst = std::max (worst, std::abs (onsets[k] - eOnsets[k]));
            }
            expect (worst <= 1, "onset off by " + juce::String (worst) + " samples");
            const auto lag = im::fluxLag (peaks, onsets);
            expectEquals (lag, (int) e["flux_lag"]);

            std::vector<int> ends (onsets.begin() + 1, onsets.end());
            ends.push_back (std::min ((int) di.size(), onsets.back() + 48000));
            const auto f0 = im::noteF0 (di, onsets, ends);
            const auto ef0 = doubles (e["f0"]);
            double worstCents = 0.0;
            int pitched = 0;
            for (size_t k = 0; k < std::min (f0.size(), ef0.size()); ++k)
            {
                worstCents = std::max (worstCents, std::abs (cents (f0[k], ef0[k])));
                pitched += f0[k] > 0.0 ? 1 : 0;
            }
            expect (worstCents < 0.5, "f0 off by " + juce::String (worstCents, 4) + " cents");

            // Against the truth (the onsets the take was synthesized with): within 10 ms, precision and recall.
            const auto truth = ints (e["truth_onsets"]);
            int hits = 0;
            for (auto o : onsets)
                for (auto t : truth)
                    if (std::abs (o - t) <= 480)
                    {
                        ++hits;
                        break;
                    }
            logMessage ("  -> " + juce::String ((int) peaks.size()) + " flux peaks, identical; " + juce::String ((int) onsets.size()) + " onsets, "
                        + juce::String (exact) + " sample-exact, the worst " + juce::String (worst) + " sample(s) off; flux lag " + juce::String (lag)
                        + " samples; " + juce::String (pitched) + " notes pitched, the worst " + juce::String (worstCents, 4)
                        + " cents from the prototype; " + juce::String (hits) + " of the onsets within 10 ms of the " + juce::String ((int) truth.size())
                        + " true ones");
        }

        beginTest ("golden: the notes in the target (DTW in the 0.5 s band, the harmonic onsets within +-60 ms), the refined pitches, the mask, and the output match the prototype");
        {
            im::AlignmentDetail detail;
            auto notes = im::alignNotes (di, target, (int) std::ceil (playAlongBandSeconds * sampleRate / hop), noCancel, &detail);
            expectEquals ((int) detail.path.path.size(), (int) e["path_length"]);
            long long checksum = 0;
            for (const auto& [i, j] : detail.path.path)
                checksum += (long long) i * 7 + (long long) j * 13;
            expectEquals ((int) checksum, (int) e["path_checksum"]);
            const auto est = doubles (e["est"]);
            double worstEst = 0.0;
            for (size_t k = 0; k < std::min (est.size(), detail.estimates.size()); ++k)
                worstEst = std::max (worstEst, std::abs (est[k] - detail.estimates[k]));
            expect (worstEst < 1.0e-6, juce::String (worstEst));
            const auto tOn = ints (e["t_on"]);
            int worstT = 0, exactT = 0;
            expectEquals ((int) detail.targetOnsets.size(), (int) tOn.size());
            for (size_t k = 0; k < std::min (tOn.size(), detail.targetOnsets.size()); ++k)
            {
                worstT = std::max (worstT, std::abs (tOn[k] - detail.targetOnsets[k]));
                exactT += tOn[k] == detail.targetOnsets[k] ? 1 : 0;
            }
            expect (worstT <= 1, "target onset off by " + juce::String (worstT));

            const auto* eNotes = e["notes"].getArray();
            expect (eNotes != nullptr && eNotes->size() == (int) notes.size(), "notes: " + juce::String ((int) notes.size()));
            int worstNote = 0;
            for (int k = 0; eNotes != nullptr && k < std::min (eNotes->size(), (int) notes.size()); ++k)
            {
                const auto& en = (*eNotes)[k];
                const auto& n = notes[(size_t) k];
                worstNote = std::max ({ worstNote, std::abs (n.o - (int) en["o"]), std::abs (n.t - (int) en["t"]), std::abs (n.length - (int) en["L"]),
                                        std::abs (n.tNext - (int) en["t_next"]) });
            }
            expect (worstNote <= 1, "a note off by " + juce::String (worstNote));

            im::MaskSettings rawSettings;
            rawSettings.timeSmooth = 0;
            auto rawNotes = notes;
            const auto rawMask = im::harmonicMask (target, rawNotes, rawSettings, noCancel);
            const auto mask = im::harmonicMask (target, notes, {}, noCancel);
            expectEquals (mask.frames, (int) e["mask_frames"]);
            const auto refined = doubles (e["refined_f0"]);
            double worstRefined = 0.0;
            for (size_t k = 0; k < std::min (refined.size(), notes.size()); ++k)
                worstRefined = std::max (worstRefined, std::abs (cents (notes[k].f0Target, refined[k])));
            expect (worstRefined < 0.5, "refined f0 off by " + juce::String (worstRefined, 4) + " cents");

            // The mask: every frame's sum over the 2049 bins (raw and smoothed), and whole frames bin by bin.
            auto frameSumError = [&] (const im::Mask& m, const juce::var& sums) {
                const auto s = doubles (sums);
                double worstRel = s.size() == (size_t) m.frames ? 0.0 : 1.0;
                for (int k = 0; k < std::min (m.frames, (int) s.size()); ++k)
                {
                    double sum = 0.0;
                    for (int b = 0; b < im::maskBins; ++b)
                        sum += m.at (k, b);
                    worstRel = std::max (worstRel, std::abs (sum - s[(size_t) k]) / s[(size_t) k]);
                }
                return worstRel;
            };
            const auto rawSumError = frameSumError (rawMask, e["raw_mask_frame_sums"]);
            const auto sumError = frameSumError (mask, e["mask_frame_sums"]);
            expect (rawSumError < 1.0e-5, juce::String (rawSumError));
            expect (sumError < 1.0e-5, juce::String (sumError));
            double worstBin = 0.0;
            int columns = 0;
            if (const auto* obj = e["mask_columns"].getDynamicObject())
                for (const auto& p : obj->getProperties())
                {
                    const auto frame = p.name.toString().getIntValue();
                    const auto values = doubles (p.value);
                    for (int b = 0; b < im::maskBins && b < (int) values.size(); ++b)
                        worstBin = std::max (worstBin, std::abs ((double) mask.at (frame, b) - values[(size_t) b]));
                    ++columns;
                }
            expect (columns > 5 && worstBin < 1.0e-5, juce::String (worstBin));

            // The output against the prototype's (masked.wav, 16-bit at a known scale).
            const auto output = im::applyMask (target, mask, noCancel);
            auto pyMasked = readMono (fixtures().getChildFile ("masked.wav"));
            const auto scale = (double) e["scales"]["masked"];
            for (auto& v : pyMasked)
                v = (float) (v / scale);
            const auto snr = snrDb (pyMasked, output);
            expect (snr > 60.0, juce::String (snr));

            const auto record = readMono (fixtures().getChildFile ("record.wav"));
            const auto sdrMix = siSdr (record, target), sdrMasked = siSdr (record, output);
            const auto spMix = spectralDistance (record, target), spMasked = spectralDistance (record, output);
            expect (sdrMasked > sdrMix + 3.0 && spMasked < spMix - 2.0);
            logMessage ("  -> DTW path " + juce::String ((int) detail.path.path.size()) + " steps, same checksum; target onsets " + juce::String (exactT) + "/"
                        + juce::String ((int) tOn.size()) + " sample-exact (worst " + juce::String (worstT) + "); " + juce::String ((int) notes.size())
                        + " notes, the worst field " + juce::String (worstNote) + " samples off; refined pitches within " + juce::String (worstRefined, 4)
                        + " cents; mask frame sums within " + juce::String (rawSumError, 8) + " (raw) and " + juce::String (sumError, 8)
                        + " (smoothed, relative), " + juce::String (columns) + " whole frames within " + juce::String (worstBin, 8)
                        + "; output against the prototype's: " + juce::String (snr, 1) + " dB SNR (masked.wav is 16-bit)");
            logMessage ("  -> against the unmixed record: SI-SDR " + juce::String (sdrMix, 2) + " dB (the mix) -> " + juce::String (sdrMasked, 2)
                        + " dB (cleaned; prototype " + juce::String ((double) e["si_sdr"]["masked"], 2) + "), spectral distance " + juce::String (spMix, 2)
                        + " -> " + juce::String (spMasked, 2) + " dB (prototype " + juce::String ((double) e["spectral"]["masked"], 2) + ")");
        }

        beginTest ("the STFT round trip: an all-ones mask gives the input back (the squared Hann windows overlap-add to a constant), the floor alone gives a tenth of it");
        {
            im::Mask ones;
            ones.frames = im::Mask::framesFor ((int) target.size());
            ones.values.assign ((size_t) ones.frames * im::maskBins, 1.0f);
            const auto back = im::applyMask (target, ones, noCancel);
            const auto snr = snrDb (target, back);
            expect (snr > 120.0, juce::String (snr));
            auto floorOnly = ones;
            std::fill (floorOnly.values.begin(), floorOnly.values.end(), 0.1f);
            auto tenth = target;
            for (auto& v : tenth)
                v *= 0.1f;
            const auto snrFloor = snrDb (tenth, im::applyMask (target, floorOnly, noCancel));
            expect (snrFloor > 120.0, juce::String (snrFloor));
            logMessage ("  -> all ones: " + juce::String (snr, 1) + " dB SNR against the input; the floor: " + juce::String (snrFloor, 1) + " dB against 0.1 x the input");
        }

        beginTest ("cleanUp: the whole step on the fixture, timed, and cancel stops it within 200 ms");
        {
            juce::StringArray stages;
            const auto r = im::cleanUp (target, di, playAlongBandSeconds, noCancel, [&] (double, const juce::String& s) { stages.addIfNotAlreadyThere (s); });
            expect (r.ok, r.error);
            expectEquals (r.notes, (int) e["notes"].getArray()->size());
            auto pyMasked = readMono (fixtures().getChildFile ("masked.wav"));
            for (auto& v : pyMasked)
                v = (float) (v / (double) e["scales"]["masked"]);
            expect (snrDb (pyMasked, r.output) > 60.0);

            // A minute of audio (the fixture repeated), cancelled 100 ms in.
            std::vector<float> longTarget, longDi;
            for (int i = 0; i < 10; ++i)
            {
                longTarget.insert (longTarget.end(), target.begin(), target.end());
                longDi.insert (longDi.end(), di.begin(), di.end());
            }
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto full = im::cleanUp (longTarget, longDi, playAlongBandSeconds, noCancel);
            const auto fullSeconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            expect (full.ok);
            std::atomic<bool> cancel { false };
            double cancelledAt = 0.0;
            std::thread canceller ([&] {
                juce::Thread::sleep (100);
                cancelledAt = juce::Time::getMillisecondCounterHiRes();
                cancel = true;
            });
            const auto c = im::cleanUp (longTarget, longDi, playAlongBandSeconds, cancel);
            const auto returnedAt = juce::Time::getMillisecondCounterHiRes();
            canceller.join();
            expect (! c.ok && c.cancelled);
            const auto lag = returnedAt - cancelledAt;
            expect (lag < 200.0 * cpuBudgetScale(), juce::String (lag));

            // No notes: a silent take.
            const auto none = im::cleanUp (target, std::vector<float> (di.size(), 0.0f), playAlongBandSeconds, noCancel);
            expect (! none.ok && ! none.cancelled && none.error.isNotEmpty());
            logMessage ("  -> 6 s: " + juce::String (r.seconds, 2) + " s, " + juce::String (r.onsets) + " onsets, " + juce::String (r.notes) + " notes ("
                        + juce::String (r.pitched) + " pitched), kept " + juce::String (r.keptDb, 1) + " dB of the target's energy; stages: "
                        + stages.joinIntoString (" / ") + ". 60 s: " + juce::String (fullSeconds, 2) + " s (" + juce::String (full.notes)
                        + " notes); cancelled 100 ms in, it returned " + juce::String (lag, 1) + " ms after the flag (\"" + c.error
                        + "\"); a silent take: \"" + none.error + "\"");
        }

        beginTest ("golden: the bleed detector (kept energy of the target and of the bleed-free proxy, and their difference) matches the prototype, and decides as it does");
        {
            // learn_tone.py bleed-golden: the proxy is oracle.wav (the hidden rig on the take); the targets are the
            // record in the band, the unmixed record (nothing to remove), and the separated high-gain stem.
            struct Case
            {
                const char* key;
                juce::File folder;
                const char* target;
            };
            const Case cases[] { { "bleed", fixtures(), "target.wav" },
                                 { "bleed_record", fixtures(), "record.wav" },
                                 { "bleed_separated_high_gain", fixtures().getChildFile ("separated_high_gain"), "target.wav" } };
            for (const auto& c : cases)
            {
                const auto& g = e[c.key];
                const auto cdi = readMono (c.folder.getChildFile ("di.wav"));
                const auto ctarget = readMono (c.folder.getChildFile (c.target));
                const auto proxy = readMono (c.folder.getChildFile ("oracle.wav"));
                const auto b = im::measureBleed (ctarget, cdi, proxy, playAlongBandSeconds, noCancel);
                expect (b.ok, b.error);
                const auto dt = std::abs (b.keptTargetDb - (double) g["kept_target"]), dp = std::abs (b.keptProxyDb - (double) g["kept_proxy"]);
                const auto dx = std::abs (b.excessDb - (double) g["excess"]);
                expect (dt < 0.005 && dp < 0.005 && dx < 0.01, juce::String (c.key) + ": " + juce::String (dt, 6) + " " + juce::String (dp, 6));
                expectEquals ((double) g["threshold"], im::bleedExcessThresholdDb);
                expect ((b.excessDb > im::bleedExcessThresholdDb) == ((double) g["excess"] > (double) g["threshold"]), "the same decision");
                logMessage (juce::String ("  -> ") + c.key + ": kept target " + juce::String (b.keptTargetDb, 4) + " dB (prototype "
                            + juce::String ((double) g["kept_target"], 4) + "), proxy " + juce::String (b.keptProxyDb, 4) + " ("
                            + juce::String ((double) g["kept_proxy"], 4) + "), excess " + juce::String (b.excessDb, 4) + " dB ("
                            + juce::String ((double) g["excess"], 4) + "): " + (b.excessDb > im::bleedExcessThresholdDb ? "clean up" : "leave it"));
            }
            const auto silent = im::measureBleed (target, std::vector<float> (di.size(), 0.0f), target, playAlongBandSeconds, noCancel);
            expect (! silent.ok && ! silent.cancelled && silent.error.isNotEmpty());
        }

        beginTest ("end to end: the matcher (Same part, the 0.5 s band, the built-in gain sets) on the target with and without the cleanup, against the hidden rig on the take");
        {
            // Two targets: the record in the band with no separation (the fixture), and the study's high-gain case
            // separated by Demucs. The truth is the hidden rig on the take (oracle.wav): the matched settings
            // render the take, and their long-term spectrum is compared with the rig's (the prototype's spectral
            // distance). Lower is closer.
            juce::StringArray rows;
            int improved = 0;
            for (const auto* name : { "mix", "separated_high_gain" })
            {
                const auto folder = juce::String (name) == "mix" ? fixtures() : fixtures().getChildFile (name);
                const auto take = readMono (folder.getChildFile ("di.wav"));
                const auto raw = readMono (folder.getChildFile ("target.wav"));
                const auto oracle = readMono (folder.getChildFile ("oracle.wav"));
                const auto cleaned = im::cleanUp (raw, take, playAlongBandSeconds, noCancel);
                expect (cleaned.ok, cleaned.error);

                const auto same = appSettings (Mode::samePart, playAlongBandSeconds);
                const auto off = ToneMatcher::match (raw, take, same, noCancel);
                const auto on = ToneMatcher::match (cleaned.output, take, same, noCancel);
                const auto anything = appSettings (Mode::anything, 0.0);
                const auto onAnything = ToneMatcher::match (cleaned.output, take, anything, noCancel);
                expect (off.ok && on.ok && onAnything.ok);
                if (! (off.ok && on.ok && onAnything.ok))
                    continue;
                const auto dOff = spectralDistance (oracle, renderResult (off, same, take));
                const auto dOn = spectralDistance (oracle, renderResult (on, same, take));
                const auto dAnything = spectralDistance (oracle, renderResult (onAnything, anything, take));
                expect (dOn < dOff, juce::String (name) + ": " + juce::String (dOn) + " against " + juce::String (dOff));
                improved += dOn < dOff ? 1 : 0;
                rows.add (juce::String (name) + ": Same part, cleanup off " + juce::String (dOff, 2) + " dB (" + describe (off, same) + ", closeness "
                          + juce::String (juce::roundToInt (off.closeness)) + "), on " + juce::String (dOn, 2) + " dB (" + describe (on, same) + ", closeness "
                          + juce::String (juce::roundToInt (on.closeness)) + "); for information, Anything on the cleaned target " + juce::String (dAnything, 2) + " dB ("
                          + describe (onAnything, anything) + ")");
            }
            for (const auto& row : rows)
                logMessage ("  -> " + row);
            logMessage ("  -> the cleanup lowered the spectral distance to the hidden rig in " + juce::String (improved) + " of 2");
        }
    }
};

static InformedMaskTests informedMaskTests;
