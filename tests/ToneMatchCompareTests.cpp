// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// Tone match's A/B comparison (docs/TONE_MATCH.md, "Comparing"): the preview player at the end of the chain
// (positions kept across switches, the equal-power crossfades, the loop's wrap, the live guitar's mute, the
// live path bit-identical while not previewing), the renders against what Apply produces, the level match,
// cancel, the alignment in same-part mode, and the page with its A/B controls, snapshotted at 2x.

#include "BuiltInCaptures.h"
#include "PluginEditor.h"
#include "TestHelpers.h"
#include "dsp/Loudness.h"
#include "dsp/PreviewPlayer.h"

#include <cmath>

namespace
{
using namespace testing;
using Player = ampsim::PreviewPlayer;

juce::File fixtures() { return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/tone_match"); }

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

std::shared_ptr<const std::vector<float>> shared (std::vector<float> x) { return std::make_shared<const std::vector<float>> (std::move (x)); }

/// A ramp that encodes its own position: value = scale * (n + 1), so a sample says where it was read.
std::vector<float> ramp (int length, double scale)
{
    std::vector<float> x ((size_t) length);
    for (size_t n = 0; n < x.size(); ++n)
        x[n] = (float) (scale * (double) (n + 1));
    return x;
}

/// Runs the player alone over `numSamples` of `live` input (or silence), in blocks; returns the left channel.
std::vector<float> run (Player& player, int numSamples, const std::vector<float>* live = nullptr, int offset = 0)
{
    std::vector<float> out ((size_t) numSamples);
    juce::AudioBuffer<float> buffer (2, blockSize);
    for (int start = 0; start < numSamples; start += blockSize)
    {
        const auto len = std::min (blockSize, numSamples - start);
        buffer.clear();
        if (live != nullptr)
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < len; ++i)
                    buffer.setSample (ch, i, (*live)[(size_t) (offset + start + i) % live->size()]);
        auto block = juce::dsp::AudioBlock<float> (buffer).getSubBlock (0, (size_t) len);
        player.process (block, {});
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + len, out.begin() + start);
    }
    return out;
}

double stepMax (const std::vector<float>& x, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = std::max<size_t> (from, 1); i < std::min (to, x.size()); ++i)
        m = std::max (m, (double) std::abs (x[i] - x[i - 1]));
    return m;
}

/// The left channel of the processor's output for `input` (silence if empty), `numSamples` long.
std::vector<float> playProcessor (AmpSimProcessor& p, const std::vector<float>& input, int numSamples)
{
    std::vector<float> out ((size_t) numSamples);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (int start = 0; start + blockSize <= numSamples; start += blockSize)
    {
        buffer.clear();
        if (! input.empty())
            buffer.copyFrom (0, 0, input.data() + ((size_t) start % input.size()), std::min (blockSize, (int) (input.size() - (size_t) start % input.size())));
        p.processBlock (buffer, midi);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize, out.begin() + start);
    }
    return out;
}

double lufs (const std::vector<float>& x, size_t from, size_t to)
{
    return ampsim::loudness::integratedMono (x.data() + from, (int) (to - from), fs);
}
} // namespace

class ToneMatchCompareTests final : public juce::UnitTest
{
public:
    ToneMatchCompareTests() : juce::UnitTest ("Tone match compare", "ampsim") {}

