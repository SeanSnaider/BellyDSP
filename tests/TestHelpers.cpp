#include "TestHelpers.h"

#include <cmath>

namespace testing
{

// ---- Files ----------------------------------------------------------------------------------

juce::File namDir() { return juce::File (AMPSIM_NAM_DIR); }

juce::File exampleModel (const juce::String& fileName) { return namDir().getChildFile ("example_models").getChildFile (fileName); }

juce::File exampleInputFile() { return namDir().getChildFile ("example_audio/input.wav"); }

juce::File namRenderTool() { return juce::File (AMPSIM_NAM_RENDER); }

juce::File& proofDir()
{
    static juce::File dir = juce::File::getCurrentWorkingDirectory().getChildFile ("proof");
    return dir;
}

juce::File tempDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ampsim_tests");
    dir.createDirectory();
    return dir;
}

juce::AudioBuffer<float> readWav (const juce::File& file, double* sampleRateOut)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return {};

    juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);

    if (sampleRateOut != nullptr)
        *sampleRateOut = reader->sampleRate;

    return buffer;
}

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (buffer.getNumChannels())
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    auto writer = juce::WavAudioFormat().createWriterFor (stream, options);
    return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

bool writeWav (const juce::File& file, const std::vector<float>& mono, double sampleRate)
{
    return writeWav (file, toBuffer (mono), sampleRate);
}

// ---- Signals --------------------------------------------------------------------------------

std::vector<float> sine (double frequency, double amplitude, int numSamples)
{
    std::vector<float> x ((size_t) numSamples);

    for (int n = 0; n < numSamples; ++n)
        x[(size_t) n] = (float) (amplitude * std::sin (juce::MathConstants<double>::twoPi * frequency * n / fs));

    return x;
}

std::vector<float> whiteNoise (int numSamples, float amplitude, juce::int64 seed)
{
    juce::Random random (seed);
    std::vector<float> x ((size_t) numSamples);

    for (auto& sample : x)
        sample = amplitude * (2.0f * random.nextFloat() - 1.0f);

    return x;
}

std::vector<float> exampleInput (int minimumSamples)
{
    const auto buffer = readWav (exampleInputFile());
    std::vector<float> x (buffer.getReadPointer (0), buffer.getReadPointer (0) + buffer.getNumSamples());

    if (x.empty())
        return x;

    const auto original = x;

    while ((int) x.size() < minimumSamples)
        x.insert (x.end(), original.begin(), original.end());

    return x;
}

namespace
{
/// One RBJ "Audio EQ Cookbook" biquad in direct form I, double precision. Same formulas as the
/// biquad() function in prototypes/amp_sim.py.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    static Biquad make (const juce::String& kind, double f0, double q, double gainDb = 0.0)
    {
        const auto A = std::pow (10.0, gainDb / 40.0);
        const auto w0 = juce::MathConstants<double>::twoPi * f0 / fs;
        const auto cw = std::cos (w0), sw = std::sin (w0);
        const auto alpha = sw / (2.0 * q);
        double b[3], a[3];

        if (kind == "lowpass")
        {
            b[0] = (1 - cw) / 2; b[1] = 1 - cw; b[2] = (1 - cw) / 2;
            a[0] = 1 + alpha; a[1] = -2 * cw; a[2] = 1 - alpha;
        }
        else if (kind == "highpass")
        {
            b[0] = (1 + cw) / 2; b[1] = -(1 + cw); b[2] = (1 + cw) / 2;
            a[0] = 1 + alpha; a[1] = -2 * cw; a[2] = 1 - alpha;
        }
        else // peak
        {
            b[0] = 1 + alpha * A; b[1] = -2 * cw; b[2] = 1 - alpha * A;
            a[0] = 1 + alpha / A; a[1] = -2 * cw; a[2] = 1 - alpha / A;
        }

        Biquad bq;
        bq.b0 = b[0] / a[0]; bq.b1 = b[1] / a[0]; bq.b2 = b[2] / a[0];
        bq.a1 = a[1] / a[0]; bq.a2 = a[2] / a[0];
        return bq;
    }

    double process (double x)
    {
        const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};
} // namespace

std::vector<double> syntheticCabIR (int length)
{
    std::vector<Biquad> cab {
        Biquad::make ("highpass", 90, 0.707),
        Biquad::make ("peak", 120, 1.2, 3.0),
        Biquad::make ("peak", 400, 1.0, -3.0),
        Biquad::make ("peak", 2500, 1.5, 4.0),
        Biquad::make ("lowpass", 5000, 0.707),
        Biquad::make ("lowpass", 5000, 0.707),
    };

    std::vector<double> h ((size_t) length);

    for (int n = 0; n < length; ++n)
    {
        auto v = n == 0 ? 1.0 : 0.0; // unit impulse in, impulse response out

        for (auto& stage : cab)
            v = stage.process (v);

        h[(size_t) n] = v;
    }

    return h;
}

