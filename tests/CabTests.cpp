// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AllocationTracking.h"
#include "TestHelpers.h"
#include "dsp/Cab.h"
#include "platform/AppInfo.h"
#include "dsp/Loudness.h"

#include <chrono>
#include <limits>
#include <tuple>
#include <complex>
#include <numeric>

namespace
{
using namespace testing;
using ampsim::Cab;

Stereo runCab (Cab& cab, const std::vector<float>& input, int bufferSize = blockSize,
               const std::function<void (size_t)>& beforeBlock = {})
{
    return runInBlocks (input, bufferSize, [&] (juce::dsp::AudioBlock<float>& block, size_t start)
    {
        if (beforeBlock)
            beforeBlock (start);
        cab.process (block, {});
    });
}

std::vector<double> scaled (std::vector<double> h, double gain)
{
    for (auto& v : h)
        v *= gain;
    return h;
}

std::vector<double> delayed (const std::vector<double>& x, size_t samples)
{
    std::vector<double> out (x.size(), 0.0);
    for (size_t i = samples; i < x.size(); ++i)
        out[i] = x[i - samples];
    return out;
}

std::vector<double> decayingNoise (int length, double tauSeconds, juce::int64 seed)
{
    const auto noise = whiteNoise (length, 1.0f, seed);
    std::vector<double> h ((size_t) length);
    for (size_t n = 0; n < h.size(); ++n)
        h[n] = noise[n] * std::exp (-(double) n / (tauSeconds * fs));
    return h;
}

juce::AudioBuffer<float> stereoBuffer (const std::vector<double>& left, const std::vector<double>& right)
{
    juce::AudioBuffer<float> b (2, (int) left.size());
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        b.setSample (0, i, (float) left[(size_t) i]);
        b.setSample (1, i, (float) right[(size_t) i]);
    }
    return b;
}

/// The magnitude response of a mono-in, one-channel-out system from its impulse response, in dB.
double magnitudeDbAt (const std::vector<float>& h, double f)
{
    std::complex<double> sum = 0.0;
    const auto w = -juce::MathConstants<double>::twoPi * f / fs;
    for (size_t n = 0; n < h.size(); ++n)
        sum += (double) h[n] * std::polar (1.0, w * (double) n);
    return 20.0 * std::log10 (std::abs (sum));
}

class CabTests final : public juce::UnitTest
{
public:
    CabTests() : juce::UnitTest ("Cab (three mics)", "ampsim") {}

