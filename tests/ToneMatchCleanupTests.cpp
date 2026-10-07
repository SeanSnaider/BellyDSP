// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// Cleaning up the target with the take, in the app (docs/TONE_MATCH.md, "Cleaning up the target with your take"):
// the page's "Clean up with my take" (on by default, disabled in Anything mode with the reason shown), a
// play-along take matched with it on (the target the matcher compared is the cleaned one, exactly
// informed::cleanUp's output, and the A/B gains Raw, the target before), with it off (the target as it was, no
// Raw), cancel while it runs, and snapshots at 2x of the three states. The cleanup itself is golden-tested in
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
        beginTest ("the page: a play-along take matched with \"Clean up with my take\" on (the default), off, and in Anything mode (disabled, the reason shown); the A/B's Raw; snapshots at 2x");
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
            expect (session.getCleanup(), "on by default");
            expect (page.getCleanupSwitch().getToggleState());

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
            expect (session.referenceIsTake() && session.getMode() == ToneMatchSession::Mode::samePart);
            expect (identical (session.getReference(), di), "the take is the DI, sample for sample (a round trip of 0)");
            expect (session.cleanupApplies() && page.getCleanupSwitch().isEnabled());
            expectEquals (page.getCleanupCaption(), juce::String ("Keeps your notes' harmonics, the rest 20 dB down"));

            // On: the matcher compares the cleaned target, exactly what cleanUp makes of the section with the take.
            juce::StringArray stages;
            page.startMatch();
            expect (session.isMatching());
            while (session.isMatching() && ! session.waitForMatch (20))
                stages.addIfNotAlreadyThere (session.getStage().upToFirstOccurrenceOf (":", false, false));
            expect (session.hasResult(), session.getError());
            const auto info = session.getCleanupInfo(); // a copy: the later matches replace it
            expect (info.attempted && info.used && info.banded);
            std::atomic<bool> noCancel { false };
            const auto expectedClean = im::cleanUp (session.targetSelection(), di, ampsim::tonematch::playAlongBandSeconds, noCancel);
            expectEquals (info.pitched, expectedClean.pitched);
            expect (session.waitForCompare (60000));
            page.refresh();
            expect (identical (session.getCompareAudio (ToneMatchSession::sourceTarget), expectedClean.output), "Target is the cleaned-up target");
            expect (identical (session.getCompareAudio (ToneMatchSession::sourceTargetRaw), session.targetSelection()), "Raw is the section as it was");
            expect (session.hasRawTarget());
            expectEquals (page.getSourceChoice().getNumOptions(), 4);
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
            const auto& r = session.getResult();
            const auto onSummary = juce::String (r.slot + 1) + " at " + juce::String (r.gainDb, 1) + " dB, " + r.cab.getFileNameWithoutExtension()
                                   + ", spectral error " + juce::String (r.spectralErrorAfterEqDb, 2) + " dB";
            snap ("26_cleanup_on");

            // Off: the target as it was, no Raw.
            page.getCleanupSwitch().setToggleState (false, juce::sendNotification);
            expect (! session.getCleanup());
            page.startMatch();
            expect (session.isMatching());
            expect (session.waitForMatch (120000));
            expect (session.hasResult(), session.getError());
            expect (! session.getCleanupInfo().attempted && ! session.hasRawTarget());
            expect (session.waitForCompare (60000));
            page.refresh();
            expect (identical (session.getCompareAudio (ToneMatchSession::sourceTarget), session.targetSelection()));
            expect (session.getCompareAudio (ToneMatchSession::sourceTargetRaw).empty());
            expectEquals (page.getSourceChoice().getNumOptions(), 3);
            expect (! page.keyPressed (juce::KeyPress ('4')));
            expect (page.getCleanupText().isEmpty());
            const auto& r2 = session.getResult();
            const auto offSummary = juce::String (r2.slot + 1) + " at " + juce::String (r2.gainDb, 1) + " dB, " + r2.cab.getFileNameWithoutExtension()
                                    + ", spectral error " + juce::String (r2.spectralErrorAfterEqDb, 2) + " dB";
            snap ("27_cleanup_off");

            // Anything: disabled, and why; a match there doesn't try it even with the switch on.
            page.getCleanupSwitch().setToggleState (true, juce::sendNotification);
            page.getModeChoice().setSelected (1, juce::sendNotification);
            page.refresh();
            expect (! session.cleanupApplies() && ! page.getCleanupSwitch().isEnabled());
            expect (page.getCleanupSwitch().getToggleState(), "the choice is kept for Same part");
            const auto anythingCaption = page.getCleanupCaption();
            expect (anythingCaption.startsWith ("Only in Same part"), anythingCaption);
            page.startMatch();
            expect (session.isMatching());
            expect (session.waitForMatch (120000));
            expect (session.hasResult() && ! session.getCleanupInfo().attempted && ! session.hasRawTarget());
            expect (session.waitForCompare (60000));
            snap ("28_cleanup_disabled");
            page.getModeChoice().setSelected (0, juce::sendNotification);
            page.refresh();
            expect (page.getCleanupSwitch().isEnabled());

            logMessage ("  -> on: stages " + stages.joinIntoString (" / ") + "; " + juce::String (info.pitched) + " notes, kept " + juce::String (info.keptDb, 1)
                        + " dB of the target's energy, " + juce::String (info.seconds, 2) + " s; Target is cleanUp's output sample for sample, Raw the section; "
                        + "\"" + cleanupText + "\"; matched slot " + onSummary + ". Off: slot " + offSummary + ". Anything: \"" + anythingCaption
                        + "\"; " + shots.joinIntoString (", "));
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
