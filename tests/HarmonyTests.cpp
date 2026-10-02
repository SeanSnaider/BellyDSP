#include "AllocationTracking.h"
#include "TestHelpers.h"
#include "dsp/NoteTracker.h"
#include "dsp/PitchDetector.h"
#include "dsp/Scales.h"

#include <bit>

namespace
{
using namespace testing;
using namespace ampsim::harmony;
using ampsim::NoteTracker;
using Change = NoteTracker::Change;

constexpr double hop = 64.0 / fs;

double hz (double midi, double a4 = 440.0) { return a4 * std::pow (2.0, (midi - 69.0) / 12.0); }

int floorMod12 (int a) { return ((a % 12) + 12) % 12; }

const char* nameOf (Change c)
{
    switch (c)
    {
        case Change::onset:   return "onset";
        case Change::jump:    return "jump";
        case Change::slide:   return "slide";
        case Change::release: return "release";
        case Change::none:    break;
    }
    return "none";
}

/// A scripted detector: one (frequency, clarity) pair per 64-sample hop.
struct Script
{
    std::vector<std::pair<double, double>> hops;

    static int count (double seconds) { return (int) std::lround (seconds / hop); }
    void silence (double seconds) { hops.insert (hops.end(), (size_t) count (seconds), { 0.0, 0.0 }); }
    void add (double midi, double clarity = 0.98, double a4 = 440.0) { hops.emplace_back (hz (midi, a4), clarity); }
    void hold (double midi, double seconds, double clarity = 0.98)
    {
        for (int i = 0; i < count (seconds); ++i)
            add (midi, clarity);
    }
    void glide (double from, double to, double seconds)
    {
        const auto n = count (seconds);
        for (int i = 1; i <= n; ++i)
            add (from + (to - from) * i / n);
    }
    void vibrato (double midi, double cents, double rate, double seconds)
    {
        for (int i = 0; i < count (seconds); ++i)
            add (midi + cents / 100.0 * std::sin (juce::MathConstants<double>::twoPi * rate * i * hop));
    }
    double now() const { return (double) hops.size() * hop; }
};

struct Event
{
    double time;
    Change change;
    int note;
};

std::vector<Event> run (const Script& s, NoteTracker::Settings settings = {})
{
    NoteTracker t;
    t.prepare (hop, settings);
    std::vector<Event> events;
    for (size_t i = 0; i < s.hops.size(); ++i)
        if (const auto c = t.update (s.hops[i].first, s.hops[i].second); c != Change::none)
            events.push_back ({ (double) (i + 1) * hop, c, t.getNote() });
    return events;
}

juce::String describe (const std::vector<Event>& events)
{
    juce::StringArray parts;
    for (const auto& e : events)
        parts.add (juce::String (nameOf (e.change)) + (e.note >= 0 ? " " + juce::MidiMessage::getMidiNoteName (e.note, true, true, 4) : juce::String())
                   + " at " + juce::String (e.time * 1000.0, 1) + " ms");
    return parts.joinIntoString (", ");
}

/// A harmonic tone (partials 1/n) that follows a pitch track sample by sample, phase continuous, so a
/// change of note is a hammer-on (no new attack). `track` gives MIDI pitch, 0 for silence; several
/// pitches at once make a chord.
std::vector<float> render (const std::function<std::vector<double> (double)>& track, double seconds)
{
    std::vector<float> out ((size_t) (seconds * fs));
    std::array<double, 4> phase {};
    for (size_t i = 0; i < out.size(); ++i)
    {
        const auto t = (double) i / fs;
        const auto notes = track (t);
        double v = 0.0;
        for (size_t k = 0; k < notes.size() && k < phase.size(); ++k)
        {
            phase[k] += hz (notes[k]) / fs;
            phase[k] -= std::floor (phase[k]);
            for (int n = 1; n <= 6; ++n)
                v += std::sin (juce::MathConstants<double>::twoPi * n * phase[k]) / n;
        }
        out[i] = (float) (0.1 * v);
    }
    return out;
}
} // namespace

class HarmonyTests final : public juce::UnitTest
{
public:
    HarmonyTests() : juce::UnitTest ("Harmony", "ampsim") {}

