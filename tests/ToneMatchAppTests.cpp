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
                    "cab_mic2_mute", "cab_room_mute", "post_fx_on",
                    // the pre effects Apply switches off, and the gates it leaves alone
                    "comp_pre_on", "boost_on", "od_on", "eq_pre_on", "gate_a_on", "gate_b_on", "pre_fx_on" });
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
            // Pre effects in a mix of states: Apply must switch the tone-coloring ones off, leave the gate
            // alone, and undo must put each back exactly as it was.
            setParam (p, "pre_fx_on", 1.0f);
            setParam (p, "comp_pre_on", 1.0f);
            setParam (p, "boost_on", 1.0f);
            setParam (p, "od_on", 0.0f);
            setParam (p, "eq_pre_on", 1.0f);
            setParam (p, "gate_a_on", 1.0f);
            p.loadCabIR (0, platform::factoryContentFolder().getChildFile ("irs/Vintage 4x12/Vintage 4x12, supercardioid, upper.wav"));
            waitForLoads (p);
            p.parameters.copyState(); // writes the settings above into the state now, before the history is cleared
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
            const auto note = page.getApplyNote();
            expectEquals (note, juce::String ("Apply sets these in one undo step: the compressors, boost, and overdrive as the match has them (it uses none), "
                                              "the pre EQ off (on now: compressor, boost, pre EQ). The gate stays as it is."));

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
            for (const auto* id : { "comp_pre_on", "boost_on", "od_on", "eq_pre_on" })
                expect (getParam (p, id) < 0.5f, juce::String (id) + " must be off after Apply");
            expect (getParam (p, "gate_a_on") > 0.5f, "Apply must leave the noise gate on");
            expect (getParam (p, "pre_fx_on") > 0.5f, "Apply leaves the pre section's own switch alone");
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
            expect (getParam (p, "boost_on") < 0.5f && getParam (p, "comp_pre_on") < 0.5f && getParam (p, "eq_pre_on") < 0.5f);

            page.discard();
            expect (! session.hasResult() && ! page.getApplyButton().isEnabled());

            logMessage ("  -> recorded " + juce::String (recorded, 3) + " s of DI through processBlock, sample-exact; matched the fixtures' 7 s target (Ember +5 dB, "
                        "Modern 4x12, dynamic, 75 W, var. 2) in anything mode: " + ampSimSlotName (p, r.slot) + " " + juce::String (r.gainDb, 1)
                        + " dB, " + r.cab.getFileNameWithoutExtension() + ", closeness " + juce::String (r.closeness, 0) + ", "
                        + juce::String (r.runtimeSeconds, 1) + " s; the page showed " + juce::String (100.0 * midProgress, 0) + "% progress "
                        + juce::String (midMs, 0) + " ms in");
            logMessage ("  -> before: " + before);
            logMessage ("  -> applied: " + after);
            logMessage ("  -> one undo: " + undone + " (identical to before, the cab and every pre effect switch included; nothing left to undo); redo re-applied; Discard cleared the result");
            logMessage ("  -> the page's Apply note: \"" + note + "\"");
            logMessage ("  -> snapshots: tone_match/" + pngs.joinIntoString (", tone_match/"));
        }

        beginTest ("Apply with a pedal, a post compressor, and a gain set no slot holds (Round 2): the set loads into the playing slot, the pedal and the compressors are set, Match renders what Apply sets, one undo step puts it all back");
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
            const auto glass = p.getSlotCapture (0);
            // Slot 3 is emptied, and slot 1 (Glass) plays; the result is Monolith's gain set, which no slot holds now.
            p.clearModel (2);
            waitForLoads (p);
            setParam (p, AmpSimProcessor::slotParamId, 0.0f);
            setParam (p, "pre_fx_on", 0.0f);
            setParam (p, "comp_pre_on", 1.0f);
            setParam (p, "boost_on", 1.0f);
            setParam (p, "comp_post_on", 0.0f);
            setParam (p, AmpSimProcessor::ampParamId (0, "output_trim"), -3.0f);
            p.parameters.copyState();
            p.undoManager.beginNewTransaction();
            p.undoManager.clearUndoHistory();
            const auto ids = juce::StringArray { "pre_fx_on", "comp_pre_on", "boost_on", "od_on", "od_mode", "od_drive", "od_tone", "comp_post_on", "comp_post_threshold",
                                                 "comp_post_ratio", "comp_post_detector", "comp_post_mix", "amp1_input_trim" };
            auto values = [&] {
                juce::StringArray v;
                for (const auto& id : ids)
                    v.add (id + "=" + juce::String (getParam (p, id), 2));
                v.add ("slot1=" + p.getSlotCapture (0).getParentDirectory().getFileName());
                return v.joinIntoString (", ");
            };
            const auto before = values();

            ampsim::tonematch::MatchResult r;
            r.mode = ampsim::tonematch::Mode::anything;
            r.model = ToneMatchSession::contentGainSets()[2];
            r.slot = 2;
            r.gainDb = 4.5;
            r.tone = { 1.0, -2.0, 0.5, 2.0, -1.0 };
            r.cab = platform::factoryContentFolder().getChildFile ("irs/Modern 4x12/Modern 4x12, dynamic, 75 W, var. 1.wav");
            r.eq[1] = { ampsim::Equalizer::BandType::peak, 1200.0f, -3.0f, 1.0f };
            r.pedal.kind = ampsim::tonematch::Pedal::Kind::overdrive;
            r.pedal.overdrive.mode = ampsim::Overdrive::Mode::distortion;
            r.pedal.overdrive.drive = 0.3f;
            r.pedal.overdrive.tone = 0.5f;
            r.pedal.overdrive.unityTrim = true;
            r.postCompressor.on = true;
            auto& c = r.postCompressor.settings;
            c.mode = ampsim::Compressor::Mode::studio;
            c.detector = ampsim::Compressor::Detector::rms;
            c.thresholdDb = -22.0f;
            c.ratio = 3.0f;
            c.attackMs = 10.0f;
            c.releaseMs = 150.0f;
            c.autoRelease = false;
            c.autoMakeup = false;
            c.makeupDb = 0.0f;
            c.mix = 1.0f;
            session.setResultForTests (r);
            page.refresh();
            expectEquals (session.applySlot(), 0, "the playing slot");
            const auto note = page.getApplyNote();
            expect (note.contains ("it uses overdrive"), note);
            const auto text = page.getResultText();
            expect (text.contains ("Monolith (loads into slot 1)") && text.contains ("Overdrive, Distortion") && text.contains ("post compressor -22.0 dB 3:1"), text);
            const auto matched = session.matchedSettings();
            ed.refresh();
            const auto shot = shots.getChildFile ("31_result_pedal.png");
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), shot));

            page.apply();
            waitForLoads (p);
            expectEquals (p.getSlotCapture (0).getFullPathName(), r.model.getFullPathName());
            expect (getParam (p, "pre_fx_on") > 0.5f && getParam (p, "od_on") > 0.5f && getParam (p, "comp_pre_on") < 0.5f && getParam (p, "boost_on") < 0.5f);
            expectEquals ((int) getParam (p, "od_mode"), (int) ampsim::Overdrive::Mode::distortion);
            expectWithinAbsoluteError (getParam (p, "od_drive"), 30.0f, 0.051f);
            expect (getParam (p, "comp_post_on") > 0.5f);
            expectWithinAbsoluteError (getParam (p, "comp_post_threshold"), -25.0f, 0.051f); // -22 dB with the Master at -3 dB
            expectEquals ((int) getParam (p, "comp_post_detector"), 1);
            // Match renders exactly what Apply set (the settings read back from the processor), the pedal and the post
            // compressor included.
            const auto di = guitarDI ((int) (3.0 * fs));
            std::atomic<bool> noCancel { false };
            auto current = session.currentSettings();
            auto m = matched;
            m.cabIR = current.cabIR;
            const auto a = ampsim::tonematch::ToneMatcher::renderTone (m, di, noCancel), b = ampsim::tonematch::ToneMatcher::renderTone (current, di, noCancel);
            expect (! a.empty() && a == b, "Match's render equals the applied settings' render, sample for sample");
            expectEquals ((int) current.pedals.size(), 1);
            expect (current.postCompressor.on);
            const auto after = values();

            expect (p.undoManager.canUndo());
            p.undoManager.undo();
            waitForLoads (p);
            expectEquals (values(), before);
            expectEquals (p.getSlotCapture (0).getFullPathName(), glass.getFullPathName());
            expect (! p.undoManager.canUndo(), "one undo step");
            logMessage ("  -> \"" + note + "\"; before: " + before + "; applied: " + after + "; Match and the applied settings render the same "
                        + juce::String ((int) a.size()) + " samples; undone: identical to before");
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
            const auto stageAtCancel = session.getStage();
            const auto cancelledAt = juce::Time::getMillisecondCounterHiRes();
            session.cancel();
            // A cancel lands within about a second on a dev Mac (ToneMatchTests' cancel test measures it); the wait
            // is scaled for CI's slower runners.
            expect (session.waitForMatch ((int) (5000.0 * testing::cpuBudgetScale())));
            const auto stoppedMs = juce::Time::getMillisecondCounterHiRes() - cancelledAt;
            page.refresh();
            expect (! session.hasResult());
            expectEquals (session.getError(), juce::String ("Cancelled"));
            expectEquals (page.getStatusText(), juce::String ("Cancelled"));
            expect (page.getMatchButton().isEnabled());
            logMessage ("  -> cancelled 300 ms into a same-part match (at \"" + stageAtCancel + "\"): stopped " + juce::String (juce::roundToInt (stoppedMs)) + " ms after the cancel; no result, the page says \""
                        + page.getStatusText() + "\", and Match is enabled again");
        }

        beginTest ("the result lines break only between items, so a value never parts from its unit");
        {
            const auto f = juce::Font (ui::theme::font (ui::theme::Text::label));
            const juce::StringArray bands { "low shelf 134 Hz -3.2 dB", "401 Hz -6.6 dB", "943 Hz +2.2 dB", "high shelf 2000 Hz -2.8 dB", "5362 Hz -1.8 dB" };
            juce::StringArray report;
            for (const auto width : { 560.0f, 420.0f, 300.0f, 200.0f })
            {
                const auto lines = ui::ToneMatchPage::packItems (f, "Match EQ", bands, width);
                // Every band is on exactly one line, whole; the lines hold all of them, in order.
                juce::StringArray rebuilt;
                for (int l = 0; l < lines.size(); ++l)
                    rebuilt.add ((l == 0 ? lines[l].fromFirstOccurrenceOf ("Match EQ  ", false, false) : lines[l]).trimCharactersAtEnd (","));
                expectEquals (rebuilt.joinIntoString (", ").replace (",,", ","), bands.joinIntoString (", "));
                int wholeBands = 0;
                for (const auto& band : bands)
                    for (const auto& line : lines)
                        wholeBands += line.contains (band) ? 1 : 0;
                expectEquals (wholeBands, bands.size());
                for (const auto& line : lines)
                    expect (! line.endsWith ("Hz") && ! line.endsWithChar ('2') && ! line.startsWith ("dB") && ! line.startsWith ("Hz"), line);
                report.add (juce::String (width, 0) + " px: " + juce::String (lines.size()) + " lines");
            }
            logMessage ("  -> five bands at " + report.joinIntoString ("; ") + "; each band whole on one line at every width");
        }

        beginTest ("a separation that fails shows its reason on the page, in full; snapshot at 2x");
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
            expect (session.setTargetFile (fixtures().getChildFile ("target_anything.wav")));
            expect (session.setReferenceFile (fixtures().getChildFile ("reference_di.wav")));
            session.setSeparate (true);
            // The message the installer gives when the server says 404 (ToneMatchSeparationTests checks the real one).
            const juce::String reason = "couldn't download the separation model: huggingface.co answered HTTP 404 Not Found (details: "
                                        + juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getFullPathName()
                                        + "/Application Support/BellyDSP/separation-log.txt)";
            session.setSeparator ([reason] (const std::vector<float>&, const std::atomic<bool>&, const ampsim::tonematch::ProgressFn&, juce::String& error) {
                error = reason;
                return std::vector<float>();
            });
            page.startMatch();
            expect (session.waitForMatch (10000));
            page.refresh();
            expectEquals (page.getStatusText(), "Separation failed: " + reason);
            const auto file = shots.getChildFile ("06_separation_error.png");
            ed.refresh();
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), file));
            logMessage ("  -> the page says \"" + page.getStatusText() + "\"; snapshot tone_match/" + file.getFileName());
        }
    }

    static juce::String ampSimSlotName (AmpSimProcessor& p, int slot) { return p.getSlotCapture (slot).getFileNameWithoutExtension(); }
};

static ToneMatchAppTests toneMatchAppTests;
} // namespace
