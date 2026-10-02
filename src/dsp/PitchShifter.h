#pragma once

#include "ModulatedDelay.h"
#include "PitchDetector.h"
#include "Svf.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace ampsim
{

/// The shared pitch shifter (BUILD_PLAN "Pitch shifter", design review round 13): one module, two engines.
/// This file holds what both engines read (PitchShifterInput) and the granular engine (GranularVoice, and
/// GranularShifter for a self-contained mono shifter such as the shimmer reverb's). Psola.h holds the PSOLA
/// engine. The multivoicer runs both; the harmonizer (Phase 10) uses PSOLA; the shimmer uses granular.
///
/// Both engines are delay-line readers: the dry signal is never touched, and each voice's output is the
/// input read back at a delay that its algorithm moves. Reading y(t) = x(t - d(t)) maps input time to
/// output time with slope 1 - d'(t), so every frequency comes out multiplied by 1 - d' (the Doppler
/// relation in ModulatedDelay.h). A steady pitch ratio r needs d' = 1 - r for good, which a bounded delay
/// can only do piecewise: the read head sweeps, and every so often it has to jump back (r > 1) or ahead
/// (r < 1) and crossfade from the old head to the new one. That jump is the whole problem of time-domain
/// pitch shifting (Zolzer (ed.), "DAFX", 2nd ed. 2011, the time-segment processing chapter).

/// sum a_i b_i in double precision: the correlation searches' inner loop (four running sums).
double pitchShifterDot (const float* a, const float* b, int n) noexcept;

/// A pitch ratio that glides to its target at a constant rate in the log domain (the same number of cents
/// every sample, so a glide sounds like a straight portamento), over a settable time. Unlike
/// juce::SmoothedValue, changing the glide time never snaps the value.
class RatioGlide
{
public:
    void setGlideSamples (int samples) noexcept { glideSamples = std::max (0, samples); }
    void jumpTo (double value) noexcept
    {
        current = target = value;
        remaining = 0;
    }
    void setTarget (double value) noexcept;

    /// One sample further along the glide; returns the new value.
    double next() noexcept
    {
        if (remaining > 0)
        {
            current *= step;
            if (--remaining == 0)
                current = target;
        }
        return current;
    }

    double getCurrent() const noexcept { return current; }
    double getTarget() const noexcept { return target; }
    bool isGliding() const noexcept { return remaining > 0; }

private:
    double current = 1.0, target = 1.0, step = 1.0;
    int glideSamples = 0, remaining = 0;
};

/// The signal a set of voices shifts, kept three ways: a ModulatedDelay line for the voices' Hermite reads
/// (the shared engine's low-level read()/write() path), the same samples as one contiguous array for the
/// correlation searches (splice alignment here, pitch marks in Psola.h), and a 12 kHz copy for the coarse
/// half of the splice search (a 4th-order Butterworth low-pass at 3 kHz, then every 4th sample).
///
/// Read before write, as in ModulatedDelay: per sample, every voice reads, then push() stores the new
/// sample. So read(d) is the input d samples before the one about to be pushed, and getTime() is the index
/// of that sample. One input serves any number of voices (the multivoicer's eight voices share one).
class PitchShifterInput
{
public:
    static constexpr int decimation = 4;
    static constexpr double lowpassHz = 3000.0;

    /// Allocates the line for delays up to maxDelayMs and the histories for searches reaching
    /// maxDelayMs + searchMarginMs back.
    void prepare (double sampleRate, double maxDelayMs, double searchMarginMs);

    /// Zeros everything (the input starts silent). Real-time safe.
    void reset() noexcept;

    /// Stores the next input sample, after the voices have read.
    void push (float x) noexcept
    {
        line.write (x);
        history.push (x);

        auto y = (double) x;
        for (auto& section : lowpass)
            y = section.processSample (y);
        if (++phase == decimation)
        {
            phase = 0;
            decimated.push ((float) y);
        }
        ++time;
    }

    /// The input `delaySamples` before the sample about to be pushed, by Hermite interpolation, clamped to
    /// [2, getMaxDelaySamples()] (ModulatedDelay::read).
    float read (double delaySamples) const noexcept { return line.read (delaySamples); }

    double getMaxDelaySamples() const noexcept { return line.getMaxDelaySamples(); }
    double getSampleRate() const noexcept { return sampleRate; }

    /// Samples pushed since the last reset: the index of the sample about to be pushed.
    int64_t getTime() const noexcept { return time; }

    /// The newest `length` input samples, oldest first: element length - k is the sample k back (delay k).
    const float* newest (int length) const noexcept { return history.newest (length); }
    int getHistoryCapacity() const noexcept { return history.getCapacity(); }

    /// The newest `length` samples of the 12 kHz copy, oldest first. Decimated sample j back from the
    /// newest (element length - 1 - j) sits at full-rate delay getDecimatedOffset() + 4 j.
    const float* newestDecimated (int length) const noexcept { return decimated.newest (length); }
    int getDecimatedCapacity() const noexcept { return decimated.getCapacity(); }
    int getDecimatedOffset() const noexcept { return phase + 1; }

private:
    double sampleRate = 48000.0;
    ModulatedDelay line;
    ContiguousHistory history, decimated;
    std::array<Svf, 2> lowpass;
    int phase = 0;
    int64_t time = 0;
};

/// One granular (Poly) voice: two read heads on a PitchShifterInput, each sweeping its delay at d' = 1 - r
/// (a sawtooth), with one crossfade at a time from the head that has run out of room to a fresh one. Works
/// on anything, chords included, because it never needs to know the pitch.
///
/// Design (the study is `prototypes/pitch_shifter.py --study granular`; its numbers are quoted here):
///
/// 1. Two heads, not three or four always overlapping. Heads that overlap read the same signal at different
///    delays, and their sum is a comb filter. With four Hann-windowed heads (the classic design, heads 5 ms
///    apart at an octave up with 20 ms hops) a sine's level depends on its frequency by up to 35 dB, in
///    teeth every 50 Hz of input: a static comb across the whole spectrum. Two heads that only overlap
///    while crossfading hold every frequency at the same level; between crossfades the output is one head,
///    which is an exact resampler.
///
/// 2. Aligned splices. A head that reads x at slope 1 - r produces r f exactly, but when the next head takes
///    over J samples further back (or ahead), every component jumps in phase by 2 pi f J. A plain sawtooth
///    shifter splices every H samples with J = (r - 1) H, so each partial's phase steps by the same amount
///    every splice and its long-run frequency becomes r f + wrap(2 pi f (r - 1) H) / (2 pi H), up to
///    1/(2H) Hz off (measured with H = 20 ms: 112.5 Hz up an octave lands 32 cents flat, 124 Hz 22 cents
///    sharp). So the jump is chosen, not fixed: when a head runs out of room, the new head's offset J is
///    searched over a 32 ms range for the best normalized cross-correlation between the 12 ms of input
///    behind the old head and the 12 ms J samples away (the waveform-similarity search of WSOLA: Verhelst and
///    Roelands, ICASSP 1993), coarsely on the 12 kHz copy and then to the sample with a parabola. A jump of a
///    whole number of periods is phase-continuous, so a single note comes out at exactly r f (measured within
///    0.006 cents), and so does any chord whose notes share a period within the range: power chords (24 ms
///    on the low E) and major triads (about 30 ms in the third octave). Chords without one (add9, minor 7th
///    voicings) can only be matched partly, and the leftover phase step, the same at every splice, pulls
///    their notes off pitch: up to about 60 cents in the worst measured case (an open Gadd9 down an
///    octave). That is the granular engine's known weakness, measured in the tests. The range is the
///    trade: +-12 ms leaves major triads up to 56 cents off, +-20 ms fixes add9 too but trails 34 ms.
///
/// 3. When to splice. A head runs until its delay nears the edge of a 37 ms band, less the room its
///    crossfade needs: going down for r > 1 (the new head lands in the top 32 ms, preferring the middle),
///    going up for r < 1. Near unison that is rare (+10 cents: every few seconds), and at r = 1 the heads
///    never move: a plain delay. Band = 2 x 16 ms (search range) + 3 ms (crossfade travel) + 2 ms (smallest
///    jump). A rising head (r < 1) tries one early search once it is 17 ms up, for a jump of up to 12 ms that
///    lands it near the bottom, and takes it if the match is good (correlation 0.9 or more, which any single
///    note reaches); otherwise (a chord needing a longer jump) it climbs on to the full search at the top.
///    That keeps down-shifted single notes at about 20 ms of trail instead of 26.
///
/// 4. The crossfade. Two heads at different delays add like two partly correlated signals, so neither
///    equal-gain (right for identical signals) nor equal-power (right for unrelated ones) keeps the level:
///    the correlation rho decides (Fink, Holters and Zolzer, "Signal-matched power-complementary
///    cross-fading and dry-wet mixing", DAFx 2016). With base gains cos and sin of (pi/2) u, both are
///    divided by sqrt(1 + 2 rho sin cos), which holds the expected power at exactly 1 for correlation rho:
///    rho = 1 makes it an equal-gain fade, rho = 0 equal-power. rho is clamped to [0, 1], and measured at the
///    chosen jump on the 12 ms of input before the search's window, which the search never saw: the search
///    picks the best of about 400 candidates, so its own correlation is biased upward on noise (by about
///    0.25, which made noise dip 1 dB at every splice), while a fresh window is not. The fade lasts
///    3 ms / |1 - r|, kept between 1 and 6 ms, so the old head travels at most 3 ms while it fades.
///
/// 5. Delay changes (the multivoicer's per-voice delay) re-place the head with one crossfade to a head at
///    exactly the new delay, so the delay knob never bends pitch; a slow drift offset (the multivoicer's
///    "separate takes" wander) moves the band and the heads together, smoothly.
///
/// The trail (on top of the voice's own delay): about 10 ms for up-shifts, 20 ms for down-shifted single
/// notes (up to 26 ms on chords), 19.5 ms at unison: a doubling delay, not latency; the dry is never
/// delayed.
///
/// Ratio changes glide in the log domain over glideMs (RatioGlide), and because the heads move by 1 - r(t)
/// every sample, the pitch follows the glide exactly.
class GranularVoice
{
public:
    static constexpr double minRatio = 0.25, maxRatio = 4.0; // -24 to +24 semitones
    static constexpr double alignRangeMs = 16.0;  // the new head may land anywhere in 2 x this
    static constexpr double windowMs = 12.0;      // correlation window
    static constexpr double jumpFloorMs = 2.0;    // the smallest jump
    static constexpr double fadeTravelMs = 3.0;   // the most the old head travels while fading out
    static constexpr double minFadeMs = 1.0, maxFadeMs = 6.0;
    static constexpr double floorMs = 1.0;        // the band's bottom above the voice's delay
    static constexpr double bandMs = 2.0 * alignRangeMs + fadeTravelMs + jumpFloorMs; // 37 ms
    static constexpr double replaceThresholdMs = 0.25; // a delay change this big re-places the head
    static constexpr double earlyRangeMs = 12.0;  // r < 1: first try a jump of up to this, once the head is this high
    static constexpr double earlyQuality = 0.9;   // ... and take it if the match is at least this good
    static constexpr double tieBreak = 0.05;      // coarse search: near-ties go to the preferred landing

    /// How much input the voices' searches need beyond the deepest delay.
    static constexpr double searchMarginMs = 2.0 * windowMs + 1.0;

    /// The delay a voice can reach: its own delay plus the band, plus the drift a caller adds.
    static double maxDelayMs (double maxVoiceDelayMs, double maxDriftMs) noexcept
    {
        return maxVoiceDelayMs + maxDriftMs + floorMs + bandMs + maxFadeMs;
    }

    struct Settings
    {
        double ratio = 1.0;   // 0.25 to 4
        double delayMs = 0.0; // the voice's own delay on top of the band
        double glideMs = 30.0;
    };

    /// The result of a splice search: the jump J (new head's delay minus the old one's, samples), the
    /// normalized correlation there, in [0, 1], and whether anything matched at all (false in silence).
    struct Splice
    {
        double jump = 0.0, correlation = 0.0;
        bool matched = false;
    };

    void prepare (double sampleRate);

    /// Heads to the middle of the band at the target settings, no fade, no glide. Real-time safe.
    void reset() noexcept;

    /// Audio thread: the new targets.
    void setSettings (const Settings& settings) noexcept;

    /// One output sample, read from `input` (before input.push() of this sample). ratioScale multiplies the
    /// glided ratio (pitch drift); extraDelaySamples is a smooth delay offset added to the band and the
    /// heads (timing drift).
    float process (const PitchShifterInput& input, double ratioScale = 1.0, double extraDelaySamples = 0.0) noexcept;

    /// The best splice for an old head at delay delayA among jumps in [jumpLo, jumpHi]: coarse search on the
    /// 12 kHz copy, refined at 48 kHz with parabolic interpolation. Near-ties go to the jump nearest
    /// `preferred`. Public for the tests.
    Splice findSplice (const PitchShifterInput& input, double delayA, double jumpLo, double jumpHi, double preferred) const noexcept;

    /// The normalized correlation for one whole-sample jump, in [0, 1], over the window of the old head's
    /// input that starts windowOffset samples further back than the search's.
    double correlationAt (const PitchShifterInput& input, double delayA, int jump, int windowOffset = 0) const noexcept;

    // For tests and meters.
    double getDelaySamples() const noexcept { return heads[(size_t) current].delay; }
    double getRatio() const noexcept { return ratio.getCurrent(); }
    bool isFading() const noexcept { return fading; }
    int getSpliceCount() const noexcept { return splices; }
    Splice getLastSplice() const noexcept { return lastSplice; }
    double getFadeCorrelation() const noexcept { return fadeCorrelation; }
    double getBandLow() const noexcept { return bandLow; }
    double getBandHigh() const noexcept { return bandLow + bandWidth; }

private:
    struct Head
    {
        double delay = 0.0;
        bool active = false;
    };

    void startFade (double newDelay, double correlation, double fadeSamples) noexcept;
    void maybeSplice (const PitchShifterInput& input, double slope) noexcept;
    double fadeLength (double slope) const noexcept;

    double sampleRate = 48000.0;
    Settings settings;
    RatioGlide ratio;

    // Sizes in samples, set in prepare().
    int alignRange = 768, window = 576, coarseWindow = 144;
    double jumpFloor = 96.0, fadeTravel = 144.0, minFade = 48.0, maxFade = 288.0;
    double floorDelay = 48.0, bandWidth = 1776.0, replaceThreshold = 12.0, earlyReach = 816.0;

    std::array<Head, 2> heads;
    int current = 0;
    bool fading = false, earlyTried = false;
    double fadePosition = 0.0, fadeStep = 0.0, fadeCorrelation = 0.0;

    double placedDelay = 0.0; // the voice delay (samples) the heads were placed for
    double targetDelay = 0.0; // the voice delay asked for
    double drift = 0.0;       // the extra delay at the last sample
    double bandLow = 0.0;

    int splices = 0;
    Splice lastSplice;
};

/// A self-contained mono granular shifter: its own input and one voice. Sample by sample with no
/// assumptions about the input, so it can sit inside a feedback loop (the shimmer reverb): its output only
/// depends on inputs at least 1 ms old.
class GranularShifter
{
public:
    void prepare (double sampleRate, double maxVoiceDelayMs = 0.0)
    {
        input.prepare (sampleRate, GranularVoice::maxDelayMs (maxVoiceDelayMs, 0.0), GranularVoice::searchMarginMs);
        voice.prepare (sampleRate);
    }

    void reset() noexcept
    {
        input.reset();
        voice.reset();
    }

    void setSettings (const GranularVoice::Settings& s) noexcept { voice.setSettings (s); }

    float processSample (float x) noexcept
    {
        const auto y = voice.process (input);
        input.push (x);
        return y;
    }

    void process (const float* in, float* out, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
            out[i] = processSample (in[i]);
    }

    const GranularVoice& getVoice() const noexcept { return voice; }

private:
    PitchShifterInput input;
    GranularVoice voice;
};

} // namespace ampsim
