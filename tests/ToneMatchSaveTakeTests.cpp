// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// Tone match's Save take (docs/TONE_MATCH.md, "Learning a capture from the song (prototype)"): after a play-along
// take, the session writes the section, its stem if a separated match made one, the take as recorded and as lined
// up, and take.json into a new folder, for prototypes/learn_tone.py. Checked: every file read back sample for
// sample against what the session holds (and, with a DI that says where it was, the lined-up take's first sample
// is the one the reported round trip points at), the JSON's fields, the DTW path once a same-part match of this
// take exists, the stem after a separated match, and the page's quiet button. Nothing here touches the audio
// thread: the files are written on the message thread.

#include "BuiltInCaptures.h"
#include "PluginEditor.h"
#include "TestHelpers.h"
#include "ToneMatchSession.h"

#include <cmath>

namespace
{
using namespace testing;

juce::File fixtures() { return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/tone_match"); }

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

/// A DI that says where it was: input sample n is (n + 1) 1e-7.
float marker (int64_t n) { return (float) ((double) (n + 1) * 1.0e-7); }

/// The processor in uneven buffers with `input` as the DI; `everyBuffer` runs before each buffer (the page's timer).
void runDevice (AmpSimProcessor& p, int numSamples, const std::function<float (int64_t)>& input, const std::function<void (int64_t)>& everyBuffer)
{
    static constexpr int sizes[] = { 128, 100, 128, 37, 256, 64, 128, 1, 200 };
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    int64_t start = 0;
    int i = 0;
    while (start < numSamples)
    {
        const auto len = (int) std::min<int64_t> (sizes[i++ % (int) std::size (sizes)], numSamples - start);
        everyBuffer (start);
        juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), 2, len);
        block.clear();
        for (int n = 0; n < len; ++n)
            block.setSample (0, n, input (start + n));
        p.processBlock (block, midi);
        start += len;
    }
}

std::vector<float> mono (const juce::File& file, double* rate = nullptr)
{
    const auto b = readWav (file, rate);
    return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
}

/// A take through the session with a fake device reporting 100 + 156 samples.
void recordTake (juce::UnitTest& t, AmpSimProcessor& p, ToneMatchSession& session, const std::function<float (int64_t)>& input, double seconds,
                 std::function<void()> timer = {})
{
    bool started = false;
    runDevice (p, (int) ((seconds + 0.5) * fs), input, [&] (int64_t) {
        if (! started)
        {
            t.expect (session.startPlayAlong());
            started = true;
        }
        session.poll();
        if (timer)
            timer();
    });
    session.poll();
}

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

void play (AmpSimProcessor& p, int numSamples, ToneMatchSession& session)
{
    static const auto guitar = guitarDI ((int) (20.0 * fs));
    static int64_t at = 0;
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (int start = 0; start < numSamples; start += blockSize)
    {
        buffer.clear();
        for (int n = 0; n < blockSize; ++n)
            buffer.setSample (0, n, 0.3f * guitar[(size_t) (at++ % (int64_t) guitar.size())]);
        p.processBlock (buffer, midi);
        if ((start / blockSize) % 8 == 0)
            session.poll();
    }
}
} // namespace

class ToneMatchSaveTakeTests final : public juce::UnitTest
{
public:
    ToneMatchSaveTakeTests() : juce::UnitTest ("Tone match save take", "ampsim") {}

