// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// Tone match in the app (docs/TONE_MATCH.md): the DI recorder, the page and its session (a match from the
// page, Apply as one undo step, undo, redo, Discard), the brand menu's way in, and editor snapshots of the
// page at 2x.

#include "BuiltInCaptures.h"
#include "PluginEditor.h"
#include "TestHelpers.h"
#include "platform/AppInfo.h"
#include "tonematch/DiRecorder.h"

namespace
{
using namespace testing;

juce::File fixtures() { return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/tone_match"); }

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

float getParam (AmpSimProcessor& p, const juce::String& id) { return p.parameters.getRawParameterValue (id)->load(); }

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

void play (AmpSimProcessor& p, const std::vector<float>& input)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
    }
}

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

juce::String cabPath (AmpSimProcessor& p) { return p.parameters.state.getProperty (AmpSimProcessor::cabPathKey (0)).toString(); }

/// The values Apply writes (every slot's Gain and tone, the post EQ, the cab), as a string, to compare before and after.
juce::String snapshotOf (AmpSimProcessor& p)
{
    juce::StringArray ids { AmpSimProcessor::slotParamId };
    for (int slot = 0; slot < 3; ++slot)
        for (const auto* name : { "input_trim", "depth", "bass", "mid", "treble", "presence" })
            ids.add (AmpSimProcessor::ampParamId (slot, name));
    ids.addArray ({ "eq_post_on", "eq_post_mode", "eq_post_b1_type", "eq_post_b1_gain", "eq_post_b2_freq", "eq_post_b3_q", "eq_post_b5_gain",
                    "cab_mic2_mute", "cab_room_mute", "post_fx_on" });
    juce::StringArray values;
    for (const auto& id : ids)
        values.add (id + "=" + juce::String (getParam (p, id), 2));
    values.add ("cab=" + juce::File (cabPath (p)).getFileNameWithoutExtension());
    return values.joinIntoString (", ");
}

class ToneMatchAppTests final : public juce::UnitTest
{
public:
    ToneMatchAppTests() : juce::UnitTest ("Tone match app", "ampsim") {}

