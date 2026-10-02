#include "BlockParameters.h"

namespace params
{

using Float = juce::AudioParameterFloat;
using Bool = juce::AudioParameterBool;
using Choice = juce::AudioParameterChoice;

juce::NormalisableRange<float> skewedRange (float lo, float hi, float centre, float interval)
{
    juce::NormalisableRange<float> range (lo, hi, interval);
    range.setSkewForCentre (centre);
    return range;
}

juce::AudioParameterFloatAttributes decibels() { return juce::AudioParameterFloatAttributes().withLabel ("dB"); }
juce::AudioParameterFloatAttributes hertz() { return juce::AudioParameterFloatAttributes().withLabel ("Hz"); }
juce::AudioParameterFloatAttributes milliseconds() { return juce::AudioParameterFloatAttributes().withLabel ("ms"); }
juce::AudioParameterFloatAttributes percent() { return juce::AudioParameterFloatAttributes().withLabel ("%"); }

// ---- Compressor -------------------------------------------------------------------------------

void CompressorParameters::addTo (Layout& layout, const juce::String& p, const juce::String& n, bool onByDefault)
{
    // Defaults (BUILD_PLAN "Compressor", pre instance): Studio, about 4:1, 6 dB soft knee, 8 ms attack
    // so picks and taps keep their pluck, auto release, 70% mix, sidechain high-pass at 100 Hz.
    const ampsim::Compressor::Settings d;
    layout.add (std::make_unique<Bool> (juce::ParameterID { p + "_on", 1 }, n + " On", onByDefault));
    layout.add (std::make_unique<Choice> (juce::ParameterID { p + "_mode", 1 }, n + " Mode", juce::StringArray { "Studio", "Pedal" }, 0));
    layout.add (std::make_unique<Choice> (juce::ParameterID { p + "_detector", 1 }, n + " Detector", juce::StringArray { "Peak", "RMS" }, 0));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_threshold", 1 }, n + " Threshold",
                                         juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), d.thresholdDb, decibels()));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_ratio", 1 }, n + " Ratio", skewedRange (1.0f, 20.0f, 4.0f, 0.01f), d.ratio,
                                         juce::AudioParameterFloatAttributes().withLabel (":1")));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_knee", 1 }, n + " Knee",
                                         juce::NormalisableRange<float> (0.0f, 24.0f, 0.1f), d.kneeDb, decibels()));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_attack", 1 }, n + " Attack", skewedRange (0.1f, 100.0f, 10.0f, 0.01f), d.attackMs, milliseconds()));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_release", 1 }, n + " Release", skewedRange (10.0f, 2000.0f, 200.0f, 0.1f), d.releaseMs, milliseconds()));
    layout.add (std::make_unique<Bool> (juce::ParameterID { p + "_auto_release", 1 }, n + " Auto Release", d.autoRelease));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_makeup", 1 }, n + " Makeup",
                                         juce::NormalisableRange<float> (-12.0f, 24.0f, 0.1f), d.makeupDb, decibels()));
    layout.add (std::make_unique<Bool> (juce::ParameterID { p + "_auto_makeup", 1 }, n + " Auto Makeup", d.autoMakeup));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_mix", 1 }, n + " Mix",
                                         juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.mix * 100.0f, percent()));
    layout.add (std::make_unique<Bool> (juce::ParameterID { p + "_sc_hpf", 1 }, n + " Sidechain High-Pass", d.sidechainHighPass));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_sc_freq", 1 }, n + " Sidechain Frequency", skewedRange (20.0f, 500.0f, 100.0f, 1.0f),
                                         d.sidechainHz, hertz()));
}

void CompressorParameters::bind (State& s, const juce::String& p)
{
    on.bind (s, p + "_on");
    mode.bind (s, p + "_mode");
    detector.bind (s, p + "_detector");
    threshold.bind (s, p + "_threshold");
    ratio.bind (s, p + "_ratio");
    knee.bind (s, p + "_knee");
    attack.bind (s, p + "_attack");
    release.bind (s, p + "_release");
    autoRelease.bind (s, p + "_auto_release");
    makeup.bind (s, p + "_makeup");
    autoMakeup.bind (s, p + "_auto_makeup");
    mix.bind (s, p + "_mix");
    sidechainOn.bind (s, p + "_sc_hpf");
    sidechainHz.bind (s, p + "_sc_freq");
}

ampsim::Compressor::Settings CompressorParameters::read() const noexcept
{
    ampsim::Compressor::Settings s;
    s.mode = mode.index() == 1 ? ampsim::Compressor::Mode::pedal : ampsim::Compressor::Mode::studio;
    s.detector = detector.index() == 1 ? ampsim::Compressor::Detector::rms : ampsim::Compressor::Detector::peak;
    s.thresholdDb = threshold.get();
    s.ratio = ratio.get();
    s.kneeDb = knee.get();
    s.attackMs = attack.get();
    s.releaseMs = release.get();
    s.autoRelease = autoRelease.on();
    s.makeupDb = makeup.get();
    s.autoMakeup = autoMakeup.on();
    s.mix = mix.get() / 100.0f;
    s.sidechainHighPass = sidechainOn.on();
    s.sidechainHz = sidechainHz.get();
    return s;
}

// ---- EQ ---------------------------------------------------------------------------------------

