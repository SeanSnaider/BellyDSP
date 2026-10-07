// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// Cleaning up the target with the take, in the app (docs/TONE_MATCH.md, "Cleaning up the target with your take"):
// the page's "Clean up with my take" (Auto by default since Round 2, On, Off; disabled when the DI's notes don't
// line up, with the reason shown), a play-along take matched in Anything (the default now) with Auto (no
// separation: not tried; a separated target with bleed: the detector cleans it up; the cleaned target the matcher
// compared is exactly informed::cleanUp's output, and the A/B gains Raw), On, and Off; cancel while it runs; and
// snapshots at 2x. The cleanup itself is golden-tested in
// InformedMaskTests.cpp.

#include "BuiltInCaptures.h"
#include "PluginEditor.h"
#include "TestHelpers.h"
#include "ToneMatchSession.h"
#include "tonematch/InformedMask.h"

#include <cmath>

namespace
{
using namespace testing;
namespace im = ampsim::tonematch::informed;

juce::File fixtures() { return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/informed_mask"); }

std::vector<float> mono (const juce::File& file)
{
    const auto b = readWav (file);
    return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
}

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

bool identical (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (! juce::exactlyEqual (a[i], b[i]))
            return false;
    return true;
}

double rms (const juce::AudioBuffer<float>& b)
{
    double s = 0.0;
    for (int n = 0; n < b.getNumSamples(); ++n)
        s += (double) b.getSample (0, n) * b.getSample (0, n);
    return std::sqrt (s / std::max (1, b.getNumSamples()));
}

/// The processor for numSamples of silence, polling the session as the page's timer does.
juce::AudioBuffer<float> run (AmpSimProcessor& p, ToneMatchSession& session, int numSamples)
{
    juce::AudioBuffer<float> out (2, numSamples), buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (int start = 0; start + blockSize <= numSamples; start += blockSize)
    {
        buffer.clear();
        p.processBlock (buffer, midi);
        out.copyFrom (0, start, buffer, 0, 0, blockSize);
        if ((start / blockSize) % 8 == 0)
            session.poll();
    }
    return out;
}
} // namespace

class ToneMatchCleanupTests final : public juce::UnitTest
{
public:
    ToneMatchCleanupTests() : juce::UnitTest ("Tone match / cleanup in the app", "ampsim") {}