    void runTest() override
    {
        scales();
        outOfKey();
        exhaustive();
        trackerOnsets();
        trackerExpression();
        trackerTransitions();
        trackerConfidence();
        endToEnd();
        realtime();
    }

private:
    // ------------------------------------------------------------------------------------------
    void scales()
    {
        beginTest ("scales: a diatonic third is 3 or 4 semitones depending on the degree; steps wrap into octaves");

        const Key cMajor { 0, Scale::major };
        const Interval third { true, 2, 0 }, fifth { true, 4, 0 };
        juce::StringArray thirds;
        const std::array<std::pair<int, int>, 7> expectedThirds { { { 60, 4 }, { 62, 3 }, { 64, 3 }, { 65, 4 }, { 67, 4 }, { 69, 3 }, { 71, 3 } } };
        for (const auto& [note, expected] : expectedThirds)
        {
            const auto shift = shiftFor (note, third, cMajor, OutOfScale::parallel);
            expectEquals (shift, expected);
            thirds.add (juce::MidiMessage::getMidiNoteName (note, true, false, 4) + (shift >= 0 ? " +" : " ") + juce::String (shift));
        }
        expectEquals (shiftFor (60, fifth, cMajor, OutOfScale::parallel), 7);
        expectEquals (shiftFor (71, fifth, cMajor, OutOfScale::parallel), 6); // B to F: the key's diminished fifth
        expectEquals (shiftFor (60, { true, 7, 0 }, cMajor, OutOfScale::parallel), 12);
        expectEquals (shiftFor (60, { true, -2, 0 }, cMajor, OutOfScale::parallel), -3); // a third below C is A
        expectEquals (shiftFor (60, { true, -7, 0 }, cMajor, OutOfScale::parallel), -12);
        expectEquals (shiftFor (60, { true, 2, 1 }, cMajor, OutOfScale::parallel), 16);   // a tenth
        expectEquals (shiftFor (64, { true, 0, -1 }, cMajor, OutOfScale::parallel), -12); // octave below

        // E natural minor: E-G, F#-A, G-B, A-C, B-D, C-E, D-F#.
        const Key eMinor { 4, Scale::naturalMinor };
        const std::array<std::pair<int, int>, 7> minorThirds { { { 64, 3 }, { 66, 3 }, { 67, 4 }, { 69, 3 }, { 71, 3 }, { 72, 4 }, { 74, 4 } } };
        for (const auto& [note, expected] : minorThirds)
            expectEquals (shiftFor (note, third, eMinor, OutOfScale::parallel), expected);

        // Scale sizes, roots, and the 8-note whole-half diminished: 8 steps make the octave.
        juce::StringArray sizes;
        for (int s = 0; s < numScales - 1; ++s)
        {
            const auto mask = maskOf ((Scale) s);
            const auto size = std::popcount ((unsigned) mask);
            expect ((mask & 1u) != 0, scaleNames[(size_t) s]);
            expectEquals (size, s == (int) Scale::wholeHalfDiminished ? 8 : 7, scaleNames[(size_t) s]);
            sizes.add (juce::String (scaleNames[(size_t) s]) + " " + juce::String (size));
        }
        const Key diminished { 0, Scale::wholeHalfDiminished };
        juce::StringArray steps;
        for (int k = 1; k <= 8; ++k)
            steps.add (juce::String (shiftFor (60, { true, k, 0 }, diminished, OutOfScale::parallel)));
        expect (steps.joinIntoString (" ") == "2 3 5 6 8 9 11 12");

        // A custom pentatonic (0 2 4 7 9), and an empty custom mask, which counts as all 12 notes.
        const Key pentatonic { 0, Scale::custom, (std::uint16_t) ((1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9)) };
        expectEquals (shiftFor (64, { true, 1, 0 }, pentatonic, OutOfScale::parallel), 3);  // E to G
        expectEquals (shiftFor (69, { true, 1, 0 }, pentatonic, OutOfScale::parallel), 3);  // A to C
        expectEquals (shiftFor (69, { true, 5, 0 }, pentatonic, OutOfScale::parallel), 12);
        const Key empty { 0, Scale::custom, 0 };
        expectEquals (shiftFor (61, { true, 2, 0 }, empty, OutOfScale::parallel), 2);

        // Chromatic intervals ignore the key.
        expectEquals (shiftFor (61, { false, 7, 0 }, cMajor, OutOfScale::parallel), 7);
        expectEquals (shiftFor (61, { false, -5, -1 }, eMinor, OutOfScale::snap), -17);

        logMessage ("  -> diatonic thirds in C major: " + thirds.joinIntoString (", ") + "; B's fifth +6 (B to F); 7 steps +12; a third below C -3; "
                    "whole-half diminished from C, steps 1 to 8: " + steps.joinIntoString (" "));
        logMessage ("  -> scale sizes: " + sizes.joinIntoString (", ") + "; a custom pentatonic's step from E is +3, five steps an octave; chromatic "
                    "intervals ignore the key");
    }

