#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

namespace ampsim
{

/// A low-frequency oscillator for modulation: chorus voices, delay wobble, reverb line modulation, and
/// later the phaser and flanger (BUILD_PLAN "Shared modulated-delay engine").
///
/// The phase runs from 0 to 1 once per cycle and the output is in [-1, 1]. Shapes, all starting at 0
/// and rising, so a phase offset means the same thing for each:
///   sine      sin(2 pi phase)
///   triangle  4 phase, then 2 - 4 phase, then 4 phase - 4: straight lines through +1 at 1/4, -1 at 3/4
///   random    a new random target each cycle, glided to along a half cosine, (1 - cos(pi phase)) / 2,
///             which starts and ends with zero slope, so the wander has no corners
/// Per-voice phase offsets give the chorus its 180 and 120 degree spreads.
class Lfo
{
public:
    enum class Shape
    {
        sine,
        triangle,
        random
    };

    /// The value of the sine or triangle shape at a phase in [0, 1).
    static float shapeAt (Shape shape, double phase) noexcept
    {
        phase -= std::floor (phase);
        if (shape == Shape::triangle)
        {
            if (phase < 0.25)
                return (float) (4.0 * phase);
            if (phase < 0.75)
                return (float) (2.0 - 4.0 * phase);
            return (float) (4.0 * phase - 4.0);
        }
        return (float) std::sin (juce::MathConstants<double>::twoPi * phase);
    }

    explicit Lfo (juce::int64 seed = 1) : random (seed) {}

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        setRate (rate);
    }

    void setRate (double hz) noexcept
    {
        rate = juce::jmax (0.0, hz);
        increment = rate / sampleRate;
    }

    void setShape (Shape newShape) noexcept { shape = newShape; }

    /// Sets the phase (0 to 1) directly, for voice offsets and resets.
    void setPhase (double newPhase) noexcept { phase = newPhase - std::floor (newPhase); }
    double getPhase() const noexcept { return phase; }

    /// Restarts the random shape's sequence from a seed, so two LFOs given the same seed and phase wander
    /// identically (and one of them inverted wanders in exact antiphase). Unlike a fresh LFO, whose first
    /// cycle stays at 0, the first glide heads straight for the sequence's first target.
    void setSeed (juce::int64 seed) noexcept
    {
        random.setSeed (seed);
        from = 0.0f;
        to = random.nextFloat() * 2.0f - 1.0f;
    }

    /// The current value, then advance one sample.
    float next() noexcept
    {
        float value;
        if (shape == Shape::random)
        {
            const auto glide = 0.5 * (1.0 - std::cos (juce::MathConstants<double>::pi * phase));
            value = from + (float) glide * (to - from);
        }
        else
        {
            value = shapeAt (shape, phase);
        }

        phase += increment;
        if (phase >= 1.0)
        {
            phase -= std::floor (phase);
            from = to;
            to = random.nextFloat() * 2.0f - 1.0f;
        }
        return value;
    }

    /// Skips n samples without producing output.
    void advance (int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            next();
    }

private:
    juce::Random random;
    Shape shape = Shape::sine;
    double sampleRate = 48000.0, rate = 1.0, increment = 1.0 / 48000.0, phase = 0.0;
    float from = 0.0f, to = 0.0f;
};

} // namespace ampsim
