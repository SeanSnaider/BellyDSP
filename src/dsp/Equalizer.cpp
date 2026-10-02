#include "Equalizer.h"

#include <cmath>

namespace ampsim
{

namespace
{
constexpr int numBands = Equalizer::numGraphicBands;
constexpr int numDesignPoints = 2 * numBands - 1; // the centres plus the midpoints between them
constexpr double prototypeGainDb = 17.0;
constexpr double midpointWeight = 0.5;

using Matrix = std::array<std::array<double, numBands>, numDesignPoints>;
using Vector = std::array<double, numBands>;

/// The SVF bell's magnitude in dB at the warped frequency ratio w = tan(pi f / fs) / tan(pi fc / fs).
/// H(s) = (s^2 + s A/Q + 1) / (s^2 + s/(A Q) + 1) at s = jw, so with r = 1 - w^2
///     |H|^2 = (r^2 + (w A / Q)^2) / (r^2 + (w / (A Q))^2),   A = 10^(gain/40).
double bellDb (double w, double q, double gainDb)
{
    if (std::abs (gainDb) < 1.0e-12)
        return 0.0;

    const auto a = std::pow (10.0, gainDb / 40.0);
    const auto r = 1.0 - w * w;
    const auto num = r * r + (w * a / q) * (w * a / q);
    const auto den = r * r + (w / (a * q)) * (w / (a * q));
    return 10.0 * std::log10 (num / den);
}

/// The warped frequency ratios of every design point against every band centre.
std::array<Vector, numDesignPoints> warpedRatios (double sampleRate)
{
    const auto pi = juce::MathConstants<double>::pi;
    std::array<Vector, numDesignPoints> w {};
    for (int k = 0; k < numDesignPoints; ++k)
    {
        const auto f = k % 2 == 0 ? Equalizer::graphicCentres[(size_t) k / 2]
                                  : std::sqrt (Equalizer::graphicCentres[(size_t) k / 2] * Equalizer::graphicCentres[(size_t) k / 2 + 1]);
        for (int m = 0; m < numBands; ++m)
            w[(size_t) k][(size_t) m] = std::tan (pi * f / sampleRate) / std::tan (pi * Equalizer::graphicCentres[(size_t) m] / sampleRate);
    }
    return w;
}

/// The weighted least-squares solution of B g = t: the normal equations (B^T W^2 B) g = B^T W^2 t,
/// solved by Gaussian elimination with partial pivoting.
Vector solveWeighted (const Matrix& b, const std::array<double, numDesignPoints>& t)
{
    std::array<std::array<double, numBands + 1>, numBands> m {}; // augmented [B^T W^2 B | B^T W^2 t]

    for (int i = 0; i < numBands; ++i)
    {
        for (int k = 0; k < numDesignPoints; ++k)
        {
            const auto w2 = k % 2 == 0 ? 1.0 : midpointWeight * midpointWeight;
            for (int j = 0; j < numBands; ++j)
                m[(size_t) i][(size_t) j] += w2 * b[(size_t) k][(size_t) i] * b[(size_t) k][(size_t) j];
            m[(size_t) i][numBands] += w2 * b[(size_t) k][(size_t) i] * t[(size_t) k];
        }
    }

    for (int col = 0; col < numBands; ++col)
    {
        int pivot = col;
        for (int row = col + 1; row < numBands; ++row)
            if (std::abs (m[(size_t) row][(size_t) col]) > std::abs (m[(size_t) pivot][(size_t) col]))
                pivot = row;
        std::swap (m[(size_t) col], m[(size_t) pivot]);

        for (int row = col + 1; row < numBands; ++row)
        {
            const auto factor = m[(size_t) row][(size_t) col] / m[(size_t) col][(size_t) col];
            for (int j = col; j <= numBands; ++j)
                m[(size_t) row][(size_t) j] -= factor * m[(size_t) col][(size_t) j];
        }
    }

    Vector g {};
    for (int row = numBands - 1; row >= 0; --row)
    {
        auto sum = m[(size_t) row][numBands];
        for (int j = row + 1; j < numBands; ++j)
            sum -= m[(size_t) row][(size_t) j] * g[(size_t) j];
        g[(size_t) row] = sum / m[(size_t) row][(size_t) row];
    }
    return g;
}

Svf::Type svfType (Equalizer::BandType type)
{
    switch (type)
    {
        case Equalizer::BandType::lowShelf:  return Svf::Type::lowShelf;
        case Equalizer::BandType::highShelf: return Svf::Type::highShelf;
        case Equalizer::BandType::notch:     return Svf::Type::notch;
        case Equalizer::BandType::peak:      break;
    }
    return Svf::Type::peak;
}
} // namespace

// ---- Design (any thread) ----------------------------------------------------------------------

std::array<double, Equalizer::numGraphicBands> Equalizer::designGraphic (const std::array<double, numGraphicBands>& sliderDb, double sampleRate)
{
    const auto w = warpedRatios (sampleRate);

    // Targets: the sliders at the centres, the average of the two neighbours at each midpoint.
    std::array<double, numDesignPoints> t {};
    for (int k = 0; k < numDesignPoints; ++k)
        t[(size_t) k] = k % 2 == 0 ? sliderDb[(size_t) k / 2] : 0.5 * (sliderDb[(size_t) k / 2] + sliderDb[(size_t) k / 2 + 1]);

    // Interaction matrix for given band gains: band m's dB response at point k per dB of its gain.
    // A band whose gain is (nearly) zero uses the prototype gain, so its column stays meaningful.
    const auto interaction = [&] (const Vector& gains)
    {
        Matrix b {};
        for (int m = 0; m < numBands; ++m)
        {
            const auto g = std::abs (gains[(size_t) m]) > 1.0e-3 ? gains[(size_t) m] : prototypeGainDb;
            for (int k = 0; k < numDesignPoints; ++k)
                b[(size_t) k][(size_t) m] = bellDb (w[(size_t) k][(size_t) m], graphicQ, g) / g;
        }
        return b;
    };

    Vector prototype {};
    prototype.fill (prototypeGainDb);
    const auto firstPass = solveWeighted (interaction (prototype), t);
    return solveWeighted (interaction (firstPass), t);
}

Svf::Coefficients Equalizer::designBand (BandType type, double frequency, double gainDb, double q, double sampleRate)
{
    return Svf::design (svfType (type), frequency, q, gainDb, sampleRate);
}

double Equalizer::responseDb (const Settings& settings, double f, double sampleRate)
{
    return responseDb (settings, std::vector<double> { f }, sampleRate).front();
}

std::vector<double> Equalizer::responseDb (const Settings& settings, const std::vector<double>& frequencies, double sampleRate)
{
    std::vector<double> db (frequencies.size(), 0.0);
    const auto pi = juce::MathConstants<double>::pi;

    if (settings.mode == Mode::graphic)
    {
        std::array<double, numGraphicBands> sliders {};
        for (size_t m = 0; m < sliders.size(); ++m)
            sliders[m] = juce::jlimit (-sliderRangeDb, sliderRangeDb, (double) settings.sliders[m]);
        const auto gains = designGraphic (sliders, sampleRate);
        for (size_t i = 0; i < frequencies.size(); ++i)
            for (size_t m = 0; m < gains.size(); ++m)
                db[i] += bellDb (std::tan (pi * frequencies[i] / sampleRate) / std::tan (pi * graphicCentres[m] / sampleRate), graphicQ, gains[m]);
    }
    else
    {
        for (const auto& band : settings.bands)
        {
            const auto c = designBand (band.type, band.frequency, band.gainDb, band.q, sampleRate);
            for (size_t i = 0; i < frequencies.size(); ++i)
                db[i] += 20.0 * std::log10 (std::abs (Svf::responseAt (c, frequencies[i], sampleRate)));
        }
    }

    for (size_t i = 0; i < frequencies.size(); ++i)
    {
        if (settings.lowCut.on)
            db[i] += CutFilter::responseDb (CutFilter::Kind::lowCut, settings.lowCut.slope, settings.lowCut.frequency, frequencies[i], sampleRate);
        if (settings.highCut.on)
            db[i] += CutFilter::responseDb (CutFilter::Kind::highCut, settings.highCut.slope, settings.highCut.frequency, frequencies[i], sampleRate);
    }

    return db;
}

// ---- Settings (audio thread) ------------------------------------------------------------------

void Equalizer::setSettings (const Settings& settings)
{
    target = settings;
    applyTargets (false);
}

void Equalizer::applyTargets (bool snap)
{
    // Mode: crossfade to the other bank, which starts clean (it hasn't been running).
    if (target.mode != mode)
    {
        mode = target.mode;
        if (mode == Mode::graphic)
        {
            for (auto& channel : graphicFilters)
                for (auto& f : channel)
                    f.reset();
            for (auto& g : graphicGain)
                g.setCurrentAndTargetValue (g.getTargetValue());
        }
        else
        {
            for (auto& band : bands)
            {
                for (auto& f : band.filters)
                    f.reset();
                band.frequency.setCurrentAndTargetValue (band.frequency.getTargetValue());
                band.gainDb.setCurrentAndTargetValue (band.gainDb.getTargetValue());
                band.q.setCurrentAndTargetValue (band.q.getTargetValue());
            }
        }
        graphicMix.setTargetValue (mode == Mode::graphic ? 1.0f : 0.0f);
    }

    // Graphic: redesign the band gains when a slider moved (about 20 us, once per change).
    if (! designValid || target.sliders != designedFor)
    {
        std::array<double, numGraphicBands> sliders {};
        for (size_t m = 0; m < sliders.size(); ++m)
            sliders[m] = juce::jlimit (-sliderRangeDb, sliderRangeDb, (double) target.sliders[m]);
        const auto gains = designGraphic (sliders, sampleRate);
        for (size_t m = 0; m < gains.size(); ++m)
        {
            if (snap)
                graphicGain[m].setCurrentAndTargetValue (gains[m]);
            else
                graphicGain[m].setTargetValue (gains[m]);
        }
        designedFor = target.sliders;
        designValid = true;
    }

    // Parametric: a changed type waits for the dip; the knobs ramp.
    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto& t = target.bands[i];
        auto& band = bands[i];
        const auto frequency = juce::jlimit (20.0, 20000.0, (double) t.frequency);
        const auto gain = juce::jlimit (-parametricRangeDb, parametricRangeDb, (double) t.gainDb);
        const auto q = juce::jlimit (0.1, 20.0, (double) t.q);

        if (snap)
        {
            band.type = t.type;
            band.frequency.setCurrentAndTargetValue (frequency);
            band.gainDb.setCurrentAndTargetValue (gain);
            band.q.setCurrentAndTargetValue (q);
            continue;
        }

        if (t.type != band.type && ! typeChangePending)
        {
            typeChangePending = true;
            wet.setTargetValue (0.0f);
        }
        band.frequency.setTargetValue (frequency);
        band.gainDb.setTargetValue (gain);
        band.q.setTargetValue (q);
    }