    // ------------------------------------------------------------------------------------------
    void outOfKey()
    {
        beginTest ("notes outside the key: Parallel keeps the nearest key note's shift, Snap lands on its harmony, and ties go to the lower note");

        const Key cMajor { 0, Scale::major };
        const Interval third { true, 2, 0 };
        juce::StringArray results;
        // Each out-of-key note in C major sits exactly between two key notes; the lower one wins.
        struct Case
        {
            int note, nearest, parallel, snap;
        };
        for (const auto& c : { Case { 61, 60, 4, 3 }, Case { 63, 62, 3, 2 }, Case { 66, 65, 4, 3 }, Case { 68, 67, 4, 3 }, Case { 70, 69, 3, 2 } })
        {
            const auto parallel = shiftFor (c.note, third, cMajor, OutOfScale::parallel);
            const auto snap = shiftFor (c.note, third, cMajor, OutOfScale::snap);
            expectEquals (nearestInKey (c.note, cMajor), c.nearest);
            expectEquals (parallel, c.parallel);
            expectEquals (snap, c.snap);
            const auto snapped = c.note + snap;
            expect ((cMajor.mask() >> floorMod12 (snapped)) & 1u, "snap lands in the key");
            results.add (juce::MidiMessage::getMidiNoteName (c.note, true, false, 4) + ": parallel +" + juce::String (parallel) + " ("
                         + juce::MidiMessage::getMidiNoteName (c.note + parallel, true, false, 4) + "), snap +" + juce::String (snap) + " ("
                         + juce::MidiMessage::getMidiNoteName (snapped, true, false, 4) + ")");
        }
        // A note with a nearer key note above it (G in A harmonic minor: G# is one semitone up, F two down).
        const Key aHarmonic { 9, Scale::harmonicMinor };
        expectEquals (nearestInKey (67, aHarmonic), 68);

        logMessage ("  -> thirds in C major: " + results.joinIntoString ("; ") + "; G in A harmonic minor goes to G#, the nearer one");
    }

    // ------------------------------------------------------------------------------------------
    void exhaustive()
    {
        beginTest ("every scale, root, note (C2 to B5), and step from -7 to +7: harmonies stay in the key and move the right way, a trip round the scale is an octave");

        int cases = 0, failures = 0;
        std::vector<Key> keys;
        for (int s = 0; s < numScales - 1; ++s)
            for (int root = 0; root < 12; ++root)
                keys.push_back ({ root, (Scale) s });
        for (const auto mask : { (std::uint16_t) 0x295, (std::uint16_t) 0x001, (std::uint16_t) 0xfff, (std::uint16_t) 0x891 })
            for (int root = 0; root < 12; ++root)
                keys.push_back ({ root, Scale::custom, mask });

        for (const auto& key : keys)
        {
            const auto mask = key.mask();
            const auto size = std::popcount ((unsigned) mask);
            for (int note = 36; note < 84; ++note)
            {
                const auto anchor = nearestInKey (note, key);
                const auto inKey = ((mask >> floorMod12 (note - key.root)) & 1u) != 0;
                failures += (inKey ? anchor == note : std::abs (anchor - note) <= 6) ? 0 : 1;

                for (int steps = -7; steps <= 7; ++steps)
                {
                    ++cases;
                    const auto target = diatonicTarget (note, steps, key);
                    const auto targetInKey = ((mask >> floorMod12 (target - key.root)) & 1u) != 0;
                    const auto rightWay = steps > 0 ? target > anchor : steps < 0 ? target < anchor : target == anchor;
                    const Interval interval { true, steps, 0 };
                    const auto agree = ! inKey || shiftFor (note, interval, key, OutOfScale::parallel) == shiftFor (note, interval, key, OutOfScale::snap);
                    failures += targetInKey && rightWay && agree ? 0 : 1;
                }
                ++cases;
                failures += shiftFor (note, { true, size, 0 }, key, OutOfScale::parallel) == 12 ? 0 : 1;
            }
        }
        expectEquals (failures, 0);
        logMessage ("  -> " + juce::String ((int) keys.size()) + " keys (11 scales and 4 custom masks, every root) x 48 notes: " + juce::String (cases)
                    + " cases, " + juce::String (failures) + " failures");
    }

