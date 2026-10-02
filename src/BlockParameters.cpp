#include "BlockParameters.h"
#include "dsp/Tempo.h"

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

// ---- Delay ------------------------------------------------------------------------------------

void DelayParameters::addTo (Layout& layout)
{
    const ampsim::Delay::Settings d;
    juce::StringArray notes;
    for (const auto& n : ampsim::tempo::notes)
        notes.add (n.name);
    const auto noteIndex = [&] (const char* name) { return notes.indexOf (name); };

    layout.add (std::make_unique<Bool> (juce::ParameterID { "delay_on", 1 }, "Delay On", false));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "delay_mode", 1 }, "Delay Mode", juce::StringArray { "Digital", "Analog", "Tape" }, 0));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "delay_stereo", 1 }, "Delay Stereo", juce::StringArray { "Stereo", "Ping-pong", "Dual" }, 0));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "delay_sync", 1 }, "Delay Sync", true));
    layout.add (std::make_unique<Float> (juce::ParameterID { "delay_time", 1 }, "Delay Time", skewedRange (1.0f, 4000.0f, 400.0f, 0.1f), d.timeMs, milliseconds()));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "delay_note", 1 }, "Delay Note", notes, noteIndex ("1/8 dotted")));
    layout.add (std::make_unique<Float> (juce::ParameterID { "delay_time_right", 1 }, "Delay Right Time", skewedRange (1.0f, 4000.0f, 400.0f, 0.1f), d.rightTimeMs, milliseconds()));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "delay_note_right", 1 }, "Delay Right Note", notes, noteIndex ("1/4")));
    layout.add (std::make_unique<Float> (juce::ParameterID { "delay_offset", 1 }, "Delay Stereo Offset", juce::NormalisableRange<float> (0.0f, 50.0f, 0.1f), d.offsetMs, milliseconds()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "delay_feedback", 1 }, "Delay Feedback", juce::NormalisableRange<float> (0.0f, 110.0f, 0.1f), d.feedback * 100.0f, percent()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "delay_lowcut", 1 }, "Delay Low Cut", skewedRange (20.0f, 2000.0f, 200.0f, 1.0f), d.lowCutHz, hertz()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "delay_highcut", 1 }, "Delay High Cut", skewedRange (500.0f, 20000.0f, 5000.0f, 1.0f), d.highCutHz, hertz()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "delay_mod_depth", 1 }, "Delay Modulation Depth", juce::NormalisableRange<float> (0.0f, 5.0f, 0.01f), d.modDepthMs, milliseconds()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "delay_mod_rate", 1 }, "Delay Modulation Rate", skewedRange (0.05f, 10.0f, 1.0f, 0.01f), d.modRateHz, hertz()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "delay_duck", 1 }, "Delay Ducking", juce::NormalisableRange<float> (0.0f, 24.0f, 0.1f), d.duckDb, decibels()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "delay_mix", 1 }, "Delay Mix", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.mix * 100.0f, percent()));
}

void DelayParameters::bind (State& s)
{
    on.bind (s, "delay_on");
    mode.bind (s, "delay_mode");
    stereoMode.bind (s, "delay_stereo");
    sync.bind (s, "delay_sync");
    time.bind (s, "delay_time");
    note.bind (s, "delay_note");
    rightTime.bind (s, "delay_time_right");
    rightNote.bind (s, "delay_note_right");
    offset.bind (s, "delay_offset");
    feedback.bind (s, "delay_feedback");
    lowCut.bind (s, "delay_lowcut");
    highCut.bind (s, "delay_highcut");
    modDepth.bind (s, "delay_mod_depth");
    modRate.bind (s, "delay_mod_rate");
    duck.bind (s, "delay_duck");
    mix.bind (s, "delay_mix");
}

ampsim::Delay::Settings DelayParameters::read (double bpm) const noexcept
{
    const auto noteMs = [bpm] (const Raw& r)
    {
        const auto& n = ampsim::tempo::notes[(size_t) juce::jlimit (0, (int) ampsim::tempo::notes.size() - 1, r.index())];
        return (float) ampsim::tempo::milliseconds (n.division, n.feel, juce::jmax (1.0, bpm));
    };

    ampsim::Delay::Settings s;
    s.mode = (ampsim::Delay::Mode) juce::jlimit (0, 2, mode.index());
    s.stereoMode = (ampsim::Delay::StereoMode) juce::jlimit (0, 2, stereoMode.index());
    s.timeMs = sync.on() ? noteMs (note) : time.get();
    s.rightTimeMs = sync.on() ? noteMs (rightNote) : rightTime.get();
    s.offsetMs = offset.get();
    s.feedback = feedback.get() / 100.0f;
    s.lowCutHz = lowCut.get();
    s.highCutHz = highCut.get();
    s.modDepthMs = modDepth.get();
    s.modRateHz = modRate.get();
    s.duckDb = duck.get();
    s.mix = mix.get() / 100.0f;
    return s;
}

} // namespace params
