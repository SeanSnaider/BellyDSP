// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "CutFilter.h"
#include "Svf.h"

#include <array>
#include <vector>

namespace ampsim
{

/// The EQ (BUILD_PLAN "EQ"): one block type with a pre-amp instance (mono) and a post-cab instance
/// (stereo). Each switches between two modes and has its own low and high cut.
///
/// Graphic: nine octave bands, 62.5 Hz to 16 kHz, +-12 dB. Neighbouring bands overlap, so the gains
/// actually given to the band filters come from Valimaki and Liski's accurate cascade design (see
/// designGraphic()), which makes the response land on the sliders. Prototyped and tuned in
/// prototypes/graphic_eq.py, which also writes the golden values the tests compare against.
///
/// Parametric: five bands, each a peak, low shelf, high shelf, or notch with its own frequency, gain,
/// and Q. Band 1 defaults to a low shelf and band 5 to a high shelf.
///
/// Every band is a TPT SVF (Svf.h) with double state. Knob values are smoothed per sample and the
/// coefficients redesigned every 32 samples. Switching modes crossfades the two banks over 10 ms
/// (both run during the fade); changing a band's type dips to the unprocessed signal, swaps, and fades
/// back, since a filter can't be blended with a different kind of filter.
class Equalizer : public Block
{
public:
    enum class Mode
    {
        graphic,
        parametric
    };

    enum class BandType
    {
        peak,
        lowShelf,
        highShelf,
        notch
    };

    static constexpr int numGraphicBands = 9;
    static constexpr std::array<double, numGraphicBands> graphicCentres { 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
    static constexpr double graphicQ = 0.9;
    static constexpr double sliderRangeDb = 12.0;
    static constexpr int numParametricBands = 5;
    static constexpr double parametricRangeDb = 18.0;
    static constexpr int coefficientInterval = 32;
    static constexpr double smoothingSeconds = 0.025;
    static constexpr double fadeSeconds = 0.010;

    struct Band
    {
        BandType type = BandType::peak;
        float frequency = 1000.0f;
        float gainDb = 0.0f;
        float q = 1.0f;
    };

    struct Cut
    {
        bool on = false;
        float frequency = 80.0f;
        CutFilter::Slope slope = CutFilter::Slope::db12;
    };

    /// Everything the EQ does, as plain values. The processor fills one from the parameters each
    /// buffer, and the GUI uses the same struct to draw the response curve.
    struct Settings
    {
        Mode mode = Mode::graphic;
        std::array<float, numGraphicBands> sliders {};
        std::array<Band, numParametricBands> bands { { { BandType::lowShelf, 100.0f, 0.0f, 0.7071f },
                                                       { BandType::peak, 400.0f, 0.0f, 1.0f },
                                                       { BandType::peak, 1000.0f, 0.0f, 1.0f },
                                                       { BandType::peak, 3000.0f, 0.0f, 1.0f },
                                                       { BandType::highShelf, 8000.0f, 0.0f, 0.7071f } } };
        Cut lowCut { false, 80.0f, CutFilter::Slope::db12 };
        Cut highCut { false, 12000.0f, CutFilter::Slope::db12 };
    };

    explicit Equalizer (bool isStereoBlock) : stereo (isStereoBlock) {}

    /// The graphic design: the band filter gains (dB) that make the cascade of overlapping bells land
    /// on the sliders.
    ///   1. Interaction matrix B: band m at a prototype gain of 17 dB, its dB response at each design
    ///      frequency k divided by 17. Design frequencies are the 9 centres and the 8 geometric
    ///      midpoints; targets are the sliders at the centres and neighbour averages at midpoints.
    ///   2. Weighted least squares, g = argmin sum_k w_k^2 (B g - t)_k^2, with weight 1 at the centres
    ///      and 0.5 at the midpoints (solved through the 9x9 normal equations).
    ///   3. Again with B rebuilt from each band at its own gain from step 2, since a bell's dB shape
    ///      narrows as its gain grows.
    /// V. Valimaki and J. Liski, "Accurate cascade graphic equalizer", IEEE SPL 24(2), 2017. The
    /// weighting is ours (prototypes/graphic_eq.py): the centres then land within 0.35 dB.
    /// Allocation-free; the audio thread runs it when a slider moves.
    static std::array<double, numGraphicBands> designGraphic (const std::array<double, numGraphicBands>& sliderDb, double sampleRate);

    /// The EQ's magnitude response in dB at f for these settings, including the cuts (graphic mode
    /// runs designGraphic). For tests and the GUI's response curve; any thread.
    static double responseDb (const Settings& settings, double f, double sampleRate);

    /// The same at many frequencies, designing the graphic gains once (the GUI's curve).
    static std::vector<double> responseDb (const Settings& settings, const std::vector<double>& frequencies, double sampleRate);

    /// Audio thread, once per buffer.
    void setSettings (const Settings& settings);

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return stereo; }

private:
    using Smoothed = juce::SmoothedValue<double>;
    using SmoothedRatio = juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative>;

    struct ParametricBand
    {
        BandType type = BandType::peak;
        SmoothedRatio frequency { 1000.0 };
        Smoothed gainDb { 0.0 };
        SmoothedRatio q { 1.0 };
        std::array<Svf, 2> filters;
    };

    static Svf::Coefficients designBand (BandType type, double frequency, double gainDb, double q, double sampleRate);
    void applyTargets (bool snap);
    void updateGraphicCoefficients();
    void updateParametricCoefficients();
    bool graphicSmoothing() const;
    bool parametricSmoothing() const;
    void runGraphic (float* const* channels, int numChannels, int numSamples);
    void runParametric (float* const* channels, int numChannels, int numSamples);

    const bool stereo;
    double sampleRate = 48000.0;
    Settings target;
    std::array<float, numGraphicBands> designedFor {};
    bool designValid = false;

    std::array<Smoothed, numGraphicBands> graphicGain;
    std::array<std::array<Svf, numGraphicBands>, 2> graphicFilters;
    std::array<ParametricBand, numParametricBands> bands;

    Mode mode = Mode::graphic;
    juce::SmoothedValue<float> graphicMix { 1.0f }; // 1 = graphic bank, 0 = parametric bank
    juce::SmoothedValue<float> wet { 1.0f };        // dips to 0 (unprocessed) for a band type change
    bool typeChangePending = false;
    int samplesUntilUpdate = 0;

    CutFilter lowCut { CutFilter::Kind::lowCut }, highCut { CutFilter::Kind::highCut };
    juce::AudioBuffer<float> dry, other;
};

} // namespace ampsim