    // ------------------------------------------------------------------------------------------
    void trackerOnsets()
    {
        beginTest ("note tracker: an onset takes two agreeing confident estimates, so a stray reading never starts a note");

        Script plain;
        plain.silence (0.05);
        const auto firstEstimate = plain.now() + hop;
        plain.hold (64.0, 0.2);
        const auto a = run (plain);
        expect (a.size() == 1 && a[0].change == Change::onset && a[0].note == 64);
        const auto onsetDelay = a.empty() ? 1.0 : a[0].time - firstEstimate;
        expectWithinAbsoluteError (onsetDelay, hop, 1.0e-9);

        // A confident stray reading of another note first, as the detector can give just after a pluck.
        Script stray;
        stray.silence (0.05);
        stray.add (62.3);
        stray.hold (64.0, 0.2);
        const auto b = run (stray);
        expect (b.size() == 1 && b[0].note == 64);

        // A4 = 432 Hz: 432 Hz is A4.
        Script tuned;
        tuned.silence (0.02);
        for (int i = 0; i < 20; ++i)
            tuned.add (69.0, 0.98, 432.0);
        NoteTracker::Settings at432;
        at432.referenceA4 = 432.0;
        const auto c = run (tuned, at432);
        expect (c.size() == 1 && c[0].note == 69);

        logMessage ("  -> E4 after silence: " + describe (a) + " (" + juce::String (onsetDelay * 1000.0, 2) + " ms, one hop, after the detector's first "
                    "reading); a stray D4 reading first: " + describe (b) + "; at A4 = 432 Hz, 432 Hz reads " + describe (c));
    }

    // ------------------------------------------------------------------------------------------
    void trackerExpression()
    {
        beginTest ("note tracker: vibrato and quick bends keep the note (the harmony follows in parallel); a bend held on a new note re-evaluates like a slide");

        const auto changesAfterOnset = [] (const std::vector<Event>& events) { return (int) events.size() - 1; };

        Script vibrato;
        vibrato.silence (0.02);
        vibrato.vibrato (69.0, 30.0, 6.0, 2.0);
        const auto v = run (vibrato);
        Script wide;
        wide.silence (0.02);
        wide.vibrato (69.0, 45.0, 6.0, 2.0);
        const auto w = run (wide);
        expectEquals (changesAfterOnset (v), 0);
        expectEquals (changesAfterOnset (w), 0);

        // A semitone bend up and straight back down in 80 ms: within 25 cents of F for under 30 ms.
        Script quick;
        quick.silence (0.02);
        quick.hold (64.0, 0.1);
        quick.glide (64.0, 65.0, 0.04);
        quick.glide (65.0, 64.0, 0.04);
        quick.hold (64.0, 0.2);
        const auto q = run (quick);
        expectEquals (changesAfterOnset (q), 0);

        // The same bend held on F for 200 ms: re-evaluated once it has held F for 30 ms.
        Script held;
        held.silence (0.02);
        held.hold (64.0, 0.1);
        held.glide (64.0, 65.0, 0.08);
        const auto reachedZone = held.now() - 0.08 * 0.25; // 75% of the way up the glide is 25 cents from F
        held.hold (65.0, 0.2);
        const auto h = run (held);
        expect (h.size() == 2 && h[1].change == Change::slide && h[1].note == 65);
        const auto heldAfter = h.size() == 2 ? h[1].time - reachedZone : 0.0;
        expectWithinAbsoluteError (heldAfter, 0.030, 2.0 * hop);

        logMessage ("  -> vibrato +-30 and +-45 cents at 6 Hz for 2 s: " + juce::String (changesAfterOnset (v)) + " and " + juce::String (changesAfterOnset (w))
                    + " changes after the onset; a semitone bend up and back in 80 ms: " + juce::String (changesAfterOnset (q))
                    + "; the bend held on F: " + describe (h) + " (" + juce::String (heldAfter * 1000.0, 1) + " ms after it came within 25 cents of F)");
    }