    lowCut.set (target.lowCut.on, target.lowCut.frequency, target.lowCut.slope);
    highCut.set (target.highCut.on, target.highCut.frequency, target.highCut.slope);
}

// ---- Processing (audio thread) ----------------------------------------------------------------

void Equalizer::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    dry.setSize (2, maxBlockSize);
    other.setSize (2, maxBlockSize);

    for (auto& g : graphicGain)
        g.reset (sampleRate, smoothingSeconds);
    for (auto& band : bands)
    {
        band.frequency.reset (sampleRate, smoothingSeconds);
        band.gainDb.reset (sampleRate, smoothingSeconds);
        band.q.reset (sampleRate, smoothingSeconds);
    }
    graphicMix.reset (sampleRate, fadeSeconds);
    wet.reset (sampleRate, fadeSeconds);

    // Start on the current settings with no ramps or fades: nothing is playing yet.
    mode = target.mode;
    graphicMix.setCurrentAndTargetValue (mode == Mode::graphic ? 1.0f : 0.0f);
    wet.setCurrentAndTargetValue (1.0f);
    typeChangePending = false;
    designValid = false;
    applyTargets (true);

    lowCut.prepare (sampleRate, maxBlockSize);
    highCut.prepare (sampleRate, maxBlockSize);
    reset();
}