    void runTest() override
    {
        beginTest ("Save take: the section, the take as recorded and lined up, and take.json, each read back exactly; nothing to save before a take or for another range");
        {
            const auto parent = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("ampsim_takes", "");
            parent.createDirectory();
            auto p = std::make_unique<AmpSimProcessor>();
            p->prepareToPlay (fs, blockSize);
            setParam (*p, "output_limit_on", 0.0f);
            ToneMatchSession session (*p);
            session.setLatencySource ([] {
                platform::device::Latency l;
                l.known = true;
                l.inputSamples = 100;
                l.outputSamples = 156;
                l.bufferSize = 128;
                l.sampleRate = 48000.0;
                return l;
            });
            // The song: a slow ramp, so the section's samples are recognisable.
            std::vector<float> song ((size_t) (8.0 * fs));
            for (size_t n = 0; n < song.size(); ++n)
                song[n] = 0.1f * std::sin ((float) n * 0.001f);
            session.setTargetSignal (song, "My Song (live).mp3");
            session.setRange (2.0, 5.0);
            session.setCountInForTake (false);
            session.setLatencyOffsetMs (2.0);
            expect (! session.canSaveTake());
            expect (session.saveTake (parent) == juce::File());

            recordTake (*this, *p, session, marker, 3.0 + 0.1);
            const auto& take = session.getTake();
            expect (take.valid && take.complete, session.getError());
            expect (session.canSaveTake());
            const auto folder = session.saveTake (parent);
            expect (folder.isDirectory(), session.getError());
            expect (folder.getFileName().startsWith ("My Song (live)-"), folder.getFileName());
            expectEquals (folder.getFileName().length(), juce::String ("My Song (live)-").length() + 15); // yyyymmdd-hhmmss

            double rate = 0.0;
            const auto target = mono (folder.getChildFile ("target.wav"), &rate);
            expectEquals (rate, 48000.0);
            expect (target == session.targetSelection(), "target.wav is the section, sample for sample");
            const auto raw = mono (folder.getChildFile ("di_raw.wav"));
            expect (raw == take.raw, "di_raw.wav is the take as recorded");
            const auto di = mono (folder.getChildFile ("di.wav"));
            expect (di == session.getReference(), "di.wav is the take lined up with the section");
            expect (! folder.getChildFile ("stem.wav").exists());
            // The lined-up take starts the round trip (100 + 156) plus the offset (2 ms, 96 samples) after the song's
            // first sample: input sample t0 + 352, where raw[0] is input sample t0.
            const auto t0 = (int64_t) std::llround ((double) raw[0] / 1.0e-7) - 1;
            expect (juce::exactlyEqual (di.front(), marker (t0 + 352)) && juce::exactlyEqual (di.back(), marker (t0 + 352 + (int64_t) di.size() - 1)),
                    "di.wav's first sample is input sample " + juce::String (std::llround ((double) di.front() / 1.0e-7) - 1) + ", raw's " + juce::String (t0));
            expectEquals ((int) di.size(), (int) (3.0 * fs));

            const auto json = juce::JSON::parse (folder.getChildFile ("take.json"));
            expectEquals (json["format"].toString(), juce::String ("bellydsp-tone-match-take"));
            expectEquals ((int) json["version"], 1);
            expectEquals (json["target_file"].toString(), juce::String ("My Song (live).mp3"));
            expectWithinAbsoluteError ((double) json["range_start_s"], 2.0, 1.0e-12);
            expectWithinAbsoluteError ((double) json["range_end_s"], 5.0, 1.0e-12);
            expectEquals ((int) json["align_samples"], 352);
            expectEquals ((int) json["section_samples"], (int) (3.0 * fs));
            expectWithinAbsoluteError ((double) json["latency_ms"], 352.0 / 48.0, 1.0e-9);
            expectWithinAbsoluteError ((double) json["offset_ms"], 2.0, 1.0e-9);
            expect ((bool) json["device_latency"]["known"]);
            expectEquals ((int) json["device_latency"]["input_samples"], 100);
            expectEquals ((int) json["device_latency"]["output_samples"], 156);
            expectEquals ((int) json["device_latency"]["buffer_size"], 128);
            expect ((bool) json["complete"]);
            expect (! (bool) json["count_in"]["on"]);
            expectEquals (json["song_source"].toString(), juce::String ("full"));
            expectWithinAbsoluteError ((double) json["band_seconds"], 0.5, 1.0e-12);
            expect (json["dtw"].isVoid(), "no match yet, so no DTW path");
            expect (json["files"]["stem"].isVoid());
            expectEquals (json["files"]["di"].toString(), juce::String ("di.wav"));

            // Saved twice in the same second: a second folder, not an overwrite.
            const auto again = session.saveTake (parent);
            expect (again.isDirectory() && again != folder, again.getFileName());

            // Another range: the take isn't of it, so there's nothing to save.
            session.setRange (1.0, 4.0);
            expect (! session.canSaveTake());
            parent.deleteRecursively();
            logMessage ("  -> " + folder.getFileName() + ": target.wav " + juce::String ((int) target.size()) + " samples (the section 2 to 5 s), di_raw.wav "
                        + juce::String ((int) raw.size()) + ", di.wav " + juce::String ((int) di.size()) + " starting at input sample t0 + 352 (100 + 156 reported, + 2 ms), "
                        "take.json with align_samples 352; a second save the same second went to " + again.getFileName());
        }

        beginTest ("Save take on the page: a quiet button once there's a take (Saved after), the files in the takes folder; after a same-part match, take.json carries its DTW path; after a separated match, stem.wav; snapshot at 2x");
        {
            const auto takes = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("ampsim_takes", "");
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
            page.setTakesFolder (takes);
            session.setLatencySource ([] {
                platform::device::Latency l;
                l.known = true;
                l.inputSamples = 182;
                l.outputSamples = 214;
                return l;
            });
            // A stand-in separator (the real one is ToneMatchSeparationTests'): the "stem" is the section at half level.
            session.setSeparator ([] (const std::vector<float>& x, const std::atomic<bool>&, const ampsim::tonematch::ProgressFn&, juce::String&) {
                auto y = x;
                for (auto& v : y)
                    v *= 0.5f;
                return y;
            });
            expect (session.setTargetFile (fixtures().getChildFile ("target_same.wav")));
            session.setRange (0.5, 6.5);
            session.setCountInForTake (false);
            page.refresh();
            expect (! page.getSaveTakeButton().isVisible(), "no take yet: no button");

            // The take: the play-along fixture's DI played into the input, as a player playing the part would.
            const auto diFile = mono (fixtures().getChildFile ("target_di_same.wav"));
            page.toggleRecording();
            expect (session.isPlayingAlong());
            int64_t at = 0;
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            for (int k = 0; k < (int) (6.6 * fs / blockSize); ++k)
            {
                buffer.clear();
                for (int n = 0; n < blockSize; ++n, ++at)
                    buffer.setSample (0, n, at < (int64_t) diFile.size() ? diFile[(size_t) at] : 0.0f);
                p.processBlock (buffer, midi);
                if (k % 8 == 0)
                    session.poll();
            }
            session.poll();
            page.refresh();
            expect (session.getTake().valid && session.getTake().complete, session.getError());
            expect (page.getSaveTakeButton().isVisible() && page.getSaveTakeButton().isEnabled());
            expectEquals (page.getSaveTakeButton().getButtonText(), juce::String ("Save take"));

            page.saveTake();
            const auto first = page.getLastSavedTake();
            expect (first.isDirectory() && first.getParentDirectory() == takes, first.getFullPathName());
            expectEquals (page.getSaveTakeButton().getButtonText(), juce::String ("Saved"));
            expect (! page.getSaveTakeButton().isEnabled());
            const auto savedStatus = page.getReferenceStatus();
            expect (savedStatus.endsWith (", saved"), savedStatus);
            for (const auto* f : { "target.wav", "di_raw.wav", "di.wav", "take.json" })
                expect (first.getChildFile (f).existsAsFile(), f);
            const auto shot = proofDir().getChildFile ("tone_match").getChildFile ("25_take_saved.png");
            shot.getParentDirectory().createDirectory();
            ed.refresh();
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), shot));

            // A same-part match of this take: its DTW path goes into take.json.
            expect (session.getMode() == ToneMatchSession::Mode::samePart);
            expect (session.startMatch());
            expect (session.waitForMatch (120000));
            expect (session.hasResult(), session.getError());
            const auto withPath = session.saveTake (takes);
            const auto json = juce::JSON::parse (withPath.getChildFile ("take.json"));
            const auto* path = json["dtw"]["path"].getArray();
            expect (path != nullptr && path->size() == (int) session.getResult().alignmentPath.size());
            if (path != nullptr && ! path->isEmpty())
            {
                expectEquals ((int) (*path)[0][0], session.getResult().alignmentPath.front().first);
                expectEquals ((int) path->getLast()[1], session.getResult().alignmentPath.back().second);
            }
            expectEquals ((int) json["dtw"]["hop"], 2048);
            expectEquals ((int) json["dtw"]["slot"], session.getResult().slot);
            expect (! withPath.getChildFile ("stem.wav").exists());

            // A separated match of the same section: stem.wav is the stem, sample for sample.
            session.setSeparate (true);
            expect (session.startMatch());
            expect (session.waitForMatch (120000));
            expect (session.hasGuitarStem());
            const auto withStem = session.saveTake (takes);
            const auto stem = mono (withStem.getChildFile ("stem.wav"));
            const auto selection = session.targetSelection();
            bool half = stem.size() == selection.size();
            for (size_t n = 0; half && n < stem.size(); ++n)
                half = juce::exactlyEqual (stem[n], 0.5f * selection[n]);
            expect (half, "stem.wav is the separated section (the stand-in's half level)");
            expectEquals (juce::JSON::parse (withStem.getChildFile ("take.json"))["files"]["stem"].toString(), juce::String ("stem.wav"));

            // A new take: the button offers Save take again.
            page.toggleRecording();
            play (p, (int) (6.8 * fs), session);
            session.poll();
            page.refresh();
            expect (session.getTake().valid);
            expectEquals (page.getSaveTakeButton().getButtonText(), juce::String ("Save take"));
            expect (page.getSaveTakeButton().isEnabled());
            takes.deleteRecursively();
            logMessage ("  -> the page saved " + first.getFileName() + " (button Saved, status \"" + savedStatus + "\"); after a same-part match take.json has the "
                        + juce::String (path != nullptr ? path->size() : 0) + "-step DTW path; after a separated match stem.wav is the stem; tone_match/" + shot.getFileName());
        }
    }
};

static ToneMatchSaveTakeTests toneMatchSaveTakeTests;
