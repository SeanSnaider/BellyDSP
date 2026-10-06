// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// Tone match's play along (docs/TONE_MATCH.md, "Play along"): the preview player's count-in (click times to
// the sample at the tempo, the song one beat after the last click, off: at once), play once, the song and
// its guitar on one clock; then through the processor with a fake device (processBlock in uneven buffers):
// the recorder's sample 0 is the song's first sample, the latency compensation shifts the take by exactly
// the reported latencies (and a loopback "player" with that round trip lines up to the sample), the take
// stops at the section's end by itself, the live path is untouched (bit for bit with the song silent) and
// the output is live plus song at its level; and the tempo suggestion against its prototype.

#include "TestHelpers.h"
#include "ToneMatchSession.h"
#include "dsp/PreviewPlayer.h"
#include "tonematch/TempoEstimate.h"

#include <cmath>

namespace
{
using namespace testing;
using Player = ampsim::PreviewPlayer;

juce::File fixtures() { return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/tone_match"); }

std::shared_ptr<const std::vector<float>> shared (std::vector<float> x) { return std::make_shared<const std::vector<float>> (std::move (x)); }

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

/// Uneven buffer sizes, as a device that doesn't keep to one (and the processor's own splitting) would.
constexpr int unevenSizes[] = { 128, 100, 128, 37, 256, 64, 128, 1, 200 };

/// Runs the player alone in uneven blocks over silence; returns the left channel and, for each sample, the
/// count-in's remaining beats as the player reported them after that sample's block, and the global index
/// where songStartedAt() said a start's first sample sounded (-1 if never).
struct PlayerRun
{
    std::vector<float> out;
    std::vector<int> countIn;
    int64_t songStart = -1;
};

PlayerRun runPlayer (Player& player, int numSamples)
{
    PlayerRun r;
    r.out.resize ((size_t) numSamples);
    r.countIn.resize ((size_t) numSamples);
    juce::AudioBuffer<float> buffer (2, 256);
    int start = 0, i = 0;
    while (start < numSamples)
    {
        const auto len = std::min (unevenSizes[i++ % (int) std::size (unevenSizes)], numSamples - start);
        buffer.clear();
        player.process (juce::dsp::AudioBlock<float> (buffer).getSubBlock (0, (size_t) len), {});
        if (const auto k = player.songStartedAt(); k >= 0 && r.songStart < 0)
            r.songStart = start + k;
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + len, r.out.begin() + start);
        std::fill (r.countIn.begin() + start, r.countIn.begin() + start + len, player.getCountInRemaining());
        start += len;
    }
    return r;
}

/// A fake device: processBlock in uneven buffers. The input at sample n comes from `input` (which may read
/// the output history, for a loopback); the left output is kept. `everyBuffer` runs before each one (the
/// page's timer, in effect).
std::vector<float> runDevice (AmpSimProcessor& p, int numSamples, const std::function<float (int64_t, const std::vector<float>&)>& input,
                              const std::function<void (int64_t)>& everyBuffer = {})
{
    std::vector<float> out;
    out.reserve ((size_t) numSamples);
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    int64_t start = 0;
    int i = 0;
    while (start < numSamples)
    {
        const auto len = (int) std::min<int64_t> (unevenSizes[i++ % (int) std::size (unevenSizes)], numSamples - start);
        if (everyBuffer)
            everyBuffer (start);
        juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), 2, len);
        block.clear();
        for (int n = 0; n < len; ++n)
            block.setSample (0, n, input (start + n, out));
        p.processBlock (block, midi);
        out.insert (out.end(), block.getReadPointer (0), block.getReadPointer (0) + len);
        start += len;
    }
    return out;
}

/// A DI that says where it was: sample n is (n + 1) 1e-7 (exact in float up to 2^24 samples, and far too
/// quiet to matter to the output).
float marker (int64_t n) { return (float) ((double) (n + 1) * 1.0e-7); }

/// A processor for the device tests: no captures (the amp passes its input), the limiter off, so the
/// output is exactly the live path plus the player.
std::unique_ptr<AmpSimProcessor> makeProcessor()
{
    auto p = std::make_unique<AmpSimProcessor>();
    p->prepareToPlay (fs, blockSize);
    setParam (*p, "output_limit_on", 0.0f);
    return p;
}

