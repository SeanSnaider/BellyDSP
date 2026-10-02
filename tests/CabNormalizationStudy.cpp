// Which reference signal should cab IRs be loudness-matched with? BUILD_PLAN said pink noise, but
// distorted guitar through stock, dark, and bright cabs still spans over 6 LU that way. This compares
// candidate references on clean and distorted guitar, logs the worst-case spread for each, and checks
// that white noise (the one the loader uses) is still the best of them.

#include "TestHelpers.h"
#include "dsp/Loudness.h"
#include "dsp/NamAmp.h"
#include "dsp/ReferenceSignals.h"

namespace
{
using namespace testing;

class CabNormalizationStudy final : public juce::UnitTest
{
public:
    CabNormalizationStudy() : juce::UnitTest ("Cab normalization study", "ampsim") {}

    void runTest() override
    {
        beginTest ("which reference signal keeps cab swaps level on clean and distorted guitar");

        using Signal = std::vector<float>;
        const auto lufs = [] (const Signal& x) { return ampsim::loudness::integratedMono (x.data(), (int) x.size(), fs); };
        const auto convolve = [] (const Signal& x, const std::vector<double>& h)
        {
            Signal hf (h.begin(), h.end());
            return ampsim::loudness::fftConvolve (x, hf.data(), (int) hf.size());
        };

        // Cabs: stock, dark, bright.
        const std::vector<std::vector<double>> cabs { syntheticCabIR (4096), syntheticCabIR (4096, 0.0, 2500.0), syntheticCabIR (4096, 8.0, 8000.0) };

        // What reaches a cab: the clean DI (a very clean amp) and each example capture's output, on a
        // take of the riff that none of the references use.
        const auto take = ampsim::referenceGuitarDI ((int) (6.0 * fs), fs, 7);
        std::vector<std::pair<juce::String, Signal>> signals { { "clean DI", take } };
        for (auto name : { "wavenet_a1_standard", "lstm", "wavenet", "A2" })
        {
            ampsim::NamAmp amp;
            amp.loadModel (exampleModel (juce::String (name) + ".nam"), true);
            amp.prepare (fs, blockSize);
            const ampsim::BlockContext context;
            signals.push_back ({ name, runInBlocks (take, blockSize, [&] (juce::dsp::AudioBlock<float>& b, size_t)
                                                    { amp.process (b.getSubsetChannelBlock (0, 1), context); }).left });
        }

        // Candidate references.
        const auto di = ampsim::referenceGuitarDI ((int) (4.0 * fs));
        Signal driven (di.size());
        for (size_t i = 0; i < di.size(); ++i)
            driven[i] = 0.5f * std::tanh (25.0f * di[i]); // a generic high-gain amp: hard tanh clipping
        Signal blend (di);
        blend.insert (blend.end(), driven.begin(), driven.end());

        // Noise with a chosen spectral tilt, made in the frequency domain: white noise's spectrum times
        // (f / 1 kHz)^(slope / 6.02), floored at 20 Hz. 0 dB/oct is white, -3 dB/oct is pink.
        const auto tilted = [] (double slopeDbPerOctave)
        {
            constexpr int order = 18;
            const auto size = (size_t) 1 << order;
            juce::dsp::FFT fft (order);
            auto x = whiteNoise ((int) size, 0.3f, 77);
            std::vector<float> buffer (2 * size, 0.0f);
            std::copy (x.begin(), x.end(), buffer.begin());
            fft.performRealOnlyForwardTransform (buffer.data(), true);
            for (size_t k = 0; k <= size / 2; ++k)
            {
                const auto f = std::max (20.0, (double) k * fs / (double) size);
                const auto a = (float) std::pow (f / 1000.0, slopeDbPerOctave / 6.0206);
                buffer[2 * k] *= a;
                buffer[2 * k + 1] *= a;
            }
            fft.performRealOnlyInverseTransform (buffer.data());
            buffer.resize (size);
            return buffer;
        };

        const std::vector<std::pair<juce::String, Signal>> references {
            { "pink noise (Kellet filter)", pinkNoise ((int) (4.0 * fs)) },
            { "white noise (= unit energy)", whiteNoise ((int) (4.0 * fs), 0.3f, 5) },
            { "noise tilted -1 dB/oct", tilted (-1.0) },
            { "noise tilted -1.5 dB/oct", tilted (-1.5) },
            { "noise tilted -2 dB/oct", tilted (-2.0) },
            { "noise tilted -3 dB/oct (pink)", tilted (-3.0) },
            { "noise tilted +1 dB/oct", tilted (1.0) },
            { "clean reference DI", di },
            { "DI through hard tanh clipping", driven },
            { "clean DI then clipped DI", blend },
        };

        juce::String best;
        double bestWorst = 1.0e9, whiteWorst = 0.0;

        for (const auto& [refName, reference] : references)
        {
            // Normalize each cab with this reference: scale so the reference's loudness doesn't change.
            const auto refLoudness = lufs (reference);
            std::vector<double> gainsDb;
            for (const auto& h : cabs)
                gainsDb.push_back (refLoudness - lufs (convolve (reference, h)));

            juce::StringArray perSignal;
            double worst = 0.0;
            for (const auto& [sigName, x] : signals)
            {
                std::vector<double> loudness;
                for (size_t c = 0; c < cabs.size(); ++c)
                    loudness.push_back (lufs (convolve (x, cabs[c])) + gainsDb[c]);
                const auto spread = *std::max_element (loudness.begin(), loudness.end()) - *std::min_element (loudness.begin(), loudness.end());
                worst = std::max (worst, spread);
                perSignal.add (sigName + " " + juce::String (spread, 2));
            }

            logMessage ("  -> " + refName + ": worst spread " + juce::String (worst, 2) + " LU (" + perSignal.joinIntoString (", ") + ")");

            if (worst < bestWorst)
            {
                bestWorst = worst;
                best = refName;
            }

            if (refName.startsWith ("white noise"))
                whiteWorst = worst;
        }

        // The product uses white noise. If another candidate ever does better, this flags it.
        expect (best.startsWith ("white noise"), "best reference was " + best);
        logMessage ("  -> best worst-case: " + best + " at " + juce::String (bestWorst, 2) + " LU; the loader uses white noise ("
                    + juce::String (whiteWorst, 2) + " LU)");
    }
};

CabNormalizationStudy cabNormalizationStudy;
} // namespace