void EqualizerParameters::addTo (Layout& layout, const juce::String& p, const juce::String& n, bool onByDefault)
{
    const ampsim::Equalizer::Settings d;
    const juce::StringArray slopes { "12 dB/oct", "24 dB/oct", "48 dB/oct" };
    static const char* sliderNames[] = { "63 Hz", "125 Hz", "250 Hz", "500 Hz", "1 kHz", "2 kHz", "4 kHz", "8 kHz", "16 kHz" };

    layout.add (std::make_unique<Bool> (juce::ParameterID { p + "_on", 1 }, n + " On", onByDefault));
    layout.add (std::make_unique<Choice> (juce::ParameterID { p + "_mode", 1 }, n + " Mode", juce::StringArray { "Graphic", "Parametric" }, 0));

    for (int m = 0; m < ampsim::Equalizer::numGraphicBands; ++m)
        layout.add (std::make_unique<Float> (juce::ParameterID { sliderId (p, m), 1 }, n + " " + sliderNames[m],
                                             juce::NormalisableRange<float> ((float) -ampsim::Equalizer::sliderRangeDb, (float) ampsim::Equalizer::sliderRangeDb, 0.1f),
                                             0.0f, decibels()));

    for (int b = 0; b < ampsim::Equalizer::numParametricBands; ++b)
    {
        const auto& band = d.bands[(size_t) b];
        const auto bn = n + " Band " + juce::String (b + 1) + " ";
        layout.add (std::make_unique<Choice> (juce::ParameterID { bandId (p, b, "type"), 1 }, bn + "Type",
                                              juce::StringArray { "Peak", "Low Shelf", "High Shelf", "Notch" }, (int) band.type));
        layout.add (std::make_unique<Float> (juce::ParameterID { bandId (p, b, "freq"), 1 }, bn + "Frequency",
                                             skewedRange (20.0f, 20000.0f, 1000.0f, 1.0f), band.frequency, hertz()));
        layout.add (std::make_unique<Float> (juce::ParameterID { bandId (p, b, "gain"), 1 }, bn + "Gain",
                                             juce::NormalisableRange<float> ((float) -ampsim::Equalizer::parametricRangeDb, (float) ampsim::Equalizer::parametricRangeDb, 0.1f),
                                             0.0f, decibels()));
        layout.add (std::make_unique<Float> (juce::ParameterID { bandId (p, b, "q"), 1 }, bn + "Q", skewedRange (0.1f, 18.0f, 1.0f, 0.01f), band.q));
    }

    layout.add (std::make_unique<Bool> (juce::ParameterID { p + "_lowcut_on", 1 }, n + " Low Cut", false));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_lowcut_freq", 1 }, n + " Low Cut Frequency", skewedRange (20.0f, 1000.0f, 100.0f, 1.0f),
                                         d.lowCut.frequency, hertz()));
    layout.add (std::make_unique<Choice> (juce::ParameterID { p + "_lowcut_slope", 1 }, n + " Low Cut Slope", slopes, 0));
    layout.add (std::make_unique<Bool> (juce::ParameterID { p + "_highcut_on", 1 }, n + " High Cut", false));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_highcut_freq", 1 }, n + " High Cut Frequency", skewedRange (1000.0f, 20000.0f, 6000.0f, 1.0f),
                                         d.highCut.frequency, hertz()));
    layout.add (std::make_unique<Choice> (juce::ParameterID { p + "_highcut_slope", 1 }, n + " High Cut Slope", slopes, 0));
}

void EqualizerParameters::bind (State& s, const juce::String& p)
{
    on.bind (s, p + "_on");
    mode.bind (s, p + "_mode");
    for (int m = 0; m < ampsim::Equalizer::numGraphicBands; ++m)
        sliders[(size_t) m].bind (s, sliderId (p, m));
    for (int b = 0; b < ampsim::Equalizer::numParametricBands; ++b)
    {
        auto& band = bands[(size_t) b];
        band.type.bind (s, bandId (p, b, "type"));
        band.frequency.bind (s, bandId (p, b, "freq"));
        band.gain.bind (s, bandId (p, b, "gain"));
        band.q.bind (s, bandId (p, b, "q"));
    }
    lowCutOn.bind (s, p + "_lowcut_on");
    lowCutFrequency.bind (s, p + "_lowcut_freq");
    lowCutSlope.bind (s, p + "_lowcut_slope");
    highCutOn.bind (s, p + "_highcut_on");
    highCutFrequency.bind (s, p + "_highcut_freq");
    highCutSlope.bind (s, p + "_highcut_slope");
}

ampsim::Equalizer::Settings EqualizerParameters::read() const noexcept
{
    // Gains to the nearest 0.01 dB: a parameter on a 0.1 dB grid can read 0 dB as 1.8e-7 dB (float
    // snapping), and a flat EQ should be exactly flat, which is bit-transparent.
    const auto gainDb = [] (const Raw& r) { return std::round (r.get() * 100.0f) / 100.0f; };

    ampsim::Equalizer::Settings s;
    s.mode = mode.index() == 1 ? ampsim::Equalizer::Mode::parametric : ampsim::Equalizer::Mode::graphic;
    for (size_t m = 0; m < sliders.size(); ++m)
        s.sliders[m] = gainDb (sliders[m]);
    for (size_t b = 0; b < bands.size(); ++b)
        s.bands[b] = { (ampsim::Equalizer::BandType) juce::jlimit (0, 3, bands[b].type.index()), bands[b].frequency.get(), gainDb (bands[b].gain), bands[b].q.get() };
    const auto slope = [] (const Raw& r) { return (ampsim::CutFilter::Slope) juce::jlimit (0, 2, r.index()); };
    s.lowCut = { lowCutOn.on(), lowCutFrequency.get(), slope (lowCutSlope) };
    s.highCut = { highCutOn.on(), highCutFrequency.get(), slope (highCutSlope) };
    return s;
}

} // namespace params