void Equalizer::reset()
{
    for (auto& channel : graphicFilters)
        for (auto& f : channel)
            f.reset();
    for (auto& band : bands)
        for (auto& f : band.filters)
            f.reset();

    samplesUntilUpdate = 0;
    updateGraphicCoefficients();
    updateParametricCoefficients();
    lowCut.reset();
    highCut.reset();
}

bool Equalizer::graphicSmoothing() const
{
    for (const auto& g : graphicGain)
        if (g.isSmoothing())
            return true;
    return false;
}

bool Equalizer::parametricSmoothing() const
{
    for (const auto& band : bands)
        if (band.frequency.isSmoothing() || band.gainDb.isSmoothing() || band.q.isSmoothing())
            return true;
    return false;
}

void Equalizer::updateGraphicCoefficients()
{
    for (size_t m = 0; m < graphicGain.size(); ++m)
    {
        const auto c = Svf::design (Svf::Type::peak, graphicCentres[m], graphicQ, graphicGain[m].getCurrentValue(), sampleRate);
        for (auto& channel : graphicFilters)
            channel[m].setCoefficients (c);
    }
}

void Equalizer::updateParametricCoefficients()
{
    for (auto& band : bands)
    {
        const auto c = designBand (band.type, band.frequency.getCurrentValue(), band.gainDb.getCurrentValue(), band.q.getCurrentValue(), sampleRate);
        for (auto& f : band.filters)
            f.setCoefficients (c);
    }
}

void Equalizer::runGraphic (float* const* channels, int numChannels, int numSamples)
{
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto& filters = graphicFilters[(size_t) ch];
        auto* x = channels[ch];
        for (int n = 0; n < numSamples; ++n)
        {
            auto v = (double) x[n];
            for (auto& f : filters)
                v = f.processSample (v);
            x[n] = (float) v;
        }
    }
}