std::vector<double> unitEnergy (const std::vector<double>& h)
{
    double energy = 0.0;
    for (auto v : h)
        energy += v * v;

    auto out = h;
    for (auto& v : out)
        v /= std::sqrt (energy);

    return out;
}

juce::AudioBuffer<float> toBuffer (const std::vector<double>& samples)
{
    juce::AudioBuffer<float> buffer (1, (int) samples.size());

    for (size_t i = 0; i < samples.size(); ++i)
        buffer.setSample (0, (int) i, (float) samples[i]);

    return buffer;
}

juce::AudioBuffer<float> toBuffer (const std::vector<float>& samples)
{
    juce::AudioBuffer<float> buffer (1, (int) samples.size());
    buffer.copyFrom (0, 0, samples.data(), (int) samples.size());
    return buffer;
}

std::vector<double> directConvolution (const std::vector<float>& x, const std::vector<double>& h)
{
    std::vector<double> y (x.size(), 0.0);

    for (size_t n = 0; n < x.size(); ++n)
    {
        double sum = 0.0;
        const auto kMax = std::min (h.size(), n + 1);

        for (size_t k = 0; k < kMax; ++k)
            sum += h[k] * (double) x[n - k];

        y[n] = sum;
    }

    return y;
}

// ---- Measurements ---------------------------------------------------------------------------

double rms (const float* x, size_t n)
{
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i)
        sum += (double) x[i] * (double) x[i];
    return n > 0 ? std::sqrt (sum / (double) n) : 0.0;
}

double rms (const std::vector<float>& x) { return rms (x.data(), x.size()); }

double toDb (double linear) { return linear > 0.0 ? 20.0 * std::log10 (linear) : -400.0; }

double maxAbsDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    double worst = 0.0;
    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
        worst = std::max (worst, std::abs ((double) a[i] - (double) b[i]));
    return worst;
}

double maxAbsDifference (const std::vector<float>& a, const std::vector<double>& b)
{
    double worst = 0.0;
    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
        worst = std::max (worst, std::abs ((double) a[i] - b[i]));
    return worst;
}

double relativeErrorDb (const std::vector<float>& actual, const std::vector<double>& expected)
{
    double errorSquares = 0.0, signalSquares = 0.0;

    for (size_t i = 0; i < std::min (actual.size(), expected.size()); ++i)
    {
        const auto e = (double) actual[i] - expected[i];
        errorSquares += e * e;
        signalSquares += expected[i] * expected[i];
    }

    if (errorSquares == 0.0)
        return -400.0; // bit-exact

    return 10.0 * std::log10 (errorSquares / signalSquares);
}

double relativeErrorDb (const std::vector<float>& actual, const std::vector<float>& expected)
{
    return relativeErrorDb (actual, std::vector<double> (expected.begin(), expected.end()));
}

double maxStep (const std::vector<float>& x, size_t start, size_t end)
{
    end = std::min (end, x.size());
    double worst = 0.0;

    for (size_t i = std::max<size_t> (start, 1); i < end; ++i)
        worst = std::max (worst, std::abs ((double) x[i] - (double) x[i - 1]));

    return worst;
}

juce::String dB (double value)
{
    return value <= -399.0 ? juce::String ("-inf dB (bit-exact)") : juce::String (value, 1) + " dB";
}

juce::String micros (double value) { return juce::String (value, 1) + " us"; }

// ---- Running audio through blocks -------------------------------------------------------------

Stereo runInBlocks (const std::vector<float>& input, int bufferSize,
                    const std::function<void (juce::dsp::AudioBlock<float>&, size_t)>& process)
{
    Stereo out;
    out.left.resize (input.size());
    out.right.resize (input.size());

    juce::AudioBuffer<float> buffer (2, bufferSize);

    for (size_t start = 0; start < input.size(); start += (size_t) bufferSize)
    {
        const auto len = std::min ((size_t) bufferSize, input.size() - start);
        buffer.copyFrom (0, 0, input.data() + start, (int) len);
        buffer.copyFrom (1, 0, input.data() + start, (int) len);

        auto block = juce::dsp::AudioBlock<float> (buffer).getSubBlock (0, len);
        process (block, start);

        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + len, out.left.begin() + (long) start);
        std::copy (buffer.getReadPointer (1), buffer.getReadPointer (1) + len, out.right.begin() + (long) start);
    }

    return out;
}

} // namespace testing
