#include "ReferenceSignals.h"

#include <cmath>

namespace ampsim
{

std::vector<float> referenceGuitarDI (int numSamples, double sampleRate, juce::int64 seed)
{
    struct Note
    {
        double start, length;
        std::vector<double> frequencies;
        double decay;
        double level;
    };

    const std::vector<Note> phrase {
        { 0.00, 0.12, { 82.41 }, 0.985, 0.8 },                  // palm-muted chugs on low E
        { 0.15, 0.12, { 82.41 }, 0.985, 0.8 },
        { 0.30, 0.12, { 82.41 }, 0.985, 0.8 },
        { 0.45, 0.12, { 82.41 }, 0.985, 0.8 },
        { 0.60, 0.50, { 82.41, 123.47, 164.81 }, 0.997, 0.6 },  // E5 power chord
        { 1.10, 0.20, { 196.00 }, 0.996, 0.7 },                 // G3
        { 1.30, 0.20, { 220.00 }, 0.996, 0.7 },                 // A3
        { 1.50, 0.20, { 246.94 }, 0.996, 0.7 },                 // B3
        { 1.70, 0.30, { 110.00, 164.81, 220.00 }, 0.997, 0.6 }, // A5 power chord
    };

    std::vector<double> mix ((size_t) numSamples, 0.0);
    juce::Random random (seed);
    const auto release = (int) (0.010 * sampleRate); // each note ends with a 10 ms release, not a click

    for (double phraseStart = 0.0; phraseStart * sampleRate < numSamples; phraseStart += 2.0)
    {
        for (const auto& note : phrase)
        {
            for (const auto frequency : note.frequencies)
            {
                const auto start = (int) ((phraseStart + note.start) * sampleRate);
                const auto length = (int) (note.length * sampleRate);
                const auto period = std::max (2, juce::roundToInt (sampleRate / frequency));
                std::vector<double> loop ((size_t) period);

                for (auto& v : loop)
                    v = 2.0 * random.nextDouble() - 1.0; // the pluck

                for (int i = 0; i < length && start + i < numSamples; ++i)
                {
                    const auto index = (size_t) (i % period);
                    const auto y = loop[index];
                    loop[index] = note.decay * 0.5 * (loop[index] + loop[(index + 1) % (size_t) period]);
                    const auto envelope = i > length - release ? (double) (length - i) / release : 1.0;
                    mix[(size_t) (start + i)] += note.level * y * envelope;
                }
            }
        }
    }

    double peak = 1.0e-9;
    for (auto v : mix)
        peak = std::max (peak, std::abs (v));

    std::vector<float> out ((size_t) numSamples);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = (float) (mix[i] * 0.5 / peak); // peaks at -6 dBFS, a typical DI level

    return out;
}

std::vector<float> referenceWhiteNoise (int numSamples, juce::int64 seed)
{
    juce::Random random (seed);
    std::vector<float> out ((size_t) numSamples);

    for (auto& sample : out)
        sample = 0.3f * (2.0f * random.nextFloat() - 1.0f);

    return out;
}

} // namespace ampsim