    // ------------------------------------------------------------------------------------------
    void trackerTransitions()
    {
        beginTest ("note tracker: legato jumps re-evaluate two hops after the new note is read; slides only once they settle");

        // Hammer-on E4 to F#4, with the detector still reading E4 for 2 hops and one stray reading
        // between (as measured on the real detector).
        Script hammer;
        hammer.silence (0.02);
        hammer.hold (64.0, 0.1);
        hammer.add (64.0);
        hammer.add (64.0);
        hammer.add (61.7);
        const auto firstNew = hammer.now() + hop;
        hammer.hold (66.0, 0.1);
        const auto j = run (hammer);
        expect (j.size() == 2 && j[1].change == Change::jump && j[1].note == 66);
        const auto jumpDelay = j.size() == 2 ? j[1].time - firstNew : 1.0;
        expectWithinAbsoluteError (jumpDelay, 2.0 * hop, 1.0e-9);

        // Pull-off and tap: down a fourth, up an octave.
        Script legato;
        legato.silence (0.02);
        legato.hold (69.0, 0.1);
        legato.hold (64.0, 0.1);
        legato.hold (76.0, 0.1);
        const auto l = run (legato);
        expect (l.size() == 3 && l[1].note == 64 && l[2].note == 76 && l[1].change == Change::jump && l[2].change == Change::jump);

        // A whole-tone slide over 80 ms: through F in 20 ms (no change there), re-evaluated on F#.
        Script slide;
        slide.silence (0.02);
        slide.hold (64.0, 0.1);
        slide.glide (64.0, 66.0, 0.08);
        const auto slideZone = slide.now() - 0.08 * 0.125;
        slide.hold (66.0, 0.2);
        const auto s = run (slide);
        expect (s.size() == 2 && s[1].change == Change::slide && s[1].note == 66);
        const auto settled = s.size() == 2 ? s[1].time - slideZone : 0.0;

        // A slow slide (400 ms) lingers near F for 100 ms, so F is re-evaluated on the way: that's the rule.
        Script slow;
        slow.silence (0.02);
        slow.hold (64.0, 0.1);
        slow.glide (64.0, 66.0, 0.4);
        slow.hold (66.0, 0.2);
        const auto sl = run (slow);
        expect (sl.size() == 3 && sl[1].note == 65 && sl[2].note == 66);

        logMessage ("  -> hammer-on E4 to F#4 (stale E4 twice, then a stray reading): " + describe (j) + ", " + juce::String (jumpDelay * 1000.0, 2)
                    + " ms after the first F#4 reading");
        logMessage ("  -> pull-off and tap: " + describe (l));
        logMessage ("  -> a whole-tone slide in 80 ms: " + describe (s) + " (" + juce::String (settled * 1000.0, 1) + " ms after reaching F#4's +-25 cents); "
                    "the same slide over 400 ms: " + describe (sl));
    }

    // ------------------------------------------------------------------------------------------
    void trackerConfidence()
    {
        beginTest ("note tracker: confidence lost for 5 ms (a chord, noise) releases the note; a 2.7 ms dip doesn't; the next note comes back");

        Script s;
        s.silence (0.02);
        s.hold (64.0, 0.1);
        s.hold (64.0, 2.0 * hop, 0.5); // a dip of two hops
        s.hold (64.0, 0.1);
        const auto chordStart = s.now();
        s.hold (60.0, 0.06, 0.6); // a chord: unconfident
        s.hold (67.0, 0.1);
        const auto e = run (s);
        expect (e.size() == 3 && e[0].note == 64 && e[1].change == Change::release && e[2].change == Change::onset && e[2].note == 67);
        const auto releaseAfter = e.size() >= 2 ? e[1].time - chordStart : 0.0;
        expectLessOrEqual (releaseAfter, 0.005 + hop);
        logMessage ("  -> " + describe (e) + "; released " + juce::String (releaseAfter * 1000.0, 2) + " ms into the chord");
    }