    void runTest() override
    {
        const auto identity = std::vector<double> { 1.0 };

        beginTest ("with no IRs the cab passes the amp through at unity on both sides");
        {
            Cab cab;
            cab.prepare (fs, blockSize);
            const auto input = guitarDI ((int) fs);
            const auto out = runCab (cab, input);
            expectEquals (maxAbsDifference (out.left, input) + maxAbsDifference (out.right, input), 0.0);
            logMessage ("  -> no IRs: left and right equal the input exactly");
        }

        beginTest ("the full three-mic mix matches a brute-force computation (limit -100 dB)");
        {
            // Two different close mics with different level, pan, delay, and polarity, plus a true
            // stereo room with its own pre-delay. Alignment off, so the manual settings apply.
            const auto h1 = syntheticCabIR (4096);
            const auto h2 = syntheticCabIR (2048, 8.0, 7000.0);
            const auto roomL = decayingNoise (14400, 0.08, 1), roomR = decayingNoise (14400, 0.08, 2);

            Cab cab;
            const auto g1 = cab.loadCloseMicSamples (0, toBuffer (h1), fs, "mic1").gain;
            const auto g2 = cab.loadCloseMicSamples (1, toBuffer (h2), fs, "mic2").gain;
            const auto gr = cab.loadRoomSamples (stereoBuffer (roomL, roomR), fs, "room").gain;

            Cab::CloseMicSettings m1 { -3.0f, -0.5f, false, 0, false }, m2 { 2.0f, 0.7f, true, 17, false };
            Cab::RoomSettings room { -8.0f, 12.5f, false };
            cab.setCloseMic (0, m1);
            cab.setCloseMic (1, m2);
            cab.setRoom (room);
            cab.setAutoAlign (false);
            cab.prepare (fs, blockSize); // snaps every ramp: the settings apply from the first sample

            const auto input = whiteNoise ((int) fs, 0.5f, 8);
            const auto out = runCab (cab, input);

            // The expected output, in double: each close mic's convolution, delayed, times
            // sqrt(2) cos/sin of its pan angle, its level, and its polarity; the room, pre-delayed.
            const auto panGains = [] (const Cab::CloseMicSettings& m)
            {
                const auto theta = (m.pan + 1.0) * juce::MathConstants<double>::pi / 4.0;
                const auto g = std::pow (10.0, m.levelDb / 20.0) * (m.invert ? -1.0 : 1.0) * std::sqrt (2.0);
                return std::pair { g * std::cos (theta), g * std::sin (theta) };
            };
            const auto y1 = delayed (directConvolution (input, scaled (h1, g1)), (size_t) m1.delaySamples);
            const auto y2 = delayed (directConvolution (input, scaled (h2, g2)), (size_t) m2.delaySamples);
            const auto preDelay = (size_t) juce::roundToInt (room.preDelayMs * 0.001 * fs);
            const auto roomLevel = std::pow (10.0, room.levelDb / 20.0);
            const auto yrL = delayed (directConvolution (input, scaled (roomL, gr)), preDelay);
            const auto yrR = delayed (directConvolution (input, scaled (roomR, gr)), preDelay);
            const auto [l1, r1] = panGains (m1);
            const auto [l2, r2] = panGains (m2);

            std::vector<double> expectedL (input.size()), expectedR (input.size());
            for (size_t n = 0; n < input.size(); ++n)
            {
                expectedL[n] = l1 * y1[n] + l2 * y2[n] + roomLevel * yrL[n];
                expectedR[n] = r1 * y1[n] + r2 * y2[n] + roomLevel * yrR[n];
            }

            const auto errorL = relativeErrorDb (out.left, expectedL);
            const auto errorR = relativeErrorDb (out.right, expectedR);
            expectLessThan (errorL, -100.0);
            expectLessThan (errorR, -100.0);
            logMessage ("  -> 2 close mics (levels -3/+2 dB, pans -0.5/+0.7, mic 2 delayed 17 samples and inverted) and a "
                        "true-stereo room (-8 dB, 12.5 ms pre-delay): left " + dB (errorL) + ", right " + dB (errorR));

            // Same mix at other buffer sizes.
            juce::StringArray sizes;
            for (auto size : { 1, 7, 64, 512 })
            {
                Cab again;
                again.loadCloseMicSamples (0, toBuffer (h1), fs, "mic1");
                again.loadCloseMicSamples (1, toBuffer (h2), fs, "mic2");
                again.loadRoomSamples (stereoBuffer (roomL, roomR), fs, "room");
                again.setCloseMic (0, m1);
                again.setCloseMic (1, m2);
                again.setRoom (room);
                again.setAutoAlign (false);
                again.prepare (fs, 512);
                const auto o = runCab (again, input, size);
                const auto e = std::max (relativeErrorDb (o.left, expectedL), relativeErrorDb (o.right, expectedR));
                expectLessThan (e, -100.0, "buffer size " + juce::String (size));
                sizes.add (juce::String (size) + ": " + dB (e));
            }
            logMessage ("  -> the same mix at buffer sizes " + sizes.joinIntoString (", "));
        }

        beginTest ("zero latency: an impulse comes out at sample 0");
        {
            Cab cab;
            cab.loadCloseMicSamples (0, toBuffer (syntheticCabIR (4096)), fs, "mic1");
            cab.prepare (fs, blockSize);
            std::vector<float> impulse (4096, 0.0f);
            impulse[0] = 1.0f;
            const auto out = runCab (cab, impulse);
            expectGreaterThan (std::abs (out.left[0]), 1.0e-3f);
            expectEquals (cab.latencySamples(), 0);
            logMessage ("  -> first output sample " + juce::String (out.left[0], 6) + " at index 0; reported latency " + juce::String (cab.latencySamples()));
        }

        beginTest ("auto alignment recovers a planted offset and polarity exactly");
        {
            // Mic 2: the same IR shape as mic 1, 23 samples later, inverted, at -2 dB.
            const auto base = syntheticCabIR (4096);
            Cab cab;
            cab.loadCloseMicSamples (0, toBuffer (delayed (base, 5)), fs, "mic1");
            cab.loadCloseMicSamples (1, toBuffer (scaled (delayed (base, 5 + 23), -0.8)), fs, "mic2");
            const auto a = cab.getAlignment();

            expect (a.valid && a.invertMic2);
            expectEquals (a.delayMic1, 23);
            expectEquals (a.delayMic2, 0);
            expectWithinAbsoluteError (a.correlation, 1.0f, 1.0e-3f);
            logMessage ("  -> same shape, 23 samples later, inverted: found mic 1 +" + juce::String (a.delayMic1) + ", mic 2 +"
                        + juce::String (a.delayMic2) + ", mic 2 inverted: " + (a.invertMic2 ? "yes" : "no") + " (match " + juce::String (a.correlation, 4) + ")");

            // Mic 2 arriving earlier instead: mic 2 is the one delayed.
            Cab early;
            early.loadCloseMicSamples (0, toBuffer (delayed (base, 40)), fs, "mic1");
            early.loadCloseMicSamples (1, toBuffer (delayed (base, 10)), fs, "mic2");
            expectEquals (early.getAlignment().delayMic2, 30);
            expectEquals (early.getAlignment().delayMic1, 0);
            expect (! early.getAlignment().invertMic2);
            logMessage ("  -> mic 2 arriving 30 samples early: mic 2 delayed " + juce::String (early.getAlignment().delayMic2) + ", no inversion");
        }

        beginTest ("auto alignment on two differently coloured mics removes the comb filter");
        {
            // Mic 2: a brighter IR, 23 samples later, inverted. Different filtering means different phase
            // delay, so the best-matching lag can be a sample off the planted 23; the summed response
            // is what matters.
            const auto h1 = delayed (syntheticCabIR (4096), 5);
            const auto h2 = scaled (delayed (syntheticCabIR (4096, 6.0, 6000.0), 5 + 23), -0.8);

            Cab aligned, unaligned;
            for (auto* cab : { &aligned, &unaligned })
            {
                cab->loadCloseMicSamples (0, toBuffer (h1), fs, "mic1");
                cab->loadCloseMicSamples (1, toBuffer (h2), fs, "mic2");
            }
            unaligned.setAutoAlign (false);

            const auto a = aligned.getAlignment();
            expect (a.valid && a.invertMic2);
            expectWithinAbsoluteError (a.delayMic1, 23, 1);

            const auto notchDepth = [] (Cab& cab)
            {
                cab.prepare (fs, blockSize);
                std::vector<float> impulse (8192, 0.0f);
                impulse[0] = 1.0f;
                const auto h = runCab (cab, impulse).left;
                double lo = 1.0e9, sum = 0.0;
                int count = 0;
                for (double f = 200.0; f <= 4000.0; f *= 1.01)
                {
                    const auto m = magnitudeDbAt (h, f);
                    lo = std::min (lo, m);
                    sum += m;
                    ++count;
                }
                return lo - sum / count; // the deepest dip relative to the average level
            };

            const auto alignedDip = notchDepth (aligned);
            const auto unalignedDip = notchDepth (unaligned);
            expectGreaterThan (alignedDip, -6.0);
            expectLessThan (unalignedDip, -15.0);
            logMessage ("  -> planted 23 samples late and inverted; found mic 1 +" + juce::String (a.delayMic1) + ", mic 2 inverted (match "
                        + juce::String (a.correlation, 3) + ")");
            logMessage ("  -> deepest dip in the summed response, 200 Hz to 4 kHz: " + juce::String (alignedDip, 1)
                        + " dB aligned vs. " + juce::String (unalignedDip, 1) + " dB unaligned (comb filtering)");
        }

        beginTest ("constant-power pan: unity at centre, +3 dB at the sides, loudness unchanged");
        {
            juce::StringArray results;
            for (auto pan : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
            {
                Cab cab;
                cab.loadCloseMicSamples (0, toBuffer (identity), fs, "identity");
                cab.setCloseMic (0, { 0.0f, pan, false, 0, false });
                cab.prepare (fs, blockSize);
                std::vector<float> impulse (blockSize, 0.0f);
                impulse[0] = 1.0f;
                const auto out = runCab (cab, impulse);
                const auto power = out.left[0] * out.left[0] + out.right[0] * out.right[0];
                expectWithinAbsoluteError (power, 2.0f, 1.0e-4f);
                results.add (juce::String (pan, 1) + ": L " + juce::String (out.left[0], 4) + " R " + juce::String (out.right[0], 4));
            }
            logMessage ("  -> pan position: left and right gains (L^2 + R^2 = 2 throughout): " + results.joinIntoString ("; "));
        }

        beginTest ("mic delay is exact, and changing it crossfades instead of clicking");
        {
            Cab cab;
            cab.loadCloseMicSamples (0, toBuffer (identity), fs, "identity");
            cab.setAutoAlign (false);
            cab.setCloseMic (0, { 0.0f, 0.0f, false, 37, false });
            cab.prepare (fs, blockSize);
            std::vector<float> impulse (blockSize, 0.0f);
            impulse[0] = 1.0f;
            const auto out = runCab (cab, impulse);
            const auto peakAt = std::distance (out.left.begin(), std::max_element (out.left.begin(), out.left.end()));
            expectEquals ((int) peakAt, 37);

            const auto signal = sine (330.0, 0.5, (int) fs);
            const size_t changeAt = 100 * blockSize;
            Cab moving;
            moving.loadCloseMicSamples (0, toBuffer (identity), fs, "identity");
            moving.setAutoAlign (false);
            moving.setCloseMic (0, { 0.0f, 0.0f, false, 37, false });
            moving.prepare (fs, blockSize);
            const auto o = runCab (moving, signal, blockSize, [&] (size_t start)
            {
                if (start == changeAt)
                    moving.setCloseMic (0, { 0.0f, 0.0f, false, 150, false });
            }).left;
            const auto steady = maxStep (o, 1000, changeAt);
            const auto during = maxStep (o, changeAt, changeAt + 1000);
            expectLessThan (during, steady * 1.1);
            logMessage ("  -> delay 37: the impulse comes out at sample " + juce::String ((int) peakAt) + "; changing 37 -> 150 mid-note: largest step "
                        + juce::String (during, 4) + " vs. " + juce::String (steady, 4) + " steady");
        }

        beginTest ("polarity and mute ramp over 20 ms instead of clicking");
        {
            const auto signal = sine (330.0, 0.5, (int) fs);
            const size_t flipAt = 100 * blockSize, muteAt = 200 * blockSize;
            Cab cab;
            cab.loadCloseMicSamples (0, toBuffer (identity), fs, "identity");
            cab.setAutoAlign (false);
            cab.prepare (fs, blockSize);
            const auto o = runCab (cab, signal, blockSize, [&] (size_t start)
            {
                if (start == flipAt) cab.setCloseMic (0, { 0.0f, 0.0f, true, 0, false });
                if (start == muteAt) cab.setCloseMic (0, { 0.0f, 0.0f, true, 0, true });
            }).left;

            const auto ramp = (size_t) juce::roundToInt (Cab::gainRampSeconds * fs);
            const auto steady = maxStep (o, 0, flipAt);
            const auto during = maxStep (o, flipAt, flipAt + ramp + 10);
            double flippedError = 0.0;
            for (size_t n = flipAt + ramp + 1; n < muteAt; ++n)
                flippedError = std::max (flippedError, std::abs ((double) o[n] + signal[n]));
            const auto afterMute = rms (o.data() + muteAt + ramp + 1, 4800);

            expectLessThan (during, steady * 1.1);
            expectLessThan (flippedError, 1.0e-5);
            expectEquals (afterMute, 0.0);
            logMessage ("  -> polarity flip: largest step " + juce::String (during, 4) + " vs. " + juce::String (steady, 4)
                        + " steady, then exactly -input (max error " + juce::String (flippedError, 7) + "); muted: silence");
        }

        beginTest ("the room mic is true stereo with an exact pre-delay");
        {
            // Left IR: an impulse at 0. Right IR: an impulse at 100, half as loud.
            std::vector<double> left (200, 0.0), right (200, 0.0);
            left[0] = 1.0;
            right[100] = 0.5;
            Cab cab;
            cab.loadRoomSamples (stereoBuffer (left, right), fs, "room");
            cab.setRoom ({ 0.0f, 20.0f, false });
            cab.prepare (fs, blockSize);
            std::vector<float> impulse (4096, 0.0f);
            impulse[0] = 1.0f;
            const auto out = runCab (cab, impulse);
            const auto peakL = std::distance (out.left.begin(), std::max_element (out.left.begin(), out.left.end()));
            const auto peakR = std::distance (out.right.begin(), std::max_element (out.right.begin(), out.right.end()));
            const auto preDelay = juce::roundToInt (0.020 * fs);

            expectEquals ((int) peakL, preDelay);
            expectEquals ((int) peakR, preDelay + 100);
            expectWithinAbsoluteError (out.right[(size_t) peakR] / out.left[(size_t) peakL], 0.5f, 1.0e-4f);
            logMessage ("  -> 20 ms pre-delay: left impulse at sample " + juce::String ((int) peakL) + " (expected " + juce::String (preDelay)
                        + "), right at " + juce::String ((int) peakR) + " (expected " + juce::String (preDelay + 100) + "), right/left "
                        + juce::String (out.right[(size_t) peakR] / out.left[(size_t) peakL], 4));
        }

        beginTest ("low and high cuts are 2nd- and 4th-order Butterworth");
        {
            juce::StringArray results;
            struct Case { bool low; float hz; Cab::Slope slope; };
            for (const auto& c : { Case { true, 100.0f, Cab::Slope::db12 }, Case { true, 100.0f, Cab::Slope::db24 },
                                   Case { false, 5000.0f, Cab::Slope::db12 }, Case { false, 5000.0f, Cab::Slope::db24 } })
            {
                Cab cab; // no IRs: the passthrough, so only the cut shapes the response
                Cab::CutSettings cuts;
                cuts.lowCutOn = c.low;
                cuts.lowCutHz = c.hz;
                cuts.lowCutSlope = c.slope;
                cuts.highCutOn = ! c.low;
                cuts.highCutHz = c.hz;
                cuts.highCutSlope = c.slope;
                cab.setCuts (cuts);
                cab.prepare (fs, blockSize);

                std::vector<float> impulse (1 << 15, 0.0f);
                impulse[0] = 1.0f;
                const auto h = runCab (cab, impulse).left;
                const auto atCutoff = magnitudeDbAt (h, c.hz);
                const auto probe = c.low ? c.hz / 2.0 : c.hz * 2.0; // an octave into the stopband
                const auto octaveAway = magnitudeDbAt (h, probe);

                // A Butterworth filter of order N: |H|^2 = 1 / (1 + w^(2N)) for a low-pass and with
                // 1/w for a high-pass, w = frequency over cutoff. That's -3.01 dB at the cutoff for any
                // N. The bilinear transform maps the digital frequency f to the analog
                // w = tan(pi f / fs) / tan(pi fc / fs), so that's where the target is evaluated.
                const auto order = c.slope == Cab::Slope::db12 ? 2.0 : 4.0;
                const auto w = std::tan (juce::MathConstants<double>::pi * probe / fs) / std::tan (juce::MathConstants<double>::pi * c.hz / fs);
                const auto expected = -10.0 * std::log10 (1.0 + std::pow (c.low ? 1.0 / w : w, 2.0 * order));
                expectWithinAbsoluteError (atCutoff, -3.0103, 0.01);
                expectWithinAbsoluteError (octaveAway, expected, 0.01);
                results.add (juce::String (c.low ? "low" : "high") + " cut " + juce::String (c.hz, 0) + " Hz "
                             + (c.slope == Cab::Slope::db12 ? "12" : "24") + " dB/oct: " + juce::String (atCutoff, 3) + " dB at fc, "
                             + juce::String (octaveAway, 2) + " dB an octave out (Butterworth target " + juce::String (expected, 2) + ")");
            }
            logMessage ("  -> " + results.joinIntoString ("; "));
        }

        beginTest ("switching a cut on and off crossfades instead of clicking");
        {
            const auto signal = sine (70.0, 0.5, (int) fs);
            const size_t onAt = 100 * blockSize, offAt = 200 * blockSize;
            Cab cab;
            cab.prepare (fs, blockSize);
            Cab::CutSettings cuts;
            cuts.lowCutHz = 200.0f;
            const auto o = runCab (cab, signal, blockSize, [&] (size_t start)
            {
                if (start == onAt) { cuts.lowCutOn = true; cab.setCuts (cuts); }
                if (start == offAt) { cuts.lowCutOn = false; cab.setCuts (cuts); }
            }).left;
            const auto steady = maxStep (o, 0, onAt);
            const auto during = std::max (maxStep (o, onAt, onAt + 1000), maxStep (o, offAt, offAt + 1000));
            expectLessThan (during, steady * 1.1);
            logMessage ("  -> 200 Hz low cut on and off under a 70 Hz note: largest step " + juce::String (during, 4)
                        + " vs. " + juce::String (steady, 4) + " steady");
        }

        beginTest ("an IR loaded into an empty mic fades in only once it's really running, so raw amp fizz never leaks");
        {
            // Mic 1 plays a dark cab; mic 2 is empty. Loading the same IR into mic 2 (at -6 dB) mid-stream:
            // JUCE starts mic 2 from a one-sample identity engine, so until its real engine is in and
            // crossfaded, mic 2's output is the raw, unfiltered signal. The cab must not mix any of it.
            // Compared block by block against a cab that only ever has mic 1.
            const auto dark = syntheticCabIR (4096, 0.0, 1500.0);
            Cab cab, reference;
            cab.loadCloseMicSamples (0, toBuffer (dark), fs, "dark1");
            cab.setCloseMic (1, { -6.0f, 0.0f, false, 0, false });
            cab.prepare (fs, blockSize);
            reference.loadCloseMicSamples (0, toBuffer (dark), fs, "dark1");
            reference.prepare (fs, blockSize);

            const auto noise = whiteNoise ((int) (1.5 * fs), 0.5f, 12);
            const size_t loadBlock = 50;
            juce::AudioBuffer<float> out (2, blockSize), ref (2, blockSize);
            int readyBlock = -1, firstMixedBlock = -1;
            double worstBeforeFade = 0.0, worstAfterFade = 0.0;
            const auto settleBlocks = (int) std::ceil (0.060 * fs / blockSize);
            const auto fadeBlocks = (int) std::ceil (Cab::gainRampSeconds * fs / blockSize);

            for (size_t b = 0; (b + 1) * blockSize <= noise.size(); ++b)
            {
                if (b == loadBlock)
                    cab.loadCloseMicSamples (1, toBuffer (dark), fs, "dark2");
                if (b >= loadBlock && b < loadBlock + 40)
                    juce::Thread::sleep (2); // give JUCE's background thread real time to build the engine

                out.copyFrom (0, 0, noise.data() + b * blockSize, blockSize);
                ref.copyFrom (0, 0, noise.data() + b * blockSize, blockSize);
                cab.process (juce::dsp::AudioBlock<float> (out), {});
                reference.process (juce::dsp::AudioBlock<float> (ref), {});

                if (readyBlock < 0 && cab.closeMic (1).isEngineReady())
                    readyBlock = (int) b;

                double difference = 0.0, ratioError = 0.0;
                for (int i = 0; i < blockSize; ++i)
                {
                    difference = std::max (difference, (double) std::abs (out.getSample (0, i) - ref.getSample (0, i)));
                    ratioError = std::max (ratioError, (double) std::abs (out.getSample (0, i) - 1.5012f * ref.getSample (0, i)));
                }

                if (firstMixedBlock < 0 && difference > 1.0e-7)
                    firstMixedBlock = (int) b;

                if (firstMixedBlock < 0)
                    worstBeforeFade = std::max (worstBeforeFade, difference);
                else if ((int) b > firstMixedBlock + fadeBlocks)
                    worstAfterFade = std::max (worstAfterFade, ratioError);
            }

            expect (readyBlock >= (int) loadBlock, "mic 2's engine never became ready");
            expectGreaterOrEqual (firstMixedBlock, readyBlock + settleBlocks - 1);
            expectEquals (worstBeforeFade, 0.0);
            expectLessThan (worstAfterFade, 1.0e-4);
            logMessage ("  -> IR loaded at block " + juce::String ((int) loadBlock) + ", JUCE's engine ready at block " + juce::String (readyBlock)
                        + ", mic 2 first mixed at block " + juce::String (firstMixedBlock) + " (after the 60 ms settle); until then the output "
                        "equals the mic-1-only cab exactly (max difference " + juce::String (worstBeforeFade) + ")");
            logMessage ("  -> after mic 2's 20 ms fade-in the output is 1.5012 x mic 1 alone (-6 dB mic added coherently, +3.53 dB), max error "
                        + juce::String (worstAfterFade, 7));
        }

        beginTest ("CPU: two 500 ms close mics plus a 1 s stereo room, 128-sample buffers");
        {
            Cab cab;
            cab.loadCloseMicSamples (0, toBuffer (decayingNoise (24000, 0.05, 21)), fs, "close1");
            cab.loadCloseMicSamples (1, toBuffer (decayingNoise (24000, 0.05, 22)), fs, "close2");
            cab.loadRoomSamples (stereoBuffer (decayingNoise (48000, 0.3, 23), decayingNoise (48000, 0.3, 24)), fs, "room");
            Cab::CutSettings cuts;
            cuts.lowCutOn = cuts.highCutOn = true;
            cab.setCuts (cuts);
            cab.prepare (fs, blockSize);

            const auto input = guitarDI ((int) (10.0 * fs));
            juce::AudioBuffer<float> buffer (2, blockSize);
            std::vector<double> micros;
            for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
            {
                buffer.copyFrom (0, 0, input.data() + start, blockSize);
                const auto t0 = std::chrono::steady_clock::now();
                cab.process (juce::dsp::AudioBlock<float> (buffer), {});
                micros.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
            }

            std::sort (micros.begin(), micros.end());
            const auto mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
            const auto p99 = micros[(size_t) (0.99 * (double) (micros.size() - 1))];
            expectLessThan (mean, 0.25 * deadlineMicros);
            logMessage ("  -> mean " + juce::String (mean, 1) + " us (" + juce::String (100.0 * mean / deadlineMicros, 1)
                        + "% of the deadline), p99 " + juce::String (p99, 1) + " us, worst " + juce::String (micros.back(), 1) + " us");
        }

        beginTest ("CPU: the longest bundled IR (1 s) in both close mics, 128-sample buffers, against the shortest one and against no IR");
        {
            // The bundled IRs (content/irs, copied next to this binary as the app carries them): the
            // longest and the shortest by sample count.
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            juce::File longest, shortest;
            juce::int64 longestLength = 0, shortestLength = std::numeric_limits<juce::int64>::max();
            for (const auto& entry : juce::RangedDirectoryIterator (platform::factoryContentFolder().getChildFile ("irs"), true, "*.wav"))
                if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (entry.getFile())); reader != nullptr)
                {
                    if (reader->lengthInSamples > longestLength)
                        std::tie (longest, longestLength) = std::make_pair (entry.getFile(), reader->lengthInSamples);
                    if (reader->lengthInSamples < shortestLength)
                        std::tie (shortest, shortestLength) = std::make_pair (entry.getFile(), reader->lengthInSamples);
                }
            expect (longest.existsAsFile() && shortest.existsAsFile());

            const auto input = guitarDI ((int) (10.0 * fs));
            struct Result { double mean = 0.0, p99 = 0.0, worst = 0.0; rtcheck::Counts counts; };
            const auto time = [&] (const juce::File& ir)
            {
                Cab cab;
                if (ir != juce::File())
                {
                    expect (cab.loadCloseMic (0, ir).ok);
                    expect (cab.loadCloseMic (1, ir).ok);
                }
                cab.prepare (fs, blockSize);
                juce::AudioBuffer<float> buffer (2, blockSize);
                // JUCE builds each convolution engine on its own background thread: play until both close
                // mics run their real engines (not the one-sample placeholder), then time 10 s.
                for (int i = 0; i < 2000 && ir != juce::File() && ! (cab.closeMic (0).isEngineReady() && cab.closeMic (1).isEngineReady()); ++i)
                {
                    buffer.clear();
                    cab.process (juce::dsp::AudioBlock<float> (buffer), {});
                    juce::Thread::sleep (1);
                }
                std::vector<double> micros;
                micros.reserve (input.size() / blockSize + 1);
                Result r;
                for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
                {
                    buffer.copyFrom (0, 0, input.data() + start, blockSize);
                    rtcheck::begin();
                    const auto t0 = std::chrono::steady_clock::now();
                    cab.process (juce::dsp::AudioBlock<float> (buffer), {});
                    const auto t1 = std::chrono::steady_clock::now();
                    r.counts += rtcheck::end();
                    micros.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());
                }
                std::sort (micros.begin(), micros.end());
                r.mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
                r.p99 = micros[(size_t) (0.99 * (double) (micros.size() - 1))];
                r.worst = micros.back();
                return r;
            };
            const auto none = time ({}), shortOne = time (shortest), longOne = time (longest);
            for (const auto* r : { &none, &shortOne, &longOne })
            {
                expectEquals (r->counts.allocations, 0L);
                expectEquals (r->counts.frees, 0L);
                expectEquals (r->counts.blockingLocks, 0L);
            }
            // The verdict for bundling: two 1 s close mics cost little next to the 2667 us deadline.
            expectLessThan (longOne.mean, 0.05 * deadlineMicros);
            const auto describe = [] (const Result& r)
            {
                return "mean " + juce::String (r.mean, 1) + " us (" + juce::String (100.0 * r.mean / deadlineMicros, 2) + "% of the deadline), p99 "
                       + juce::String (r.p99, 1) + " us, worst " + juce::String (r.worst, 1) + " us";
            };
            logMessage ("  -> no IR: " + describe (none));
            logMessage ("  -> \"" + shortest.getFileNameWithoutExtension() + "\" (" + juce::String (shortestLength) + " samples) in both close mics: " + describe (shortOne));
            logMessage ("  -> \"" + longest.getFileNameWithoutExtension() + "\" (" + juce::String (longestLength) + " samples) in both close mics: " + describe (longOne));
            logMessage ("  -> the 1 s pair costs " + juce::String (longOne.mean - shortOne.mean, 1) + " us more per buffer than the shortest pair; audio thread in all three: "
                        + juce::String (none.counts.allocations + shortOne.counts.allocations + longOne.counts.allocations) + " allocations, "
                        + juce::String (none.counts.frees + shortOne.counts.frees + longOne.counts.frees) + " frees, "
                        + juce::String (none.counts.blockingLocks + shortOne.counts.blockingLocks + longOne.counts.blockingLocks) + " blocking locks");
        }
    }
};

CabTests cabTests;
} // namespace