    void runTest() override
    {
        beginTest ("the page: \"Clean up with my take\" Auto (the default: not separated, separated with bleed, a DI that isn't a take), On, and Off after a play-along take in Anything; the A/B's Raw; snapshots at 2x");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            for (int i = 0; i < 4000 && p.isLoading(); ++i)
                juce::Thread::sleep (5);
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::toneMatch);
            auto& page = ed.getToneMatchPage();
            auto& session = page.getSession();
            session.setLatencySource ([] {
                platform::device::Latency l;
                l.known = true; // a round trip of 0: the take's sample 0 is the section's
                return l;
            });
            const auto shotDir = proofDir().getChildFile ("tone_match");
            shotDir.createDirectory();
            juce::StringArray shots;
            auto snap = [&] (const juce::String& name) {
                page.refresh();
                ed.refresh();
                const auto f = shotDir.getChildFile (name + ".png");
                expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), f));
                shots.add ("tone_match/" + f.getFileName());
            };

            // The fixture's record in a band (6 s, no separation), and the take of the same notes played into the input.
            expect (session.setTargetFile (fixtures().getChildFile ("target.wav")));
            session.setRange (0.0, 6.0);
            session.setCountInForTake (false);
            page.refresh();
            expect (session.getCleanupChoice() == ToneMatchSession::Cleanup::automatic, "Auto by default");
            expectEquals (page.getCleanupChoice().getSelected(), 0);

            const auto di = mono (fixtures().getChildFile ("di.wav"));
            page.toggleRecording();
            expect (session.isPlayingAlong());
            {
                int64_t at = 0;
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;
                for (int k = 0; k < (int) (6.3 * fs / blockSize); ++k)
                {
                    buffer.clear();
                    for (int n = 0; n < blockSize; ++n, ++at)
                        buffer.setSample (0, n, at < (int64_t) di.size() ? di[(size_t) at] : 0.0f);
                    p.processBlock (buffer, midi);
                    if (k % 8 == 0)
                        session.poll();
                }
            }
            session.poll();
            page.refresh();
            expect (session.getTake().valid && session.getTake().complete, session.getError());
            expect (session.referenceIsTake() && session.getMode() == ToneMatchSession::Mode::anything, "a take leaves the mode on Anything (Round 2)");
            expect (identical (session.getReference(), di), "the take is the DI, sample for sample (a round trip of 0)");
            expect (session.cleanupApplies() && page.getCleanupChoice().isEnabled(), "a take lines up in Anything too");
            expectEquals (page.getCleanupCaption(), juce::String ("Only when a separated stem has bleed to remove"));
            std::atomic<bool> noCancel { false };
            const auto expectedClean = im::cleanUp (session.targetSelection(), di, ampsim::tonematch::playAlongBandSeconds, noCancel);

            auto matchNow = [&] {
                page.startMatch();
                expect (session.isMatching());
                expect (session.waitForMatch (180000));
                expect (session.hasResult(), session.getError());
                expect (session.waitForCompare (60000));
                page.refresh();
            };
            auto summary = [&] {
                const auto& r = session.getResult();
                return juce::String (r.slot + 1) + " at " + juce::String (r.gainDb, 1) + " dB, " + r.cab.getFileNameWithoutExtension()
                       + ", spectral error " + juce::String (r.spectralErrorAfterEqDb, 2) + " dB";
            };
            auto expectCleaned = [&] (const juce::String& what) {
                const auto& info = session.getCleanupInfo();
                expect (info.attempted && info.used && info.banded, what);
                expectEquals (info.pitched, expectedClean.pitched);
                expect (identical (session.getCompareAudio (ToneMatchSession::sourceTarget), expectedClean.output), what + ": Target is the cleaned-up target");
                expect (identical (session.getCompareAudio (ToneMatchSession::sourceTargetRaw), session.targetSelection()), what + ": Raw is the section");
                expect (session.hasRawTarget());
                expectEquals (page.getSourceChoice().getNumOptions(), 4);
            };
            auto expectRaw = [&] (const juce::String& what) {
                expect (! session.getCleanupInfo().used && ! session.hasRawTarget(), what);
                expect (identical (session.getCompareAudio (ToneMatchSession::sourceTarget), session.targetSelection()), what);
                expect (session.getCompareAudio (ToneMatchSession::sourceTargetRaw).empty());
                expectEquals (page.getSourceChoice().getNumOptions(), 3);
            };

            // Auto, no separation: nothing to clean up (not even tried), the target as it is.
            matchNow();
            expect (! session.getCleanupInfo().attempted);
            expectRaw ("auto, not separated");
            expect (page.getCleanupText().isEmpty());
            const auto autoSummary = summary();
            snap ("26_cleanup_auto");

            // Auto with separation on: a stand-in separator that returns the section as it is, so the target is the
            // record in the band. The bleed detector runs after the raw match and finds the band: cleaned, matched again.
            session.setSeparator ([] (const std::vector<float>& x, const std::atomic<bool>&, const ampsim::tonematch::ProgressFn&, juce::String&) { return x; });
            session.setSeparate (true);
            matchNow();
            const auto bandInfo = session.getCleanupInfo();
            expect (bandInfo.automatic && bandInfo.measuredBleed, bandInfo.skipped);
            expectGreaterThan (bandInfo.excessDb, im::bleedExcessThresholdDb);
            expectCleaned ("auto, separated, with bleed");
            const auto autoBandText = page.getCleanupText();
            expect (autoBandText.contains ("bleed"), autoBandText);
            const auto autoBandSummary = summary();
            snap ("27_cleanup_auto_bleed");

            // Auto, separated, on the unmixed record (the same notes, nothing else): no bleed, so not cleaned up.
            expect (session.setTargetFile (fixtures().getChildFile ("record.wav")));
            session.setRange (0.0, 6.0);
            session.setReferenceSignal (di, "the take");
            matchNow();
            const auto recordInfo = session.getCleanupInfo();
            // The reference isn't a take of this target any more (another file), so the cleanup doesn't apply at all.
            expect (! recordInfo.attempted && ! session.cleanupApplies());
            const auto notTakeCaption = page.getCleanupCaption();
            expect (notTakeCaption.startsWith ("Needs your notes lined up"), notTakeCaption);
            expect (! page.getCleanupChoice().isEnabled());
            snap ("28_cleanup_disabled");
            // In Same part the notes line up (unbanded DTW), and Auto with separation always cleans up there.
            page.getModeChoice().setSelected (0, juce::sendNotification);
            page.refresh();
            expect (session.cleanupApplies() && page.getCleanupChoice().isEnabled());
            matchNow();
            expect (session.getCleanupInfo().used && ! session.getCleanupInfo().measuredBleed && ! session.getCleanupInfo().banded);

            // Back to the band and a fresh take of it, Anything, separation off: On and Off are kept as chosen.
            session.setSeparate (false);
            page.getModeChoice().setSelected (1, juce::sendNotification);
            expect (session.setTargetFile (fixtures().getChildFile ("target.wav")));
            session.setRange (0.0, 6.0);
            page.toggleRecording();
            {
                int64_t at = 0;
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;
                for (int k = 0; k < (int) (6.3 * fs / blockSize); ++k)
                {
                    buffer.clear();
                    for (int n = 0; n < blockSize; ++n, ++at)
                        buffer.setSample (0, n, at < (int64_t) di.size() ? di[(size_t) at] : 0.0f);
                    p.processBlock (buffer, midi);
                    if (k % 8 == 0)
                        session.poll();
                }
            }
            session.poll();
            expect (session.referenceIsTake() && session.getMode() == ToneMatchSession::Mode::anything);
            page.getCleanupChoice().setSelected (1, juce::sendNotification);
            expect (session.getCleanupChoice() == ToneMatchSession::Cleanup::on);
            expectEquals (page.getCleanupCaption(), juce::String ("Keeps your notes' harmonics, the rest 20 dB down"));
            matchNow();
            expectCleaned ("on");
            const auto cleanupText = page.getCleanupText();
            expect (cleanupText.contains ("cleaned up with your take"), cleanupText);
            // Level match over all four.
            double lo = 1.0e9, hi = -1.0e9;
            for (int s = 0; s < ToneMatchSession::numAllSources; ++s)
            {
                const auto l = session.getLoudness (s) + session.getLevelMatchGainDb (s);
                lo = std::min (lo, l);
                hi = std::max (hi, l);
            }
            expectLessThan (hi - lo, 0.1);
            // Raw plays (key 4), through the processor, at the level match's gain; Target (key 1) too.
            expect (page.keyPressed (juce::KeyPress ('4')));
            expectEquals (session.getPreviewSource(), (int) ToneMatchSession::sourceTargetRaw);
            page.togglePreview();
            const auto rawOut = run (p, session, (int) (1.0 * fs));
            expect (page.keyPressed (juce::KeyPress ('1')));
            const auto cleanOut = run (p, session, (int) (1.0 * fs));
            page.togglePreview();
            run (p, session, 4800);
            expect (rms (rawOut) > 1.0e-3 && rms (cleanOut) > 1.0e-3);
            const auto onSummary = summary();
            snap ("29_cleanup_on");

            page.getCleanupChoice().setSelected (2, juce::sendNotification);
            expect (session.getCleanupChoice() == ToneMatchSession::Cleanup::off);
            matchNow();
            expect (! session.getCleanupInfo().attempted);
            expectRaw ("off");
            expect (! page.keyPressed (juce::KeyPress ('4')));
            const auto offSummary = summary();
            snap ("30_cleanup_off");

            logMessage ("  -> auto, not separated: slot " + autoSummary + ". Auto, separated (the band): bleed " + juce::String (bandInfo.excessDb, 2)
                        + " dB over the tone's own, cleaned (" + juce::String (bandInfo.pitched) + " notes), \"" + autoBandText + "\", slot " + autoBandSummary
                        + ". Not a take: \"" + notTakeCaption + "\". On: \"" + cleanupText + "\", slot " + onSummary + ". Off: slot " + offSummary + "; "
                        + shots.joinIntoString (", "));
        }

        beginTest ("cancel while the cleanup runs (a minute of target and DI, unbanded): the match ends cancelled within half a second");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            ToneMatchSession session (p);
            const auto target = mono (fixtures().getChildFile ("target.wav")), di = mono (fixtures().getChildFile ("di.wav"));
            std::vector<float> longTarget, longDi;
            for (int i = 0; i < 10; ++i)
            {
                longTarget.insert (longTarget.end(), target.begin(), target.end());
                longDi.insert (longDi.end(), di.begin(), di.end());
            }
            session.setTargetSignal (longTarget, "long");
            session.setRange (0.0, 60.0);
            session.setReferenceSignal (longDi, "long DI");
            session.setMode (ToneMatchSession::Mode::samePart);
            session.setCleanup (true); // On: Auto wouldn't clean up an unseparated target
            // Slots need a capture for a match to start: the fixture capture in slot 1.
            p.loadModel (0, juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/tone_match/captures/Glass.nam"));
            for (int i = 0; i < 2000 && p.isLoading(); ++i)
                juce::Thread::sleep (5);
            expect (session.whyCantMatch().isEmpty(), session.whyCantMatch());
            expect (session.startMatch());
            juce::String stageThen;
            for (int i = 0; i < 200 && ! session.getStage().startsWith ("Cleaning up"); ++i)
                juce::Thread::sleep (1);
            juce::Thread::sleep (30);
            stageThen = session.getStage();
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            session.cancel();
            expect (session.waitForMatch (5000));
            const auto lag = juce::Time::getMillisecondCounterHiRes() - t0;
            expect (stageThen.startsWith ("Cleaning up"), stageThen);
            expect (! session.hasResult());
            expectEquals (session.getError(), juce::String ("Cancelled"));
            expect (lag < 500.0 * cpuBudgetScale(), juce::String (lag));
            logMessage ("  -> cancelled during \"" + stageThen + "\": done " + juce::String (lag, 1) + " ms later, \"" + session.getError() + "\"");
        }
    }
};

static ToneMatchCleanupTests toneMatchCleanupTests;
