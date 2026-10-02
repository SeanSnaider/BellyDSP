#include "TestHelpers.h"
#include "dsp/Gain.h"

namespace
{
using namespace testing;

std::vector<float> runGain (ampsim::Gain& gain, const std::vector<float>& input, std::vector<float>* right = nullptr)
{
    const ampsim::BlockContext context;
    auto out = runInBlocks (input, blockSize, [&] (juce::dsp::AudioBlock<float>& block, size_t)
    {
        gain.process (block, context);
    });

    if (right != nullptr)
        *right = out.right;

    return out.left;
}

class GainTests final : public juce::UnitTest
{
public:
    GainTests() : juce::UnitTest ("Gain", "ampsim") {}

    void runTest() override
    {
        beginTest ("steady-state gain is exactly 10^(dB/20)");
        {
            ampsim::Gain gain (false);
            gain.setGainDecibels (-6.0f);
            gain.prepare (fs, blockSize);

            const auto input = sine (440.0, 0.5, 4800);
            const auto out = runGain (gain, input);
            const auto g = std::pow (10.0, -6.0 / 20.0);

            double worst = 0.0;
            for (size_t i = 0; i < input.size(); ++i)
                worst = std::max (worst, std::abs (out[i] - input[i] * g));

            expectLessThan (worst, 1.0e-6);
            logMessage ("  -> -6 dB setting: max deviation from x * 10^(-6/20) is " + juce::String (worst, 9));
        }

        beginTest ("a +12 dB change ramps over 25 ms in a straight line in dB, with no step");
        {
            ampsim::Gain gain (false);
            gain.setGainDecibels (0.0f);
            gain.prepare (fs, blockSize);
            gain.setGainDecibels (12.0f);

            const std::vector<float> ones (2400, 1.0f); // DC input, so the output is the gain itself
            const auto g = runGain (gain, ones);
            const auto target = std::pow (10.0, 12.0 / 20.0);
            const int rampSteps = juce::roundToInt (0.025 * fs); // 1200

            int reachedAt = -1;
            for (size_t n = 0; n < g.size(); ++n)
                if (std::abs (g[n] - target) < 1.0e-5 * target) { reachedAt = (int) n; break; }

            // Multiplicative smoothing: after n+1 steps the gain in dB is 12 * (n+1) / 1200.
            double worstLineError = 0.0, largestStepDb = 0.0;
            for (int n = 0; n < rampSteps; ++n)
            {
                const auto expectedDb = 12.0 * (n + 1) / rampSteps;
                worstLineError = std::max (worstLineError, std::abs (toDb (g[(size_t) n]) - expectedDb));
                if (n > 0)
                    largestStepDb = std::max (largestStepDb, toDb (g[(size_t) n]) - toDb (g[(size_t) n - 1]));
            }

            expectEquals (reachedAt, rampSteps - 1, "ramp should land on the target after exactly 1200 samples");
            expectLessThan (worstLineError, 0.001);
            expectLessThan (largestStepDb, 0.02);
            expectGreaterThan ((double) g[0], 1.0);

            logMessage ("  -> reached +12.000 dB after " + juce::String (reachedAt + 1) + " samples ("
                        + juce::String ((reachedAt + 1) / fs * 1000.0, 2) + " ms); worst distance from a straight dB line "
                        + juce::String (worstLineError, 5) + " dB; largest single-sample change "
                        + juce::String (largestStepDb, 4) + " dB");
        }

        beginTest ("the stereo instance applies identical gain to both channels, even mid-ramp");
        {
            ampsim::Gain gain (true);
            gain.prepare (fs, blockSize);
            gain.setGainDecibels (-9.0f);

            const auto input = sine (220.0, 0.5, 4800);
            std::vector<float> right;
            const auto left = runGain (gain, input, &right);

            expectEquals (maxAbsDifference (left, right), 0.0);
            logMessage ("  -> left/right max difference across a -9 dB ramp: " + juce::String (maxAbsDifference (left, right)));
        }
    }
};

GainTests gainTests;
} // namespace