void Equalizer::runParametric (float* const* channels, int numChannels, int numSamples)
{
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* x = channels[ch];
        for (int n = 0; n < numSamples; ++n)
        {
            auto v = (double) x[n];
            for (auto& band : bands)
                v = band.filters[(size_t) ch].processSample (v);
            x[n] = (float) v;
        }
    }
}

void Equalizer::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    const auto numChannels = (stereo && block.getNumChannels() > 1) ? 2 : 1;
    const auto numSamples = (int) block.getNumSamples();
    float* const channels[2] = { block.getChannelPointer (0), numChannels > 1 ? block.getChannelPointer (1) : nullptr };
    float* const others[2] = { other.getWritePointer (0), other.getWritePointer (1) };

    // A band type change has dipped to the unprocessed signal: swap the types, then fade back in.
    if (typeChangePending && ! wet.isSmoothing())
    {
        for (size_t i = 0; i < bands.size(); ++i)
        {
            if (bands[i].type != target.bands[i].type)
            {
                bands[i].type = target.bands[i].type;
                for (auto& f : bands[i].filters)
                    f.reset();
            }
        }
        typeChangePending = false;
        wet.setTargetValue (1.0f);
        updateParametricCoefficients();
    }

    const bool dipping = wet.isSmoothing() || wet.getCurrentValue() < 1.0f;
    if (dipping)
        for (int ch = 0; ch < numChannels; ++ch)
            dry.copyFrom (ch, 0, channels[ch], numSamples);

    const bool modeFading = graphicMix.isSmoothing();
    const bool runG = modeFading || mode == Mode::graphic;
    const bool runP = modeFading || mode == Mode::parametric;

    if (modeFading)
        for (int ch = 0; ch < numChannels; ++ch)
            other.copyFrom (ch, 0, channels[ch], numSamples);

    // Work through the buffer in 32-sample intervals: at the start of each, advance the smoothers by
    // one interval and redesign whatever is moving with the value at the interval's middle (landing
    // exactly on the target when a ramp ends), as AmpTone does.
    for (int start = 0; start < numSamples;)
    {
        if (samplesUntilUpdate == 0)
        {
            if (runG && graphicSmoothing())
            {
                for (size_t m = 0; m < graphicGain.size(); ++m)
                {
                    auto& g = graphicGain[m];
                    if (! g.isSmoothing())
                        continue;
                    const auto mid = g.skip (coefficientInterval / 2);
                    g.skip (coefficientInterval / 2);
                    const auto c = Svf::design (Svf::Type::peak, graphicCentres[m], graphicQ, g.isSmoothing() ? mid : g.getTargetValue(), sampleRate);
                    for (auto& channel : graphicFilters)
                        channel[m].setCoefficients (c);
                }
            }

            if (runP && parametricSmoothing())
            {
                for (auto& band : bands)
                {
                    if (! (band.frequency.isSmoothing() || band.gainDb.isSmoothing() || band.q.isSmoothing()))
                        continue;
                    const auto value = [] (auto& smoother)
                    {
                        if (! smoother.isSmoothing())
                            return smoother.getTargetValue();
                        const auto mid = smoother.skip (coefficientInterval / 2);
                        smoother.skip (coefficientInterval / 2);
                        return smoother.isSmoothing() ? mid : smoother.getTargetValue();
                    };
                    const auto f = value (band.frequency);
                    const auto g = value (band.gainDb);
                    const auto q = value (band.q);
                    const auto c = designBand (band.type, f, g, q, sampleRate);
                    for (auto& filter : band.filters)
                        filter.setCoefficients (c);
                }
            }

            samplesUntilUpdate = coefficientInterval;
        }

        const auto len = std::min (samplesUntilUpdate, numSamples - start);
        float* const span[2] = { channels[0] + start, numChannels > 1 ? channels[1] + start : nullptr };
        float* const otherSpan[2] = { others[0] + start, others[1] + start };

        if (runG)
            runGraphic (span, numChannels, len);
        if (runP)
            runParametric (modeFading ? otherSpan : span, numChannels, len);

        start += len;
        samplesUntilUpdate -= len;
    }

    if (modeFading)
    {
        // Linear crossfade between the banks: the same signal through two EQs, strongly correlated.
        for (int n = 0; n < numSamples; ++n)
        {
            const auto m = graphicMix.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto p = others[ch][n];
                channels[ch][n] = p + m * (channels[ch][n] - p);
            }
        }
    }

    lowCut.process (channels, numChannels, numSamples);
    highCut.process (channels, numChannels, numSamples);

    if (dipping)
    {
        for (int n = 0; n < numSamples; ++n)
        {
            const auto w = wet.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto d = dry.getSample (ch, n);
                channels[ch][n] = d + w * (channels[ch][n] - d);
            }
        }
    }
}

} // namespace ampsim
