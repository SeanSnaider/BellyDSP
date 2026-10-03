// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AllocationTracking.h"
#include "TestHelpers.h"
#include "dsp/Harmonizer.h"
#include "dsp/PitchDetector.h"

#include <algorithm>
#include <chrono>
#include <numeric>

namespace
{
using namespace testing;
using ampsim::Harmonizer;
namespace hy = ampsim::harmony;

double hz (double midi) { return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0); }

double percentile (std::vector<double> v, double p)
{
    std::sort (v.begin(), v.end());
    return v.empty() ? 0.0 : v[(size_t) (p * (double) (v.size() - 1))];
}
double midiOf (double f) { return 69.0 + 12.0 * std::log2 (f / 440.0); }

/// A guitar-like tone following a pitch track (fractional MIDI notes; several at once make a chord), phase
/// continuous, so a change of note without a new attack is a hammer-on. `plucks` restart the envelope.
std::vector<float> play (const std::function<std::vector<double> (double)>& track, double seconds, const std::vector<double>& plucks)
{
    std::vector<float> out ((size_t) (seconds * fs));
    std::array<double, 4> phase {};
    size_t nextPluck = 0;
    double pluckAt = -1.0e9;
    for (size_t i = 0; i < out.size(); ++i)
    {
        const auto t = (double) i / fs;
        while (nextPluck < plucks.size() && t >= plucks[nextPluck])
            pluckAt = plucks[nextPluck++];
        const auto age = t - pluckAt;
        const auto envelope = age < 0.0 ? 0.0 : (1.0 - std::exp (-age / 0.002)) * std::exp (-age / 1.5);
        const auto notes = track (t);
        double v = 0.0;
        for (size_t k = 0; k < notes.size() && k < phase.size(); ++k)
        {
            phase[k] += hz (notes[k]) / fs;
            phase[k] -= std::floor (phase[k]);
            for (int n = 1; n <= 6; ++n)
                v += std::sin (juce::MathConstants<double>::twoPi * n * phase[k]) / n;
        }
        out[i] = (float) (0.15 * envelope * v);
    }
    return out;
}

struct Render
{
    std::vector<float> wet;                                    // output minus the dry (left)
    std::vector<std::pair<double, int>> changes;               // tracker changes: time, note
    std::vector<std::array<double, Harmonizer::maxVoices>> fades; // per hop
    double maxDryError = 0.0;
};

Render run (Harmonizer::Settings settings, const std::vector<float>& input)
{
    Harmonizer h;
    h.setSettings (settings);
    h.prepare (fs, blockSize);
    Render r;
    r.wet.resize (input.size());
    std::vector<float> left ((size_t) blockSize), right ((size_t) blockSize);
    auto changes = h.getTracker().getChangeCount();
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        std::copy (input.begin() + (long) start, input.begin() + (long) start + blockSize, left.begin());
        std::copy (left.begin(), left.end(), right.begin());
        float* channels[] = { left.data(), right.data() };
        h.setSettings (settings);
        h.process (juce::dsp::AudioBlock<float> (channels, 2, (size_t) blockSize), { input.data() + start, blockSize });
        for (int n = 0; n < blockSize; ++n)
            r.wet[start + (size_t) n] = left[(size_t) n] - input[start + (size_t) n];
        if (h.getTracker().getChangeCount() != changes)
        {
            changes = h.getTracker().getChangeCount();
            r.changes.emplace_back ((double) (start + blockSize) / fs, h.getTracker().getNote());
        }
        std::array<double, Harmonizer::maxVoices> f {};
        for (int v = 0; v < Harmonizer::maxVoices; ++v)
            f[(size_t) v] = h.getVoiceFade (v);
        r.fades.push_back (f);
    }
    return r;
}

/// The pitch (fractional MIDI) of x over the frame ending at `end`, by the shared McLeod detector (tuner
/// preset: 28 Hz to 1.4 kHz, 72 ms frame); 0 when there's no confident reading.
double pitchAt (const std::vector<float>& x, size_t end, double clarityNeeded = 0.9)
{
    ampsim::PitchDetector d;
    d.prepare (fs, ampsim::PitchDetector::Settings::tuner());
    const auto start = end > (size_t) (0.2 * fs) ? end - (size_t) (0.2 * fs) : 0;
    d.push (x.data() + start, (int) (end - start));
    const auto e = d.detect();
    return e.frequency > 0.0 && e.clarity >= clarityNeeded ? midiOf (e.frequency) : 0.0;
}