    // ------------------------------------------------------------------------------------------
    void endToEnd()
    {
        beginTest ("on audio through the shared detector (110 Hz floor, 3 ms window): legato re-evaluates within ~10 ms of the new note, a slide once it lands, a chord releases");

        // A3, hammer-on B3 at 0.3 s, pull-off to A3 at 0.5 s, tap E4 at 0.7 s, slide E4 to G4 from 0.9 to
        // 1.0 s, a chord (A3 C#4 E4) from 1.3 to 1.6 s, then D4.
        const auto track = [] (double t) -> std::vector<double>
        {
            if (t < 0.05) return {};
            if (t < 0.3) return { 57.0 };
            if (t < 0.5) return { 59.0 };
            if (t < 0.7) return { 57.0 };
            if (t < 0.9) return { 64.0 };
            if (t < 1.0) return { 64.0 + 3.0 * (t - 0.9) / 0.1 };
            if (t < 1.3) return { 67.0 };
            if (t < 1.6) return { 57.0, 61.0, 64.0 };
            if (t < 1.9) return { 62.0 };
            return {};
        };
        const auto audio = render (track, 2.0);

        ampsim::PitchDetector detector;
        detector.prepare (fs, ampsim::PitchDetector::Settings::harmonizer());
        NoteTracker tracker;
        tracker.prepare (hop, {});
        std::vector<Event> events;
        for (size_t start = 0; start + 64 <= audio.size(); start += 64)
        {
            detector.push (audio.data() + start, 64);
            const auto e = detector.detect();
            if (const auto c = tracker.update (e.frequency, e.clarity); c != Change::none)
                events.push_back ({ (double) (start + 64) / fs, c, tracker.getNote() });
        }

        // Each expected change: when the note changed, and what the tracker should say.
        struct Expected
        {
            double at;
            Change change;
            int note;
        };
        const std::vector<Expected> expected { { 0.05, Change::onset, 57 }, { 0.3, Change::jump, 59 }, { 0.5, Change::jump, 57 },
                                               { 0.7, Change::jump, 64 }, { 1.0, Change::slide, 67 }, { 1.3, Change::release, -1 },
                                               { 1.6, Change::onset, 62 }, { 1.9, Change::release, -1 } };
        expectEquals ((int) events.size(), (int) expected.size(), describe (events));
        juce::StringArray delays;
        double worstLegato = 0.0;
        for (size_t i = 0; i < std::min (events.size(), expected.size()); ++i)
        {
            const auto& got = events[i];
            const auto& want = expected[i];
            expect (got.change == want.change && got.note == want.note, describe (events));
            const auto delay = got.time - want.at;
            delays.add (juce::String (nameOf (got.change)) + " " + (got.note >= 0 ? juce::MidiMessage::getMidiNoteName (got.note, true, true, 4) : juce::String())
                        + " +" + juce::String (delay * 1000.0, 1) + " ms");
            if (want.change == Change::jump)
                worstLegato = std::max (worstLegato, delay);
        }
        expectLessThan (worstLegato, 0.0125);
        logMessage ("  -> after each change in the audio: " + delays.joinIntoString (", ") + "; slowest legato re-evaluation " + juce::String (worstLegato * 1000.0, 1)
                    + " ms (plan: about 10 ms); no other changes");
    }

    // ------------------------------------------------------------------------------------------
    void realtime()
    {
        beginTest ("real time: the tracker and the scale rules allocate and lock nothing");

        Script s;
        s.silence (0.02);
        for (int k = 0; k < 40; ++k)
        {
            s.hold (60.0 + (k * 5) % 17, 0.03);
            s.vibrato (60.0 + (k * 7) % 13, 30.0, 6.0, 0.05);
            s.hold (64.0, 0.006, 0.4);
        }
        NoteTracker t;
        t.prepare (hop, {});
        const Key key { 4, Scale::harmonicMinor };
        int sum = 0;
        rtcheck::begin();
        for (const auto& [f, c] : s.hops)
        {
            t.update (f, c);
            if (t.hasNote())
                for (int v = -7; v <= 7; ++v)
                    sum += shiftFor (t.getNote(), { true, v, 0 }, key, OutOfScale::parallel) + shiftFor (t.getNote(), { true, v, 0 }, key, OutOfScale::snap);
        }
        const auto totals = rtcheck::end();
        expectEquals (totals.allocations, 0L);
        expectEquals (totals.frees, 0L);
        expectEquals (totals.blockingLocks, 0L);
        logMessage ("  -> " + juce::String ((int) s.hops.size()) + " hops with every rule exercised and 30 shift computations each (checksum " + juce::String (sum)
                    + "): " + juce::String (totals.allocations) + " allocations, " + juce::String (totals.frees) + " frees, " + juce::String (totals.blockingLocks) + " locks");
    }
};

static HarmonyTests harmonyTests;