    void runTest() override
    {
        const auto shots = proofDir().getChildFile ("tone_match");
        shots.createDirectory();

        beginTest ("the DI recorder: a producer thread's samples arrive exactly, it stops itself at a minute, and a stalled reader loses nothing");
        {
            ampsim::tonematch::DiRecorder recorder;
            recorder.prepare();
            const auto source = guitarDI ((int) (70.0 * fs));
            recorder.start();
            std::atomic<bool> producing { true };
            std::thread audio ([&] {
                // 70 s in 128-sample buffers, as fast as it goes (no reader for the first 20 s).
                for (size_t start = 0; start + blockSize <= source.size(); start += blockSize)
                    recorder.push (source.data() + start, blockSize);
                producing = false;
            });
            juce::Thread::sleep (20); // the reader stalls while the producer races ahead
            while (producing || recorder.isRecording())
            {
                recorder.drain();
                if (! producing)
                    break;
            }
            audio.join();
            recorder.drain();
            const auto& rec = recorder.getRecording();
            const auto exact = rec.size() == (size_t) ampsim::tonematch::DiRecorder::maxSamples
                               && std::equal (rec.begin(), rec.end(), source.begin());
            expect (exact, juce::String ((int) rec.size()));
            expect (! recorder.isRecording());
            const auto dropped = recorder.takeDroppedCount(); // what arrived after the minute filled the ring (not recorded anyway)
            logMessage ("  -> 70 s pushed in 128-sample buffers by a producer thread: recorded " + juce::String (recorder.recordedSeconds(), 3)
                        + " s, sample-exact, then stopped by itself; ring capacity 2^22 samples (87 s), so a stalled reader loses nothing "
                          "within the minute (" + juce::String ((juce::int64) dropped) + " samples past the end were refused)");
        }

        beginTest ("the brand menu opens the tone match page; Close goes back to the page before it; it's never restored at startup");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::cab);
            const auto menu = ed.brandMenu();
            const juce::PopupMenu::Item* item = nullptr;
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                if (it.getItem().text == "Match tone...")
                    item = &it.getItem();
            expect (item != nullptr && item->isEnabled);
            if (item != nullptr && item->action)
                item->action();
            expect (ed.getShownPage() == ui::PageId::toneMatch && ed.getToneMatchPage().isVisible());
            expectEquals (p.parameters.state.getProperty ("uiPage").toString(), juce::String ("cab"));
            if (ed.getToneMatchPage().onClose)
                ed.getToneMatchPage().onClose();
            expect (ed.getShownPage() == ui::PageId::cab);
            logMessage ("  -> \"Match tone...\" showed the page over the cab page; Close went back to the cab page; the saved page stayed \"cab\"");
        }

        beginTest ("the page: record the DI through the app, load a target, match, Apply in one undo step (knobs, EQ, cab), undo, redo, Discard; snapshots at 2x");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::toneMatch);
            auto& page = ed.getToneMatchPage();
            auto& session = page.getSession();
            auto snap = [&] (const juce::String& name) {
                page.refresh();
                ed.refresh();
                const auto file = shots.getChildFile (name + ".png");
                expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), file));
                return file.getFileName();
            };
            juce::StringArray pngs;
            pngs.add (snap ("01_empty"));

            // Record: the page's Record, 4 s of guitar through the processor's audio callback, Stop.
            page.toggleRecording();
            expect (session.isRecording());
            const auto di = guitarDI ((int) (4.0 * fs));
            play (p, std::vector<float> (di.begin(), di.begin() + (std::ptrdiff_t) (2.0 * fs)));
            session.poll();
            pngs.add (snap ("02_recording"));
            play (p, std::vector<float> (di.begin() + (std::ptrdiff_t) (2.0 * fs), di.end()));
            page.toggleRecording();
            expect (! session.isRecording());
            const auto recorded = session.getReferenceSeconds();
            expectWithinAbsoluteError (recorded, 4.0, 0.01);
            const auto exact = session.getReference().size() == (size_t) (4.0 * fs) / blockSize * blockSize
                               && std::equal (session.getReference().begin(), session.getReference().end(), di.begin());
            expect (exact, "the recording must be the DI the processor took in");

            // The real test material: the fixtures' target (Ember +5 dB through a modern 4x12) and the reference DI.
            expect (session.setTargetFile (fixtures().getChildFile ("target_anything.wav")));
            expect (session.setReferenceFile (fixtures().getChildFile ("reference_di.wav")));
            page.getModeChoice().setSelected (1, juce::sendNotification); // Anything
            session.setRange (1.0, 8.0);
            page.refresh();
            expect (page.getMatchButton().isEnabled(), page.getStatusText());

            // Settings that Apply must change, and that undo must bring back.
            setParam (p, AmpSimProcessor::slotParamId, 0.0f);
            setParam (p, "eq_post_mode", 0.0f);
            p.loadCabIR (0, platform::factoryContentFolder().getChildFile ("irs/Vintage 4x12/Vintage 4x12, supercardioid, upper.wav"));
            waitForLoads (p);
            p.undoManager.beginNewTransaction();
            p.undoManager.clearUndoHistory();
            const auto before = snapshotOf (p);
            const auto cabBefore = cabPath (p);

            page.startMatch();
            expect (session.isMatching());
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            while (session.getProgress() < 0.3 && juce::Time::getMillisecondCounterHiRes() - t0 < 60000.0)
                juce::Thread::sleep (20);
            const auto midProgress = session.getProgress();
            const auto midMs = juce::Time::getMillisecondCounterHiRes() - t0;
            pngs.add (snap ("03_matching"));
            expect (session.waitForMatch (120000));
            page.refresh();
            expect (session.hasResult(), session.getError());
            const auto r = session.getResult(); // a copy: Discard clears the session's
            pngs.add (snap ("04_result"));

            page.apply();
            waitForLoads (p);
            const auto after = snapshotOf (p);
            expectEquals ((int) getParam (p, AmpSimProcessor::slotParamId), r.slot);
            expectWithinAbsoluteError (getParam (p, AmpSimProcessor::ampParamId (r.slot, "input_trim")), (float) r.gainDb, 0.051f);
            expectWithinAbsoluteError (getParam (p, AmpSimProcessor::ampParamId (r.slot, "mid")), (float) r.tone[2], 0.051f);
            expectEquals ((int) getParam (p, "eq_post_mode"), 1);
            expectWithinAbsoluteError (getParam (p, "eq_post_b3_gain"), r.eq[2].gainDb, 0.051f);
            expectWithinAbsoluteError (getParam (p, "eq_post_b3_freq"), r.eq[2].frequency, r.eq[2].frequency * 0.01f);
            expectEquals (cabPath (p), r.cab.getFullPathName());
            expectEquals (p.getCabAssignment (r.slot).getFullPathName(), r.cab.getFullPathName());
            expect (getParam (p, "cab_mic2_mute") > 0.5f && getParam (p, "cab_room_mute") > 0.5f);
            pngs.add (snap ("05_applied"));

            // One undo step puts everything back, the cab included; redo applies it again.
            expect (p.undoManager.canUndo());
            p.undoManager.undo();
            waitForLoads (p);
            const auto undone = snapshotOf (p);
            expectEquals (undone, before);
            expectEquals (cabPath (p), cabBefore);
            const auto canUndoMore = p.undoManager.canUndo();
            expect (! canUndoMore, "Apply must be a single undo step");
            p.undoManager.redo();
            waitForLoads (p);
            expectEquals (cabPath (p), r.cab.getFullPathName());
            expectEquals ((int) getParam (p, AmpSimProcessor::slotParamId), r.slot);

            page.discard();
            expect (! session.hasResult() && ! page.getApplyButton().isEnabled());

            logMessage ("  -> recorded " + juce::String (recorded, 3) + " s of DI through processBlock, sample-exact; matched the fixtures' 7 s target (Ember +5 dB, "
                        "Modern 4x12, dynamic, 75 W, var. 2) in anything mode: " + ampSimSlotName (p, r.slot) + " " + juce::String (r.gainDb, 1)
                        + " dB, " + r.cab.getFileNameWithoutExtension() + ", closeness " + juce::String (r.closeness, 0) + ", "
                        + juce::String (r.runtimeSeconds, 1) + " s; the page showed " + juce::String (100.0 * midProgress, 0) + "% progress "
                        + juce::String (midMs, 0) + " ms in");
            logMessage ("  -> before: " + before);
            logMessage ("  -> applied: " + after);
            logMessage ("  -> one undo: " + undone + " (identical to before, the cab included; nothing left to undo); redo re-applied; Discard cleared the result");
            logMessage ("  -> snapshots: tone_match/" + pngs.joinIntoString (", tone_match/"));
        }

        beginTest ("cancelling from the page stops the match and says so, and the page can match again");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            waitForLoads (p);
            ui::ToneMatchPage page (p);
            auto& session = page.getSession();
            expect (session.setTargetFile (fixtures().getChildFile ("target_same.wav")));
            expect (session.setReferenceFile (fixtures().getChildFile ("reference_di.wav")));
            page.getModeChoice().setSelected (0, juce::sendNotification); // Same part
            page.startMatch();
            juce::Thread::sleep (300);
            session.cancel();
            expect (session.waitForMatch (5000));
            page.refresh();
            expect (! session.hasResult());
            expectEquals (session.getError(), juce::String ("Cancelled"));
            expectEquals (page.getStatusText(), juce::String ("Cancelled"));
            expect (page.getMatchButton().isEnabled());
            logMessage ("  -> cancelled 300 ms into a same-part match: no result, the page says \"" + page.getStatusText() + "\", and Match is enabled again");
        }
    }

    static juce::String ampSimSlotName (AmpSimProcessor& p, int slot) { return p.getSlotCapture (slot).getFileNameWithoutExtension(); }
};

static ToneMatchAppTests toneMatchAppTests;
} // namespace