    void runTest() override
    {
        const auto shots = proofDir().getChildFile ("tone_match");
        shots.createDirectory();

        beginTest ("player: a switch keeps the position (the DI's clock exactly; the target's through the loop offset, or the alignment), a stop and a start");
        {
            Player player;
            player.prepare (fs, blockSize);
            auto m = std::make_unique<Player::Material>();
            m->audio = { shared (ramp (480000, 1.0e-6)), shared (ramp (480000, 2.0e-6)), shared (ramp (480000, 3.0e-6)) };
            player.setMaterial (std::move (m));
            player.setLoop (0, 48000, 48000 * 5); // the target's loop: 1 s to 5 s
            player.setLoop (1, 0, 48000 * 8);     // the DI's: all 8 s
            player.setMuteLive (true);
            player.setSource (1);
            player.setPlaying (true);
            const auto F = (int) std::lround (Player::crossfadeSeconds * fs);

            // Match from the DI's loop start: after the fade-in, sample n of the output is position n.
            auto a = run (player, 24000);
            const auto positionAt = [] (float value, double scale) { return (double) value / scale - 1.0; };
            const auto before = positionAt (a.back(), 2.0e-6);
            expectWithinAbsoluteError (before, 23999.0, 0.05);

            // To Current (the same clock): it continues at exactly the next sample.
            player.setSource (2);
            auto b = run (player, 4096);
            const auto after = positionAt (b.back(), 3.0e-6);
            expectWithinAbsoluteError (after, 24000.0 + 4095.0, 0.1);

            // To the target (another clock, unaligned): the offset into its own loop is kept.
            player.setSource (0);
            auto c = run (player, 4096);
            const auto expectedTarget = 48000.0 + (24000.0 + 4096.0 + 4095.0);
            const auto onTarget = positionAt (c.back(), 1.0e-6);
            expectWithinAbsoluteError (onTarget, expectedTarget, 0.1);

            // Stop: silent after the fade; idle (no voices, live back at exactly 1).
            player.setPlaying (false);
            auto d = run (player, 4096);
            expectEquals (d.back(), 0.0f);
            expect (! player.isActive());

            // Aligned (same part): the target's position goes through the table to the DI's.
            Player aligned;
            aligned.prepare (fs, blockSize);
            auto am = std::make_unique<Player::Material>();
            am->audio = { shared (ramp (480000, 1.0e-6)), shared (ramp (480000, 2.0e-6)), nullptr };
            am->aligned = true;
            am->hop = 2048.0;
            am->offset = 4096.0;
            for (int i = 0; i < 200; ++i)
            {
                am->diAtTarget.push_back (4096.0 + 2048.0 * 0.8 * i); // the DI is played 25% faster
                am->targetAtDi.push_back (4096.0 + 2048.0 * i / 0.8);
            }
            const auto mapped = Player::alignedPosition (*am, 0, 4096.0 + 2048.0 * 50.0);
            aligned.setMaterial (std::move (am));
            aligned.setLoop (0, 0, 48000 * 8);
            aligned.setLoop (1, 0, 48000 * 8);
            aligned.setSource (0);
            aligned.setPlaying (true);
            run (aligned, 4096 + 2048 * 50 - 1);
            aligned.setSource (1);
            auto e = run (aligned, 2 * F);
            const auto onDi = positionAt (e.back(), 2.0e-6);
            expectWithinAbsoluteError (mapped, 4096.0 + 2048.0 * 40.0, 1.0e-6);
            expectWithinAbsoluteError (onDi, mapped + 2.0 * F - 1.0, 1.5);
            logMessage ("  -> Match at position " + juce::String (before, 1) + " -> Current continued at " + juce::String (after - 4095.0, 1)
                        + " (the next sample) -> the target at " + juce::String (onTarget - 4095.0, 1) + " (its loop start 48000 + the offset "
                        + juce::String (24000 + 4096) + "); aligned 25% faster DI: target sample " + juce::String (4096 + 2048 * 50) + " -> DI sample "
                        + juce::String (onDi - 2.0 * F + 1.0, 1) + " (the table says " + juce::String (mapped, 1) + "); stopped: silent and idle");
        }

        beginTest ("player: the source switch is an equal-power crossfade with no step; the loop wraps with one; the live guitar mutes and comes back");
        {
            // Two different sines at -6 dBFS, 220 and 331 Hz, so a hard switch or a hard wrap would jump.
            const auto A = 0.5;
            Player player;
            player.prepare (fs, blockSize);
            auto m = std::make_unique<Player::Material>();
            m->audio = { shared (sine (220.0, A, 480000)), shared (sine (331.0, A, 480000)), shared (sine (331.0, A, 480000)) };
            player.setMaterial (std::move (m));
            const auto loopEnd = 48000 + 17 * 101; // 1.036 s: not a whole number of periods, so the wrap is a jump in the signal
            player.setLoop (0, 0, loopEnd);
            player.setLoop (1, 0, 48000 * 8);
            player.setSource (0);
            player.setPlaying (true);
            auto head = run (player, 24000);
            player.setSource (1);
            auto sw = run (player, 4800);
            player.setSource (0);
            auto back = run (player, 30000); // through the target's loop end at least once
            std::vector<float> all (head);
            all.insert (all.end(), sw.begin(), sw.end());
            all.insert (all.end(), back.begin(), back.end());

            // The natural steps: 2 pi f A / fs for each sine; the largest a sum can make is their sum.
            const auto natural = juce::MathConstants<double>::twoPi * 331.0 * A / fs;
            const auto bound = juce::MathConstants<double>::twoPi * (220.0 + 331.0) * A / fs;
            const auto switchStep = stepMax (all, 24000, 24000 + 2000);
            const auto backStep = stepMax (all, 28800, 28800 + 2000);
            const auto wrapStep = stepMax (all, 28800 + 2000, all.size());
            const auto target = sine (220.0, A, 480000);
            const auto hardWrap = std::abs (target[0] - target[(size_t) loopEnd - 1]);
            const auto hardSwitch = std::abs ((double) sine (331.0, A, 24001)[24000] - (double) target[23999]);
            expectLessThan (switchStep, bound * 1.02);
            expectLessThan (backStep, bound * 1.02);
            expectLessThan (wrapStep, bound * 1.02);

            // Equal power: the RMS over the 20 ms crossfade within 1 dB of either sine's -9 dBFS.
            double energy = 0.0;
            for (size_t i = 24000; i < 24000 + 960; ++i)
                energy += (double) all[i] * all[i];
            const auto worstDb = std::abs (10.0 * std::log10 (energy / 960.0) - 20.0 * std::log10 (A / std::sqrt (2.0)));
            expectLessThan (worstDb, 1.0);

            // The live guitar: muted while previewing (after its 20 ms fade), back at exactly unity once stopped.
            Player live;
            live.prepare (fs, blockSize);
            auto lm = std::make_unique<Player::Material>();
            lm->audio = { shared (std::vector<float> (96000, 0.0f)), nullptr, nullptr };
            live.setMaterial (std::move (lm));
            live.setLoop (0, 0, 96000);
            const auto guitar = sine (110.0, 0.3, 96000);
            auto idle = run (live, 4800, &guitar);
            live.setMuteLive (true);
            live.setPlaying (true);
            auto muted = run (live, 4800, &guitar, 4800);
            live.setPlaying (false);
            auto back2 = run (live, 9600, &guitar, 9600);
            double mutedPeak = 0.0;
            for (size_t i = 1200; i < muted.size(); ++i)
                mutedPeak = std::max (mutedPeak, (double) std::abs (muted[i]));
            const auto idleExact = std::equal (idle.begin(), idle.end(), guitar.begin());
            const auto backExact = std::equal (back2.begin() + 2400, back2.end(), guitar.begin() + 9600 + 2400);
            expect (idleExact && backExact);
            expectEquals (mutedPeak, 0.0);
            live.setMuteLive (false);
            live.setPlaying (true);
            auto heard = run (live, 4800, &guitar, 19200);
            const auto heardExact = std::equal (heard.begin(), heard.end(), guitar.begin() + 19200);
            expect (heardExact, "with the mute off, the live guitar passes untouched under a silent preview");

            logMessage ("  -> largest sample-to-sample step: switching Target -> Match " + juce::String (switchStep, 5) + ", back " + juce::String (backStep, 5)
                        + ", through the loop's wrap " + juce::String (wrapStep, 5) + " (the 331 Hz sine's own " + juce::String (natural, 5)
                        + ", the bound for a sum of both " + juce::String (bound, 5) + "; a hard switch would jump " + juce::String (hardSwitch, 3)
                        + ", a hard wrap " + juce::String (hardWrap, 3) + "); RMS through the switch within " + juce::String (worstDb, 2) + " dB of -9 dBFS");
            logMessage ("  -> the live guitar: untouched while idle (bit for bit), silent 25 ms into a preview (peak " + juce::String (mutedPeak)
                        + "), untouched again from 50 ms after Stop (bit for bit), and untouched under a silent preview with the mute off");
        }

        beginTest ("the live path is bit-identical while not previewing, before and after a preview, and the chain adds no latency");
        {
            const auto input = guitarDI ((int) (3.0 * fs));
            AmpSimProcessor plain;
            plain.prepareToPlay (fs, blockSize);
            AmpSimProcessor withPlayer;
            withPlayer.prepareToPlay (fs, blockSize);
            auto m = std::make_unique<Player::Material>();
            m->audio = { shared (sine (440.0, 0.05, 96000)), shared (sine (330.0, 0.05, 96000)), nullptr }; // quiet: the limiter stays transparent
            withPlayer.getPreviewPlayer().setMaterial (std::move (m));
            withPlayer.getPreviewPlayer().setLoop (0, 0, 96000);
            withPlayer.getPreviewPlayer().setLoop (1, 0, 96000);
            setParam (plain, "gate_a_on", 1.0f);
            setParam (withPlayer, "gate_a_on", 1.0f);

            // 1 s with material loaded but not playing, 0.5 s previewing, then 1.5 s after Stop.
            const auto a = playProcessor (plain, input, (int) input.size());
            std::vector<float> b;
            const auto playAt = (size_t) fs / blockSize * blockSize, stopAt = (size_t) (1.5 * fs) / blockSize * blockSize; // on block boundaries
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;
                for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
                {
                    if (start == playAt)
                        withPlayer.getPreviewPlayer().setPlaying (true);
                    if (start == stopAt)
                        withPlayer.getPreviewPlayer().setPlaying (false);
                    buffer.clear();
                    buffer.copyFrom (0, 0, input.data() + start, blockSize);
                    withPlayer.processBlock (buffer, midi);
                    b.insert (b.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
                }
            }
            const auto beforeSame = std::equal (a.begin(), a.begin() + (long) playAt, b.begin());
            const auto afterFrom = stopAt + (size_t) (0.05 * fs);
            const auto afterSame = std::equal (a.begin() + (long) afterFrom, a.begin() + (long) b.size(), b.begin() + (long) afterFrom);
            const auto differedWhilePlaying = ! std::equal (a.begin() + (long) playAt, a.begin() + (long) stopAt, b.begin() + (long) playAt);
            expect (beforeSame, "before the preview: bit for bit");
            size_t firstDiff = b.size();
            for (size_t i = afterFrom; i < b.size(); ++i)
                if (a[i] != b[i])
                {
                    firstDiff = i;
                    break;
                }
            expect (afterSame, "50 ms after Stop: bit for bit; first difference at " + juce::String ((double) firstDiff / fs, 4) + " s: "
                                   + juce::String (a[std::min (firstDiff, a.size() - 1)], 8) + " vs " + juce::String (b[std::min (firstDiff, b.size() - 1)], 8));
            expect (differedWhilePlaying);
            expect (! withPlayer.getPreviewPlayer().isActive());
            expectEquals (withPlayer.getChain().latencySamples(), 0);
            expectEquals (withPlayer.getLatencySamples(), 0);
            logMessage ("  -> 3 s of the riff through two processors, one with A/B material loaded: identical bit for bit for the first second (loaded, not playing) "
                        "and from 50 ms after Stop to the end; different only while previewing. Chain latency 0 samples, reported latency 0");
        }

        beginTest ("the comparison in the page: renders equal what Apply produces (bit for bit), level match within 0.1 LU, playback, keys, cancel; snapshots at 2x");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            setParam (p, "output_limit_on", 0.0f); // the levels as they are, for the loudness measurements
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

            expect (session.setTargetFile (fixtures().getChildFile ("target_anything.wav")));
            expect (session.setReferenceFile (fixtures().getChildFile ("reference_di.wav")));
            page.getModeChoice().setSelected (1, juce::sendNotification); // Anything
            session.setRange (1.0, 8.0);
            setParam (p, "eq_post_mode", 0.0f);
            page.startMatch();
            expect (session.waitForMatch (120000));
            page.refresh();
            expect (session.hasResult(), session.getError());
            expect (session.isRenderingCompare(), "the renders start by themselves");
            pngs.add (snap ("10_compare_rendering"));
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            expect (session.waitForCompare (60000));
            const auto renderMs = juce::Time::getMillisecondCounterHiRes() - t0;
            expect (session.isCompareReady(), session.getCompareStatus());
            const auto& matchRender = session.getCompareAudio (ToneMatchSession::sourceMatch);
            const auto matchCopy = matchRender;
            expectEquals ((int) matchRender.size(), (int) session.getReference().size());

            // Level match: each source's loudness over its loop, times its gain, within 0.1 LU of the others.
            double lo = 1.0e9, hi = -1.0e9;
            juce::StringArray levels;
            for (int s = 0; s < ToneMatchSession::numSources; ++s)
            {
                const auto matched = session.getLoudness (s) + session.getLevelMatchGainDb (s);
                lo = std::min (lo, matched);
                hi = std::max (hi, matched);
                levels.add (ToneMatchSession::sourceName (s) + " " + juce::String (session.getLoudness (s), 2) + " LUFS, gain "
                            + juce::String (session.getLevelMatchGainDb (s), 2) + " dB");
            }
            expectLessThan (hi - lo, 0.1);

            // Through the processor: each source played for one loop, its output's loudness.
            const auto sectionLoop = session.getLoop();
            const auto diLoop = session.getDiLoopSamples();
            juce::StringArray played;
            double playedLo = 1.0e9, playedHi = -1.0e9;
            for (int s = 0; s < ToneMatchSession::numSources; ++s)
            {
                page.selectSource (s);
                page.togglePreview();
                expect (session.isPreviewPlaying());
                const auto loopSamples = s == 0 ? (int) ((sectionLoop.second - sectionLoop.first) * fs) : (int) (diLoop.second - diLoop.first);
                const auto out = playProcessor (p, {}, loopSamples + 4800);
                page.togglePreview();
                playProcessor (p, {}, 4800); // the fade out
                const auto l = lufs (out, 0, (size_t) loopSamples - 960); // one pass, its wrap's crossfade aside
                playedLo = std::min (playedLo, l);
                playedHi = std::max (playedHi, l);
                played.add (ToneMatchSession::sourceName (s) + " " + juce::String (l, 2));
            }
            expectLessThan (playedHi - playedLo, 0.1);
            expect (! p.getPreviewPlayer().isActive());

            // Keys: 2 picks Match, Space plays, 3 picks Current; Space stops.
            expect (page.keyPressed (juce::KeyPress ('2')));
            expectEquals (session.getPreviewSource(), 1);
            expect (page.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)));
            expect (session.isPreviewPlaying());
            playProcessor (p, {}, 48000);
            page.refresh();
            pngs.add (snap ("11_compare_playing_match"));
            expect (page.keyPressed (juce::KeyPress ('3')));
            expectEquals (session.getPreviewSource(), 2);
            playProcessor (p, {}, 9600);
            session.setLoop (2.0, 5.5);
            playProcessor (p, {}, 9600);
            pngs.add (snap ("12_compare_loop_current"));
            expect (page.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)));
            expect (! session.isPreviewPlaying());
            session.setLevelMatch (false);
            const auto offGains = session.getLevelMatchGainDb (0) == 0.0f && session.getLevelMatchGainDb (1) == 0.0f && session.getLevelMatchGainDb (2) == 0.0f;
            expect (offGains);
            session.setLevelMatch (true);
            session.setLoop (0.0, session.getSectionSeconds());

            // Bit for bit: Apply, then the renderer on the settings read back from the processor.
            page.apply();
            waitForLoads (p);
            p.runHousekeeping();
            const std::atomic<bool> never { false };
            const auto afterApply = ampsim::tonematch::ToneMatcher::renderTone (session.currentSettings(), session.getReference(), never);
            const auto bitExact = afterApply.size() == matchCopy.size() && std::equal (afterApply.begin(), afterApply.end(), matchCopy.begin());
            double worst = 0.0;
            for (size_t i = 0; i < std::min (afterApply.size(), matchCopy.size()); ++i)
                worst = std::max (worst, (double) std::abs (afterApply[i] - matchCopy[i]));
            expect (bitExact, "the Match render must equal a render of what Apply set; largest difference " + juce::String (worst));

            // And the page's own Current follows: re-rendered once the settings are still, now equal to Match.
            const auto t1 = juce::Time::getMillisecondCounterHiRes();
            while (! session.isRenderingCompare() && juce::Time::getMillisecondCounterHiRes() - t1 < 3000.0)
            {
                juce::Thread::sleep (20);
                session.poll();
            }
            expect (session.waitForCompare (60000));
            const auto& currentAfter = session.getCompareAudio (ToneMatchSession::sourceCurrent);
            const auto currentFollows = currentAfter.size() == matchCopy.size() && std::equal (currentAfter.begin(), currentAfter.end(), matchCopy.begin());
            expect (currentFollows, "after Apply, the Current render equals the Match render");
            pngs.add (snap ("13_compare_after_apply"));

            // Cancel: a render stopped mid-way says so, and the comparison can render again.
            expect (session.startCompareRender (true));
            juce::Thread::sleep (30);
            const auto c0 = juce::Time::getMillisecondCounterHiRes();
            session.cancelCompare();
            expect (session.waitForCompare (5000));
            const auto cancelMs = juce::Time::getMillisecondCounterHiRes() - c0;
            const auto cancelStatus = session.getCompareStatus();
            expectEquals (cancelStatus, juce::String ("Rendering cancelled"));
            expectLessThan (cancelMs, 3000.0); // bounded by a capture's load, which can't be interrupted (a gain set: five models)
            expect (session.startCompareRender (true));
            expect (session.waitForCompare (60000));
            expect (session.getCompareStatus().isEmpty(), session.getCompareStatus());

            logMessage ("  -> anything mode, the fixtures' 7 s target: Match and Current rendered in " + juce::String (renderMs / 1000.0, 2) + " s on the worker; "
                        + levels.joinIntoString ("; ") + ": level-matched spread " + juce::String (hi - lo, 4) + " LU");
            logMessage ("  -> each played through the processor for one loop (limiter off), output loudness: " + played.joinIntoString (", ")
                        + " LUFS: spread " + juce::String (playedHi - playedLo, 3) + " LU");
            logMessage ("  -> after Apply, renderTone on the settings read back from the processor equals the Match render bit for bit ("
                        + juce::String ((int) matchCopy.size()) + " samples, largest difference " + juce::String (worst)
                        + "); the page's own Current re-render after Apply equals it too: " + (currentFollows ? juce::String ("yes") : juce::String ("no")));
            logMessage ("  -> keys 2, Space, 3, Space: Match, playing, Current, stopped; cancel stopped a render in " + juce::String (cancelMs, 1)
                        + " ms (\"" + cancelStatus + "\"), and it rendered again after");
            logMessage ("  -> snapshots: tone_match/" + pngs.joinIntoString (", tone_match/"));
        }

        beginTest ("same part: the DI's loop and every switch go through the matcher's alignment; snapshot at 2x");
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
            expect (session.setTargetFile (fixtures().getChildFile ("target_same.wav")));
            expect (session.setReferenceFile (fixtures().getChildFile ("reference_di.wav")));
            page.getModeChoice().setSelected (0, juce::sendNotification); // Same part
            session.setRange (0.0, session.getTargetSeconds());
            page.startMatch();
            expect (session.waitForMatch (120000));
            expect (session.hasResult(), session.getError());
            expect (session.waitForCompare (60000));
            expect (session.isCompareReady() && session.isAligned());
            expect (! session.getResult().alignmentPath.empty());

            // The DI's loop follows the target's through the alignment, and stays in order.
            session.setLoop (2.0, 6.0);
            const auto di = session.getDiLoopSamples();
            expect (di.first < di.second);
            expect (di.first > 0 && di.second < (int64_t) session.getReference().size());

            // A switch mid-loop lands where the alignment says.
            page.selectSource (0);
            page.togglePreview();
            playProcessor (p, {}, (int) (1.5 * fs));
            const auto fraction = p.getPreviewPlayer().getPlayheadFraction();
            page.selectSource (1);
            playProcessor (p, {}, 4800);
            const auto fractionAfter = p.getPreviewPlayer().getPlayheadFraction();
            page.refresh();
            const auto file = shots.getChildFile ("14_compare_same_part.png");
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), file));
            page.togglePreview();
            expectWithinAbsoluteError (fractionAfter, fraction + 0.025f, 0.08f);
            logMessage ("  -> same part on the fixtures' target_same.wav: the target's loop 2.0 to 6.0 s maps to the DI's " + juce::String ((double) di.first / fs, 3)
                        + " to " + juce::String ((double) di.second / fs, 3) + " s through the DTW path (" + juce::String ((int) session.getResult().alignmentPath.size())
                        + " pairs); 1.5 s in (" + juce::String (fraction, 3) + " of the loop) a switch to Match continued at "
                        + juce::String (fractionAfter, 3) + " of the DI's loop 0.1 s later; tone_match/14_compare_same_part.png");
        }
    }
};

static ToneMatchCompareTests toneMatchCompareTests;