Harmonizer::Settings oneVoice (hy::Interval interval, hy::Key key = { 0, hy::Scale::major }, hy::OutOfScale rule = hy::OutOfScale::parallel)
{
    auto s = Harmonizer::defaults();
    s.voices[0] = { true, interval, 0.0, 0.0, 0.0 };
    for (int v = 1; v < Harmonizer::maxVoices; ++v)
        s.voices[(size_t) v].on = false;
    s.key = key;
    s.outOfScale = rule;
    return s;
}
} // namespace

class HarmonizerTests final : public juce::UnitTest
{
public:
    HarmonizerTests() : juce::UnitTest ("Harmonizer", "ampsim") {}

    void runTest() override
    {
        melody();
        bends();
        legatoAndSlides();
        onsets();
        chords();
        scaleRules();
        dryAndClicks();
        realtime();
    }

private:
    // ------------------------------------------------------------------------------------------
    void melody()
    {
        beginTest ("a C major scale, a diatonic third above: each note gets the key's third (3 or 4 semitones), within a few cents");

        const std::vector<double> notes { 60, 62, 64, 65, 67, 69, 71, 72 };
        const std::vector<double> expected { 64, 65, 67, 69, 71, 72, 74, 76 };
        std::vector<double> plucks;
        for (size_t i = 0; i < notes.size(); ++i)
            plucks.push_back (0.1 + 0.4 * (double) i);
        const auto input = play ([&] (double t) -> std::vector<double>
        {
            if (t < 0.1)
                return {};
            return { notes[std::min (notes.size() - 1, (size_t) ((t - 0.1) / 0.4))] };
        }, 0.1 + 0.4 * (double) notes.size(), plucks);

        const auto r = run (oneVoice ({ true, 2, 0 }), input);
        juce::StringArray results;
        double worst = 0.0;
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto measured = pitchAt (r.wet, (size_t) ((0.1 + 0.4 * (double) i + 0.35) * fs));
            const auto error = (measured - expected[i]) * 100.0;
            worst = std::max (worst, std::abs (error));
            results.add (juce::MidiMessage::getMidiNoteName ((int) notes[i], true, true, 4) + " -> " + juce::MidiMessage::getMidiNoteName ((int) std::lround (measured), true, true, 4)
                         + " (" + (error >= 0 ? "+" : "") + juce::String (error, 2) + " ct)");
        }
        expectLessThan (worst, 3.0);
        logMessage ("  -> " + results.joinIntoString (", ") + "; worst " + juce::String (worst, 2) + " cents");
    }

    // ------------------------------------------------------------------------------------------
    void bends()
    {
        beginTest ("bends and vibrato: the harmony moves in parallel (the harmony-to-dry interval stays put)");

        // E4 held, bent up 70 cents and back over 0.6 s, then 30-cent vibrato at 5.5 Hz.
        const auto input = play ([] (double t) -> std::vector<double>
        {
            if (t < 0.05)
                return {};
            if (t < 0.45)
                return { 64.0 };
            if (t < 1.05)
                return { 64.0 + 0.7 * std::sin (juce::MathConstants<double>::pi * (t - 0.45) / 0.6) };
            return { 64.0 + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * (t - 1.05)) };
        }, 2.0, { 0.05 });
        const auto r = run (oneVoice ({ true, 2, 0 }), input);

        // The interval between dry and harmony, measured on short frames (harmonizer preset) every 10 ms.
        ampsim::PitchDetector dry, wet;
        dry.prepare (fs, ampsim::PitchDetector::Settings::harmonizer());
        wet.prepare (fs, ampsim::PitchDetector::Settings::harmonizer());
        double lowest = 1000.0, highest = -1000.0;
        int frames = 0;
        for (size_t start = 0; start + 480 <= input.size(); start += 480)
        {
            dry.push (input.data() + start, 480);
            wet.push (r.wet.data() + start, 480);
            const auto t = (double) (start + 480) / fs;
            if (t < 0.5)
                continue;
            const auto a = dry.detect(), b = wet.detect();
            if (a.clarity < 0.95 || b.clarity < 0.95)
                continue;
            // The harmony trails by a few ms: compare against the dry 1 frame (10 ms) earlier would be fairer
            // on fast bends; at these rates (at most 3.7 cents/ms) the trail is worth about 1 cent per ms.
            const auto interval = (midiOf (b.frequency) - midiOf (a.frequency)) * 100.0;
            lowest = std::min (lowest, interval);
            highest = std::max (highest, interval);
            ++frames;
        }
        expectEquals ((int) r.changes.size(), 1); // the onset, nothing else
        expectGreaterThan (lowest, 300.0 - 40.0);
        expectLessThan (highest, 300.0 + 40.0);
        logMessage ("  -> E4 bent 70 cents and back, then vibrato +-30 cents: " + juce::String ((int) r.changes.size() - 1) + " re-evaluations after the onset; "
                    "harmony minus dry over " + juce::String (frames) + " frames from " + juce::String (lowest, 1) + " to " + juce::String (highest, 1)
                    + " cents (a third, 300, held; the spread is the harmony trailing the moving dry by a few ms)");
    }

    // ------------------------------------------------------------------------------------------
    void legatoAndSlides()
    {
        beginTest ("legato and slides: a hammer-on re-evaluates within about 10 ms; a slide only once it lands");

        // C major. E4 (third +3, to G4), hammer-on to F4 at 0.5 s (third +4, to A4), slide to A4 from 1.0 to
        // 1.1 s (third +3, to C5), held.
        const auto input = play ([] (double t) -> std::vector<double>
        {
            if (t < 0.05)
                return {};
            if (t < 0.5)
                return { 64.0 };
            if (t < 1.0)
                return { 65.0 };
            if (t < 1.1)
                return { 65.0 + 4.0 * (t - 1.0) / 0.1 };
            return { 69.0 };
        }, 1.6, { 0.05 });
        const auto r = run (oneVoice ({ true, 2, 0 }), input);

        juce::StringArray seen;
        for (const auto& [t, note] : r.changes)
            seen.add (juce::MidiMessage::getMidiNoteName (note, true, true, 4) + " at " + juce::String (t * 1000.0, 1) + " ms");
        const auto changeTo = [&r] (int note)
        {
            for (const auto& [t, n] : r.changes)
                if (n == note)
                    return t;
            return -1.0;
        };
        const auto hammer = changeTo (65) - 0.5, landed = changeTo (69) - 1.1;
        expectEquals ((int) r.changes.size(), 3, seen.joinIntoString (", "));
        expect (hammer > 0.0 && hammer < 0.0125, seen.joinIntoString (", "));
        expect (landed > 0.0 && landed < 0.06, seen.joinIntoString (", "));

        const auto afterHammer = pitchAt (r.wet, (size_t) (0.95 * fs));
        const auto afterSlide = pitchAt (r.wet, (size_t) (1.55 * fs));
        expectWithinAbsoluteError (afterHammer, 69.0, 0.05);
        expectWithinAbsoluteError (afterSlide, 72.0, 0.05);
        logMessage ("  -> notes: " + seen.joinIntoString (", ") + "; the hammer-on re-evaluated " + juce::String (hammer * 1000.0, 1) + " ms after it, the slide "
                    + juce::String (landed * 1000.0, 1) + " ms after it landed (no notes in between); harmonies measured "
                    + juce::MidiMessage::getMidiNoteName ((int) std::lround (afterHammer), true, true, 4) + " " + juce::String ((afterHammer - 69.0) * 100.0, 2)
                    + " ct and " + juce::MidiMessage::getMidiNoteName ((int) std::lround (afterSlide), true, true, 4) + " " + juce::String ((afterSlide - 72.0) * 100.0, 2) + " ct");
    }

    // ------------------------------------------------------------------------------------------
    void onsets()
    {
        beginTest ("onset timing per string: from the pluck to the harmony fading in (plan: high E 6 to 8 ms, G 10 to 12, A about 20, low E 25 to 30 with the floor lowered)");

        struct String
        {
            const char* name;
            double midi;
            int floor;
            double budgetMs;
        };
        juce::StringArray results;
        for (const auto& s : { String { "high E", 64.0, 0, 8.0 }, String { "B", 59.0, 0, 10.0 }, String { "G", 55.0, 0, 12.0 }, String { "D", 50.0, 0, 16.0 },
                               String { "A", 45.0, 0, 20.0 }, String { "low E", 40.0, 1, 30.0 } })
        {
            const auto input = play ([&s] (double t) -> std::vector<double> { return t < 0.1 ? std::vector<double>() : std::vector<double> { s.midi }; }, 0.4, { 0.1 });
            auto settings = oneVoice ({ true, 2, 0 }, { 4, hy::Scale::naturalMinor });
            settings.floor = s.floor;
            const auto r = run (settings, input);
            double onsetMs = -1.0;
            for (size_t i = 0; i < r.fades.size(); ++i)
                if (r.fades[i][0] >= 0.5)
                {
                    onsetMs = ((double) (i + 1) * blockSize / fs - 0.1) * 1000.0;
                    break;
                }
            results.add (juce::String (s.name) + " " + juce::String (onsetMs, 1) + " ms (budget " + juce::String (s.budgetMs, 0) + ")");
            // A buffer of 128 samples is 2.7 ms: the measurement's own resolution.
            expect (onsetMs > 0.0 && onsetMs <= s.budgetMs + 2.0 * 2.67 + 6.0, results.joinIntoString (", "));
        }
        logMessage ("  -> half-way faded in, measured to the end of its 128-sample buffer: " + results.joinIntoString (", "));
    }

    // ------------------------------------------------------------------------------------------
    void chords()
    {
        beginTest ("chords: the voices fade out instead of producing garbage, and come back with the next single note");

        const auto input = play ([] (double t) -> std::vector<double>
        {
            if (t < 0.05)
                return {};
            if (t < 0.5)
                return { 64.0 };
            if (t < 1.0)
                return { 57.0, 61.0, 64.0, 69.0 }; // A major
            return { 67.0 };
        }, 1.5, { 0.05, 0.5, 1.0 });
        const auto r = run (oneVoice ({ true, 2, 0 }, { 9, hy::Scale::major }), input);
        const auto rmsDb = [&r] (double from, double to)
        { return toDb (rms (r.wet.data() + (size_t) (from * fs), (size_t) ((to - from) * fs)) + 1.0e-12); };
        const auto single = rmsDb (0.3, 0.45), chord = rmsDb (0.55, 0.95), back = rmsDb (1.2, 1.45);
        expectLessThan (chord, single - 40.0);
        expectGreaterThan (back, single - 6.0);
        logMessage ("  -> harmony level: " + juce::String (single, 1) + " dBFS on E4, " + juce::String (chord, 1) + " dBFS through an A major chord, "
                    + juce::String (back, 1) + " dBFS on the G4 after it");
    }

    // ------------------------------------------------------------------------------------------
    void scaleRules()
    {
        beginTest ("through the block: an out-of-key note follows Parallel or Snap, and a chromatic voice ignores the key");

        const auto input = play ([] (double t) -> std::vector<double> { return t < 0.05 ? std::vector<double>() : std::vector<double> { 61.0 }; }, 0.6, { 0.05 });
        const auto parallel = pitchAt (run (oneVoice ({ true, 2, 0 }, { 0, hy::Scale::major }, hy::OutOfScale::parallel), input).wet, (size_t) (0.5 * fs));
        const auto snap = pitchAt (run (oneVoice ({ true, 2, 0 }, { 0, hy::Scale::major }, hy::OutOfScale::snap), input).wet, (size_t) (0.5 * fs));
        const auto chromatic = pitchAt (run (oneVoice ({ false, -5, 0 }, { 0, hy::Scale::major }), input).wet, (size_t) (0.5 * fs));
        expectWithinAbsoluteError (parallel, 65.0, 0.05);
        expectWithinAbsoluteError (snap, 64.0, 0.05);
        expectWithinAbsoluteError (chromatic, 56.0, 0.05);
        logMessage ("  -> C#4 in C major, a third above: Parallel gives " + juce::MidiMessage::getMidiNoteName ((int) std::lround (parallel), true, true, 4) + " ("
                    + juce::String ((parallel - 65.0) * 100.0, 2) + " ct), Snap " + juce::MidiMessage::getMidiNoteName ((int) std::lround (snap), true, true, 4) + " ("
                    + juce::String ((snap - 64.0) * 100.0, 2) + " ct); chromatic -5 gives " + juce::MidiMessage::getMidiNoteName ((int) std::lround (chromatic), true, true, 4)
                    + " (" + juce::String ((chromatic - 56.0) * 100.0, 2) + " ct)");
    }

    // ------------------------------------------------------------------------------------------
    void dryAndClicks()
    {
        beginTest ("the dry passes untouched and undelayed; voices start and stop without clicks");

        const auto input = guitarDI ((int) (2.0 * fs));
        auto off = oneVoice ({ true, 2, 0 });
        off.voices[0].on = false;
        const auto silent = run (off, input);
        double worst = 0.0;
        for (const auto v : silent.wet)
            worst = std::max (worst, (double) std::abs (v));
        expectEquals (worst, 0.0);

        // Notes with gaps (each with a 15 ms release, so any step at a transition is the harmonizer's own):
        // the harmony fades in and out at each one.
        auto notes = play ([] (double t) -> std::vector<double> { return { 60.0 + ((int) (t / 0.3) % 5) * 2.0 }; }, 2.4,
                           { 0.0, 0.3, 0.6, 0.9, 1.2, 1.5, 1.8, 2.1 });
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto inNote = std::fmod ((double) i / fs, 0.3);
            notes[i] *= (float) (inNote < 0.2 ? 1.0 : inNote < 0.215 ? 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * (inNote - 0.2) / 0.015) : 0.0);
        }
        const auto r = run (oneVoice ({ true, 2, 0 }), notes);
        double steady = 0.0, transitions = 0.0;
        for (int k = 0; k < 8; ++k)
        {
            const auto t0 = 0.3 * k;
            steady = std::max (steady, maxStep (r.wet, (size_t) ((t0 + 0.08) * fs), (size_t) ((t0 + 0.18) * fs)));
            transitions = std::max (transitions, maxStep (r.wet, (size_t) (t0 * fs), (size_t) ((t0 + 0.04) * fs)));
            transitions = std::max (transitions, maxStep (r.wet, (size_t) ((t0 + 0.19) * fs), (size_t) ((t0 + 0.26) * fs)));
        }
        expectLessThan (transitions, steady * 1.25);
        logMessage ("  -> every voice off: the output is the input to the bit (largest difference " + juce::String (worst) + "); notes with gaps: largest "
                    "step as the harmony fades in or out " + juce::String (transitions, 4) + " vs " + juce::String (steady, 4) + " while it plays");
    }

    // ------------------------------------------------------------------------------------------
    void realtime()
    {
        beginTest ("real time: four voices, key changes, rules, floors, and glides allocate and lock nothing; CPU");

        auto settings = Harmonizer::defaults();
        for (auto& v : settings.voices)
            v.on = true;
        Harmonizer h;
        h.setSettings (settings);
        h.prepare (fs, blockSize);
        const auto input = play ([] (double t) -> std::vector<double>
        {
            const auto k = (int) (t / 0.25);
            return k % 7 == 6 ? std::vector<double> { 57.0, 61.0, 64.0 } : std::vector<double> { 55.0 + (k * 3) % 12 };
        }, 6.0, { 0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 5.5 });
        std::vector<float> left ((size_t) blockSize), right ((size_t) blockSize);
        std::vector<double> micros;
        rtcheck::Counts total;
        int block = 0;
        for (size_t start = 0; start + blockSize <= input.size(); start += blockSize, ++block)
        {
            if (block % 150 == 0)
            {
                settings.key.root = (settings.key.root + 5) % 12;
                settings.key.scale = (hy::Scale) ((int) settings.key.scale % 11 + 1);
                settings.outOfScale = settings.outOfScale == hy::OutOfScale::parallel ? hy::OutOfScale::snap : hy::OutOfScale::parallel;
                settings.floor = (settings.floor + 1) % 3;
                settings.voices[1].interval.diatonic = ! settings.voices[1].interval.diatonic;
                settings.glideMs = 30.0 - settings.glideMs;
            }
            std::copy (input.begin() + (long) start, input.begin() + (long) start + blockSize, left.begin());
            std::copy (left.begin(), left.end(), right.begin());
            float* channels[] = { left.data(), right.data() };
            rtcheck::begin();
            const auto t0 = std::chrono::steady_clock::now();
            h.setSettings (settings);
            h.process (juce::dsp::AudioBlock<float> (channels, 2, (size_t) blockSize), { input.data() + start, blockSize });
            const auto elapsed = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();
            const auto c = rtcheck::end();
            micros.push_back (elapsed); // outside the measurement: the vector grows
            total.allocations += c.allocations;
            total.frees += c.frees;
            total.blockingLocks += c.blockingLocks;
        }
        expectEquals (total.allocations, 0L);
        expectEquals (total.frees, 0L);
        expectEquals (total.blockingLocks, 0L);
        const auto mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
        logMessage ("  -> " + juce::String (block) + " buffers, four voices, the key, scale, rule, floor, and glide changed every 150: "
                    + juce::String (total.allocations) + " allocations, " + juce::String (total.frees) + " frees, " + juce::String (total.blockingLocks)
                    + " locks; mean " + juce::String (mean, 1) + " us per 128-sample buffer (" + juce::String (100.0 * mean / 2666.7, 2) + "% of the deadline), p99 "
                    + juce::String (percentile (micros, 0.99), 1) + " us");
    }
};

static HarmonizerTests harmonizerTests;