/// The first sample at or after `from` where |a - b| exceeds `threshold`, or -1.
int64_t firstDifference (const std::vector<float>& a, const std::vector<float>& b, size_t from, double threshold)
{
    for (size_t n = from; n < std::min (a.size(), b.size()); ++n)
        if (std::abs ((double) a[n] - (double) b[n]) > threshold)
            return (int64_t) n;
    return -1;
}
} // namespace

class ToneMatchPlayAlongTests final : public juce::UnitTest
{
public:
    ToneMatchPlayAlongTests() : juce::UnitTest ("Tone match play along", "ampsim") {}

    void runTest() override
    {
        beginTest ("count-in: each click on its sample at the tempo (133 BPM, a beat of 21654.1 samples), the song one beat after the last, 4 3 2 1 shown, in uneven buffers");
        {
            juce::StringArray lines;
            for (const auto& [beats, bpm] : std::initializer_list<std::pair<int, double>> { { 4, 133.0 }, { 2, 133.0 }, { 4, 120.0 }, { 2, 97.3 } })
            {
                Player player;
                player.prepare (fs, 256);
                auto m = std::make_unique<Player::Material>();
                m->audio[3] = shared (std::vector<float> (96000, 1.0f)); // the song: ones, so its fade-in shows where it starts
                m->clickAccent = shared ({ 0.5f });                     // one-sample clicks, so each is seen to the sample
                m->click = shared ({ 0.25f });
                player.setMaterial (std::move (m));
                player.setLoop (2, 0, 96000);
                player.setSource (3);
                player.setMuteLive (false);
                player.setOnce (true);
                const auto beat = 60.0 * fs / bpm;
                player.setCountIn (beats, beat);
                player.startFresh();
                const auto songAt = (int64_t) std::llround (beats * beat);
                const auto r = runPlayer (player, (int) songAt + 4800);

                // Clicks: exactly at round(k beat), the first accented, nothing else before the song.
                std::vector<int64_t> clicks;
                bool accentFirst = false, othersPlain = true;
                for (int64_t n = 0; n < songAt; ++n)
                    if (r.out[(size_t) n] != 0.0f)
                    {
                        if (clicks.empty())
                            accentFirst = r.out[(size_t) n] == 0.5f;
                        else
                            othersPlain = othersPlain && r.out[(size_t) n] == 0.25f;
                        clicks.push_back (n);
                    }
                bool onTime = (int) clicks.size() == beats;
                for (int k = 0; k < (int) clicks.size(); ++k)
                    onTime = onTime && clicks[(size_t) k] == (int64_t) std::llround (k * beat);
                expect (onTime && accentFirst && othersPlain, juce::String ((int) clicks.size()) + " clicks");

                // The song: its first sample (gain sin 0 = 0) at round(beats beat), so the first sound one later;
                // and the player said so in that sample's block.
                const auto firstSound = firstDifference (r.out, std::vector<float> (r.out.size(), 0.0f), (size_t) clicks.back() + 1, 0.0);
                expectEquals ((juce::int64) firstSound, (juce::int64) songAt + 1);
                expectEquals ((juce::int64) r.songStart, (juce::int64) songAt);
                expectEquals ((juce::int64) (songAt - clicks.back()), (juce::int64) (std::llround (beats * beat) - std::llround ((beats - 1) * beat)));

                // The count shown: beats .. 1, each from its click, 0 from the song on.
                bool countRight = true;
                for (int k = 0; k < beats; ++k)
                    countRight = countRight && r.countIn[(size_t) clicks[(size_t) k] + 300] == beats - k;
                countRight = countRight && r.countIn[(size_t) songAt + 300] == 0;
                expect (countRight);

                juce::StringArray at;
                for (auto c : clicks)
                    at.add (juce::String (c));
                lines.add (juce::String (beats) + " beats at " + juce::String (bpm, 1) + " BPM (beat " + juce::String (beat, 2) + " samples): clicks at "
                           + at.joinIntoString (", ") + "; the song's first sample at " + juce::String (r.songStart) + " = round(" + juce::String (beats)
                           + " x beat), reported by songStartedAt() in its block");
            }
            for (const auto& l : lines)
                logMessage ("  -> " + l);
        }

        beginTest ("count-in off: the song starts on the first sample; play once ends at the section's end, silent and idle; the song and its guitar keep the exact place");
        {
            Player player;
            player.prepare (fs, 256);
            auto m = std::make_unique<Player::Material>();
            std::vector<float> ramp (48000), ramp2 (48000);
            for (size_t n = 0; n < ramp.size(); ++n)
            {
                ramp[n] = (float) (1.0e-5 * (double) (n + 1));
                ramp2[n] = (float) (2.0e-5 * (double) (n + 1));
            }
            m->audio[3] = shared (ramp);
            m->audio[4] = shared (ramp2);
            m->clickAccent = shared ({ 0.5f });
            m->click = shared ({ 0.25f });
            player.setMaterial (std::move (m));
            player.setMuteLive (false);
            player.setLoop (2, 0, 48000);
            player.setSource (3);
            player.setOnce (true);
            player.setCountIn (0, 24000.0);
            player.startFresh();
            auto head = runPlayer (player, 12000);
            expectEquals ((juce::int64) head.songStart, (juce::int64) 0);
            expectEquals (head.out[0], 0.0f);
            expectGreaterThan (head.out[1], 0.0f);

            // Song -> guitar only, 12000 samples in: the same clock, so the next sample of the other one.
            player.setSource (4);
            auto sw = runPlayer (player, 2000);
            const auto pos = (double) sw.out.back() / 2.0e-5 - 1.0;
            expectWithinAbsoluteError (pos, 12000.0 + 1999.0, 0.05);

            // To the end: the last 20 ms fade out, then silence; finished, and idle.
            auto tail = runPlayer (player, 48000 - 14000 + 4800);
            const auto end = (size_t) (48000 - 14000);
            bool silentAfter = true;
            for (size_t n = end; n < tail.out.size(); ++n)
                silentAfter = silentAfter && tail.out[n] == 0.0f;
            expect (silentAfter);
            expect (tail.out[end - 960 - 10] != 0.0f);
            expect (player.hasFinished());
            expect (! player.isActive());
            logMessage ("  -> count-in off: the song's first sample on sample 0 of the first buffer; a switch to the guitar 12000 samples in continued at "
                        + juce::String (pos, 2) + " after 1999 more (exact); played once to the section's end, then silent, finished and idle");
        }

        beginTest ("through the processor: recording sample 0 is the song's first sample (count-in on and off, uneven buffers), and the output is live + song at its level");
        {
            juce::StringArray lines;
            for (const auto countIn : { true, false })
            {
                auto withTake = makeProcessor();
                auto plain = makeProcessor();
                ToneMatchSession session (*withTake);
                session.setLatencySource ([] { return platform::device::Latency {}; }); // a device reporting no latency
                session.setTargetSignal (sine (220.0, 0.3, (int) (8.0 * fs)), "sine");
                session.setRange (1.0, 5.0);
                session.setCountInForTake (countIn);
                session.setCountInBeats (4);
                session.setCountInBpm (150.0);
                session.setSongLevelDb (-6.0f);
                session.setClickLevelDb (-12.0f);
                const auto total = (int) ((countIn ? 1.6 : 0.0) * fs + 4.0 * fs + 9600);
                bool started = false;
                const auto in = [] (int64_t n, const std::vector<float>&) { return marker (n); };
                const auto outTake = runDevice (*withTake, total, in, [&] (int64_t at) {
                    if (! started && at >= 4800)
                    {
                        expect (session.startPlayAlong());
                        started = true;
                    }
                    session.poll();
                });
                const auto outPlain = runDevice (*plain, total, in);
                session.poll();

                // Where the song starts: the first difference after the count-in (the song is sin(220 t) faded in
                // from 0, so its first sample adds nothing: the start is one before the first difference).
                // (After the last click has died away: Record was pressed in the buffer starting at or just after
                // sample 4800, the last click is 3 beats later and lasts 40 ms.)
                const auto afterClicks = (size_t) (countIn ? 4800 + 256 + std::llround (3.0 * 60.0 * fs / 150.0) + (int) (0.040 * fs) : 4800);
                const auto first = firstDifference (outTake, outPlain, afterClicks, 1.0e-9);
                const auto songStart = first - 1;
                const auto& t = session.getTake();
                expect (t.valid && t.complete);
                expect (! t.raw.empty());
                std::vector<float> expectedRaw (t.raw.size());
                for (size_t k = 0; k < expectedRaw.size(); ++k)
                    expectedRaw[k] = marker (songStart + (int64_t) k);
                const auto exact = ! t.raw.empty() && std::equal (t.raw.begin(), t.raw.end(), expectedRaw.begin());
                expect (exact, "recording sample 0 = input sample " + juce::String (t.raw.empty() ? -1.0 : (double) t.raw[0] / 1.0e-7 - 1.0, 1)
                                   + ", the song started at " + juce::String (songStart));

                // Output = live + song x level: past the fade-in, the difference is the section times -6 dB.
                const auto section = session.targetSelection();
                const auto g = std::pow (10.0, -6.0 / 20.0);
                double worst = 0.0;
                for (size_t k = 960; k < section.size() - 960; ++k)
                    worst = std::max (worst, std::abs ((double) outTake[(size_t) songStart + k] - (double) outPlain[(size_t) songStart + k] - g * section[k]));
                expectLessThan (worst, 1.0e-6);
                lines.add (juce::String (countIn ? "count-in 4 at 150 BPM" : "no count-in") + ": the song's first sample at output sample " + juce::String (songStart)
                           + " (" + juce::String ((double) (songStart - 4800) / fs, 4) + " s after Record), the recording's sample 0 is input sample "
                           + juce::String ((double) t.raw[0] / 1.0e-7 - 1.0, 0) + ", all " + juce::String ((int) t.raw.size())
                           + " samples in order; output minus the same processor without the take = the section x -6 dB within " + juce::String (worst, 9));
            }
            for (const auto& l : lines)
                logMessage ("  -> " + l);
        }

        beginTest ("latency compensation: the take is shifted by exactly input + output latency (+ the offset), and a loopback with that round trip lines up to the sample");
        {
            juce::StringArray lines;
            struct Run
            {
                int in, out;
                double offsetMs;
                int trueRoundTrip;
            };
            for (const auto& run : { Run { 137, 211, 0.0, 348 }, Run { 137, 211, 2.0, 348 }, Run { 0, 0, 0.0, 348 } })
            {
                auto p = makeProcessor();
                // A quiet live path: the loopback feeds the output back, so the feedback stays at -54 dB a round trip.
                setParam (*p, "input_gain", -24.0f);
                setParam (*p, "output_gain", -24.0f);
                ToneMatchSession session (*p);
                session.setLatencySource ([run] {
                    platform::device::Latency l;
                    l.known = true;
                    l.inputSamples = run.in;
                    l.outputSamples = run.out;
                    return l;
                });
                session.setLatencyOffsetMs (run.offsetMs);
                const auto song = whiteNoise ((int) (6.0 * fs), 0.25f, 9);
                session.setTargetSignal (song, "noise");
                session.setRange (1.0, 4.5);
                session.setCountInForTake (false);
                bool started = false;
                // The loopback: what the device plays comes back trueRoundTrip samples later, at half the level.
                const auto loop = [&run] (int64_t n, const std::vector<float>& out) {
                    return n >= run.trueRoundTrip ? 0.5f * out[(size_t) (n - run.trueRoundTrip)] : 0.0f;
                };
                runDevice (*p, (int) (4.5 * fs), loop, [&] (int64_t at) {
                    if (! started && at >= 2400)
                    {
                        expect (session.startPlayAlong());
                        started = true;
                    }
                    session.poll();
                });
                session.poll();
                const auto& t = session.getTake();
                expect (t.valid && t.complete);
                const auto expectedShift = run.in + run.out + (int64_t) std::llround (run.offsetMs * 48.0);
                expectEquals ((juce::int64) t.alignSamples, (juce::int64) expectedShift);
                expectEquals ((juce::int64) t.raw.size(), (juce::int64) (t.sectionSamples + expectedShift));
                const auto& ref = session.getReference();
                expectEquals ((juce::int64) ref.size(), (juce::int64) t.sectionSamples);
                const auto sameAsRaw = std::equal (ref.begin(), ref.end(), t.raw.begin() + (std::ptrdiff_t) expectedShift);
                expect (sameAsRaw, "the reference is the take from the shift on");

                // The reference against the section: the lag of the cross-correlation's peak (-400..400).
                const auto section = session.targetSelection();
                int bestLag = 0;
                double best = -1.0e9;
                for (int lag = -400; lag <= 400; ++lag)
                {
                    double c = 0.0;
                    for (size_t k = 4800; k + 4800 < section.size(); k += 3)
                        c += (double) ref[(size_t) ((int64_t) k + lag)] * section[k];
                    if (c > best)
                    {
                        best = c;
                        bestLag = lag;
                    }
                }
                const auto expectedLag = run.trueRoundTrip - (int) expectedShift;
                expectEquals (bestLag, expectedLag);
                expect (session.referenceIsTake() && session.getMode() == ToneMatchSession::Mode::samePart);
                lines.add ("reported in " + juce::String (run.in) + " + out " + juce::String (run.out) + " + offset " + juce::String (run.offsetMs, 1)
                           + " ms: the take shifted by " + juce::String (t.alignSamples) + " samples (" + juce::String ((double) t.alignSamples / 48.0, 2)
                           + " ms); against a loopback with a " + juce::String (run.trueRoundTrip) + "-sample round trip the reference lags the section by "
                           + juce::String (bestLag) + " samples");
            }
            for (const auto& l : lines)
                logMessage ("  -> " + l);
        }

        beginTest ("the take stops by itself at the section's end (plus the round trip); Stop earlier keeps what lines up; the live path isn't muted, bit for bit with the song silent");
        {
            auto withTake = makeProcessor();
            auto plain = makeProcessor();
            setParam (*withTake, "gate_a_on", 1.0f);
            setParam (*plain, "gate_a_on", 1.0f);
            ToneMatchSession session (*withTake);
            session.setLatencySource ([] {
                platform::device::Latency l;
                l.known = true;
                l.inputSamples = 100;
                l.outputSamples = 156;
                return l;
            });
            session.setTargetSignal (std::vector<float> ((size_t) (6.0 * fs), 0.0f), "silence"); // the song is silent
            session.setRange (0.0, 3.0);
            session.setCountInForTake (false); // and no clicks: the player sounds nothing at all
            const auto guitar = guitarDI ((int) (5.0 * fs));
            bool started = false;
            int64_t stoppedAt = -1;
            const auto in = [&guitar] (int64_t n, const std::vector<float>&) { return guitar[(size_t) n]; };
            const auto a = runDevice (*withTake, (int) guitar.size(), in, [&] (int64_t at) {
                if (! started && at >= 4800)
                {
                    expect (session.startPlayAlong());
                    started = true;
                }
                session.poll();
                if (started && stoppedAt < 0 && ! session.isRecording())
                    stoppedAt = at;
            });
            const auto b = runDevice (*plain, (int) guitar.size(), in);
            const auto identical = a.size() == b.size() && std::equal (a.begin(), a.end(), b.begin());
            expect (identical, "with the song silent the take's output equals the plain processor's, sample for sample");
            const auto& t = session.getTake();
            expect (t.valid && t.complete);
            expectEquals ((juce::int64) t.raw.size(), (juce::int64) (3.0 * fs + 256));
            expect (stoppedAt > 0 && stoppedAt < (int64_t) (4800 + 3.0 * fs + 256 + 2 * 256 + 4800), juce::String (stoppedAt));
            expect (! withTake->getDiRecorder().isRecording());
            const auto firstTakeSamples = (int) t.raw.size();

            // Stop 3.5 s into a 5 s section: the take keeps the 3.5 s that line up (from the shift on).
            session.setRange (0.0, 5.0);
            started = false;
            bool stopped = false;
            runDevice (*withTake, (int) (4.5 * fs), [] (int64_t, const std::vector<float>&) { return 0.1f; }, [&] (int64_t at) {
                if (! started)
                {
                    expect (session.startPlayAlong());
                    started = true;
                }
                if (! stopped && at >= (int64_t) (3.5 * fs + 256))
                {
                    session.stopRecording();
                    stopped = true;
                }
                session.poll();
            });
            const auto& early = session.getTake();
            expect (early.valid && ! early.complete, "valid " + juce::String ((int) early.valid) + " complete " + juce::String ((int) early.complete) + " raw "
                                                         + juce::String ((int) early.raw.size()) + " error " + session.getError());
            expectWithinAbsoluteError ((double) session.getReferenceSeconds(), 3.5, 0.02);
            logMessage ("  -> a 3 s section with the device reporting 100 + 156 samples: the take stopped by itself (the page's timer saw it "
                        + juce::String ((double) (stoppedAt - 4800) / fs, 3) + " s after Record), " + juce::String (firstTakeSamples)
                        + " samples recorded = the section + 256; 5 s of the riff with the gate on through the processor taking it, with the song silent: identical, bit for bit, "
                        "to the same processor without (" + juce::String ((int) a.size()) + " samples); Stop 3.5 s into a 5 s section kept "
                        + juce::String (session.getReferenceSeconds(), 3) + " s");
        }

        beginTest ("the tempo suggestion: the C++ estimate against its prototype on the fixtures, and the session offers it once the range is still");
        {
            const juce::var e = juce::JSON::parse (fixtures().getChildFile ("expected_tempo.json").loadFileAsString());
            juce::StringArray lines;
            const std::atomic<bool> never { false };
            for (const auto* name : { "target_anything.wav", "target_same.wav", "target_di_playalong.wav", "reference_di.wav", "separation_mix.wav" })
            {
                const auto b = readWav (fixtures().getChildFile (name));
                std::vector<float> x ((size_t) b.getNumSamples());
                for (int ch = 0; ch < b.getNumChannels(); ++ch)
                    for (int n = 0; n < b.getNumSamples(); ++n)
                        x[(size_t) n] += b.getSample (ch, n) / (float) b.getNumChannels();
                const auto t = ampsim::tonematch::estimateTempo (x.data(), (int) x.size(), fs, never);
                const auto& g = e[name];
                expectWithinAbsoluteError (t.bpm, (double) g["bpm"], 0.05);
                expectWithinAbsoluteError (t.confidence, (double) g["confidence"], 0.005);
                expect (t.confident == (bool) g["confident"]);
                lines.add (juce::String (name) + " " + juce::String (t.bpm, 2) + " BPM (prototype " + juce::String ((double) g["bpm"], 2) + "), confidence "
                           + juce::String (t.confidence, 3));
            }
            const auto noise = whiteNoise ((int) (10.0 * fs), 0.1f, 1);
            const auto n = ampsim::tonematch::estimateTempo (noise.data(), (int) noise.size(), fs, never);
            expect (! n.confident);

            auto p = makeProcessor();
            ToneMatchSession session (*p);
            const auto x = readWav (fixtures().getChildFile ("target_anything.wav"));
            session.setTargetSignal (std::vector<float> (x.getReadPointer (0), x.getReadPointer (0) + x.getNumSamples()), "target");
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            expect (session.waitForTempo (5000));
            const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
            expectWithinAbsoluteError (session.getSuggestedBpm(), 120.0, 0.5);
            expectWithinAbsoluteError (session.getCountInBpm(), 120.0, 1.0e-6); // the app's tempo until one is set here
            session.setCountInBpm (97.0);
            expectWithinAbsoluteError (session.getCountInBpm(), 97.0, 1.0e-9);
            for (const auto& l : lines)
                logMessage ("  -> " + l);
            logMessage ("  -> white noise: " + juce::String (n.bpm, 1) + " BPM at confidence " + juce::String (n.confidence, 3) + ", no suggestion; the session suggested "
                        + juce::String (session.getSuggestedBpm(), 2) + " BPM for target_anything.wav " + juce::String (ms, 0)
                        + " ms after the target was set (250 ms of that waiting for the range to be still)");
        }
    }
};

static ToneMatchPlayAlongTests toneMatchPlayAlongTests;
