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

// ---- Gates ------------------------------------------------------------------------------------

void GateParameters::addTo (Layout& layout, const juce::String& p, const juce::String& n)
{
    // Defaults and ranges are the block's (Gate.h): threshold -55 dBFS with 8 dB of hysteresis, 10 ms
    // hold, 0.5 ms attack, adaptive release with a 250 ms slow side, a full mute when closed, detecting
    // from the DI through a 100 Hz sidechain high-pass. Off by default, like every effect.
    const ampsim::Gate::Settings d;
    using G = ampsim::Gate;
    layout.add (std::make_unique<Bool> (juce::ParameterID { p + "_on", 1 }, n + " On", false));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_threshold", 1 }, n + " Threshold",
                                         juce::NormalisableRange<float> (G::minThresholdDb, G::maxThresholdDb, 0.1f), d.thresholdDb, decibels()));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_hysteresis", 1 }, n + " Hysteresis",
                                         juce::NormalisableRange<float> (0.0f, G::maxHysteresisDb, 0.1f), d.hysteresisDb, decibels()));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_hold", 1 }, n + " Hold", skewedRange (0.0f, G::maxHoldMs, 50.0f, 0.1f), d.holdMs, milliseconds()));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_attack", 1 }, n + " Attack",
                                         skewedRange (G::minAttackMs, G::maxAttackMs, 2.0f, 0.01f), d.attackMs, milliseconds()));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_release", 1 }, n + " Release",
                                         skewedRange (G::minReleaseMs, G::maxReleaseMs, 200.0f, 0.1f), d.releaseMs, milliseconds()));
    layout.add (std::make_unique<Choice> (juce::ParameterID { p + "_release_mode", 1 }, n + " Release Mode", juce::StringArray { "Adaptive", "Classic" }, 0));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_range", 1 }, n + " Range",
                                         juce::NormalisableRange<float> (G::muteDb, 0.0f, 0.1f), d.rangeDb, decibels()));
    layout.add (std::make_unique<Choice> (juce::ParameterID { p + "_detector", 1 }, n + " Detector", juce::StringArray { "DI", "Own Input" }, 0));
    layout.add (std::make_unique<Bool> (juce::ParameterID { p + "_sc_hpf", 1 }, n + " Sidechain High-Pass", d.sidechainHighPass));
    layout.add (std::make_unique<Float> (juce::ParameterID { p + "_sc_freq", 1 }, n + " Sidechain Frequency",
                                         skewedRange (G::minSidechainHz, G::maxSidechainHz, 100.0f, 1.0f), d.sidechainHz, hertz()));
}

void GateParameters::bind (State& s, const juce::String& p)
{
    on.bind (s, p + "_on");
    threshold.bind (s, p + "_threshold");
    hysteresis.bind (s, p + "_hysteresis");
    hold.bind (s, p + "_hold");
    attack.bind (s, p + "_attack");
    release.bind (s, p + "_release");
    releaseMode.bind (s, p + "_release_mode");
    range.bind (s, p + "_range");
    detector.bind (s, p + "_detector");
    sidechainOn.bind (s, p + "_sc_hpf");
    sidechainHz.bind (s, p + "_sc_freq");
}

ampsim::Gate::Settings GateParameters::read() const noexcept
{
    ampsim::Gate::Settings s;
    s.thresholdDb = threshold.get();
    s.hysteresisDb = hysteresis.get();
    s.holdMs = hold.get();
    s.attackMs = attack.get();
    s.releaseMs = release.get();
    s.releaseMode = releaseMode.index() == 1 ? ampsim::Gate::ReleaseMode::classic : ampsim::Gate::ReleaseMode::adaptive;
    s.rangeDb = range.get();
    s.detector = detector.index() == 1 ? ampsim::Gate::DetectorSource::ownInput : ampsim::Gate::DetectorSource::di;
    s.sidechainHighPass = sidechainOn.on();
    s.sidechainHz = sidechainHz.get();
    return s;
}

// ---- Boost and overdrive --------------------------------------------------------------------------

void BoostParameters::addTo (Layout& layout)
{
    // Ranges and defaults are the block's (Boost.h): Clean at 0 dB is bit-transparent; Tight cuts below
    // 150 Hz and pushes 800 Hz by 6 dB.
    const ampsim::Boost::Settings d;
    layout.add (std::make_unique<Bool> (juce::ParameterID { "boost_on", 1 }, "Boost On", false));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "boost_mode", 1 }, "Boost Mode", juce::StringArray { "Clean", "Tight", "Screamer" }, 0));
    layout.add (std::make_unique<Float> (juce::ParameterID { "boost_level", 1 }, "Boost Level", juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f),
                                         d.levelDb, decibels()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "boost_tilt", 1 }, "Boost Tilt", juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f),
                                         d.tiltDb, decibels()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "boost_tight_freq", 1 }, "Boost Tight Frequency", skewedRange (20.0f, 1000.0f, 150.0f, 1.0f),
                                         d.tightHz, hertz()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "boost_mid", 1 }, "Boost Mid", juce::NormalisableRange<float> (0.0f, 12.0f, 0.1f),
                                         d.midDb, decibels()));
}

void BoostParameters::bind (State& s)
{
    on.bind (s, "boost_on");
    mode.bind (s, "boost_mode");
    level.bind (s, "boost_level");
    tilt.bind (s, "boost_tilt");
    tightHz.bind (s, "boost_tight_freq");
    mid.bind (s, "boost_mid");
}

ampsim::Boost::Settings BoostParameters::read (int oversampling, double volts) const noexcept
{
    ampsim::Boost::Settings s;
    s.mode = (ampsim::Boost::Mode) juce::jlimit (0, ampsim::Boost::numModes - 1, mode.index());
    s.levelDb = level.get();
    s.tiltDb = tilt.get();
    s.tightHz = tightHz.get();
    s.midDb = mid.get();
    s.oversampling = oversampling;
    s.voltsAtFullScale = volts;
    return s;
}

juce::StringArray OverdriveParameters::modeNames()
{
    const juce::StringArray names { "Mid Drive", "Distortion" };
    jassert (names.size() == numModes);
    return names;
}

void OverdriveParameters::addTo (Layout& layout)
{
    // Defaults: Mid Drive with drive and tone at noon, unity level, fully wet, Tight off (150 Hz when on).
    layout.add (std::make_unique<Bool> (juce::ParameterID { "od_on", 1 }, "Overdrive On", false));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "od_mode", 1 }, "Overdrive Mode", modeNames(), 0));
    layout.add (std::make_unique<Float> (juce::ParameterID { "od_drive", 1 }, "Overdrive Drive", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
                                         50.0f, percent()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "od_tone", 1 }, "Overdrive Tone", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
                                         50.0f, percent()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "od_level", 1 }, "Overdrive Level", skewedRange (-60.0f, 24.0f, 0.0f, 0.1f), 0.0f,
                                         decibels()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "od_mix", 1 }, "Overdrive Mix", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
                                         100.0f, percent()));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "od_tight", 1 }, "Overdrive Tight", false));
    layout.add (std::make_unique<Float> (juce::ParameterID { "od_tight_freq", 1 }, "Overdrive Tight Frequency",
                                         skewedRange (ampsim::DriveEngine::tightOffHz + 1.0f, ampsim::DriveEngine::maxTightHz, 150.0f, 1.0f), 150.0f, hertz()));
}

void OverdriveParameters::bind (State& s)
{
    on.bind (s, "od_on");
    mode.bind (s, "od_mode");
    drive.bind (s, "od_drive");
    tone.bind (s, "od_tone");
    level.bind (s, "od_level");
    mix.bind (s, "od_mix");
    tightOn.bind (s, "od_tight");
    tightHz.bind (s, "od_tight_freq");
}

ampsim::Overdrive::Settings OverdriveParameters::read (int oversampling, double volts) const noexcept
{
    ampsim::Overdrive::Settings s;
    s.mode = (ampsim::Overdrive::Mode) juce::jlimit (0, numModes - 1, mode.index());
    s.drive = drive.get() / 100.0f;
    s.tone = tone.get() / 100.0f;
    s.levelDb = level.get();
    s.mix = mix.get() / 100.0f;
    s.tightHz = tightOn.on() ? tightHz.get() : ampsim::DriveEngine::tightOffHz;
    s.oversampling = oversampling;
    s.voltsAtFullScale = volts;
    return s;
}

int oversamplingFactor (const Raw& choice) noexcept
{
    return choice.index() == 1 ? 8 : 4;
}

double voltsAtFullScale (double interfaceDbu) noexcept
{
    return 1.4142135623730951 * 0.7746 * std::pow (10.0, interfaceDbu / 20.0);
}

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
    layout.add (std::make_unique<Choice> (juce::ParameterID { "delay_note", 1 }, "Delay Note", notes, noteIndex ("1/8D")));
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

// ---- Chorus -----------------------------------------------------------------------------------

namespace
{
juce::StringArray noteNames()
{
    juce::StringArray names;
    for (const auto& n : ampsim::tempo::notes)
        names.add (n.name);
    return names;
}

double noteMs (const Raw& r, double bpm)
{
    const auto& n = ampsim::tempo::notes[(size_t) juce::jlimit (0, (int) ampsim::tempo::notes.size() - 1, r.index())];
    return ampsim::tempo::milliseconds (n.division, n.feel, juce::jmax (1.0, bpm));
}
} // namespace

void ChorusParameters::addTo (Layout& layout)
{
    const ampsim::Chorus::Settings d;
    const auto notes = noteNames();
    layout.add (std::make_unique<Bool> (juce::ParameterID { "chorus_on", 1 }, "Chorus On", false));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "chorus_mode", 1 }, "Chorus Mode", juce::StringArray { "Classic", "Dimension", "Tri" }, 0));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "chorus_shape", 1 }, "Chorus LFO Shape", juce::StringArray { "Triangle", "Sine", "Random" }, 0));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "chorus_sync", 1 }, "Chorus Sync", false));
    layout.add (std::make_unique<Float> (juce::ParameterID { "chorus_rate", 1 }, "Chorus Rate", skewedRange (0.05f, 10.0f, 1.0f, 0.01f), d.rateHz, hertz()));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "chorus_note", 1 }, "Chorus Note", notes, notes.indexOf ("1/1")));
    layout.add (std::make_unique<Float> (juce::ParameterID { "chorus_depth", 1 }, "Chorus Depth", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.depth * 100.0f, percent()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "chorus_mix", 1 }, "Chorus Mix", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.mix * 100.0f, percent()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "chorus_width", 1 }, "Chorus Width", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.width * 100.0f, percent()));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "chorus_analog", 1 }, "Chorus Analog", d.analog));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "chorus_noise", 1 }, "Chorus Noise", d.noise));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "chorus_hp", 1 }, "Chorus Low-End Protection", d.wetHighPass));
    layout.add (std::make_unique<Float> (juce::ParameterID { "chorus_hp_freq", 1 }, "Chorus Low-End Frequency",
                                         skewedRange ((float) ampsim::Chorus::minHighPassHz, (float) ampsim::Chorus::maxHighPassHz, 150.0f, 1.0f), d.wetHighPassHz, hertz()));
}

void ChorusParameters::bind (State& s)
{
    on.bind (s, "chorus_on");
    mode.bind (s, "chorus_mode");
    shape.bind (s, "chorus_shape");
    sync.bind (s, "chorus_sync");
    rate.bind (s, "chorus_rate");
    note.bind (s, "chorus_note");
    depth.bind (s, "chorus_depth");
    mix.bind (s, "chorus_mix");
    width.bind (s, "chorus_width");
    analog.bind (s, "chorus_analog");
    noise.bind (s, "chorus_noise");
    highPass.bind (s, "chorus_hp");
    highPassHz.bind (s, "chorus_hp_freq");
}

ampsim::Chorus::Settings ChorusParameters::read (double bpm) const noexcept
{
    static constexpr ampsim::Lfo::Shape shapes[] = { ampsim::Lfo::Shape::triangle, ampsim::Lfo::Shape::sine, ampsim::Lfo::Shape::random };
    ampsim::Chorus::Settings s;
    s.mode = (ampsim::Chorus::Mode) juce::jlimit (0, 2, mode.index());
    s.shape = shapes[juce::jlimit (0, 2, shape.index())];
    s.rateHz = sync.on() ? (float) juce::jlimit (ampsim::Chorus::minRateHz, ampsim::Chorus::maxRateHz, 1000.0 / noteMs (note, bpm)) : rate.get();
    s.depth = depth.get() / 100.0f;
    s.mix = mix.get() / 100.0f;
    s.width = width.get() / 100.0f;
    s.analog = analog.on();
    s.noise = noise.on();
    s.wetHighPass = highPass.on();
    s.wetHighPassHz = highPassHz.get();
    return s;
}

// ---- Reverb -----------------------------------------------------------------------------------

void ReverbParameters::addTo (Layout& layout)
{
    const ampsim::Reverb::Settings d;
    const auto notes = noteNames();
    layout.add (std::make_unique<Bool> (juce::ParameterID { "reverb_on", 1 }, "Reverb On", false));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "reverb_engine", 1 }, "Reverb Type", juce::StringArray { "Room", "Hall", "Plate" }, (int) d.engine));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_mix", 1 }, "Reverb Mix", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.mix * 100.0f, percent()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_predelay", 1 }, "Reverb Pre-delay",
                                         skewedRange (0.0f, (float) ampsim::Reverb::maxPreDelayMs, 40.0f, 0.1f), d.preDelayMs, milliseconds()));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "reverb_predelay_sync", 1 }, "Reverb Pre-delay Sync", false));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "reverb_predelay_note", 1 }, "Reverb Pre-delay Note", notes, notes.indexOf ("1/16")));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_size", 1 }, "Reverb Size", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.size * 100.0f, percent()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_decay", 1 }, "Reverb Decay", skewedRange (0.1f, 30.0f, 2.5f, 0.01f), d.decaySeconds,
                                         juce::AudioParameterFloatAttributes().withLabel ("s")));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_low_decay", 1 }, "Reverb Low Decay", skewedRange (0.25f, 4.0f, 1.0f, 0.01f), d.lowDecayMultiplier,
                                         juce::AudioParameterFloatAttributes().withLabel ("x")));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_high_decay", 1 }, "Reverb High Decay", skewedRange (0.1f, 2.0f, 0.5f, 0.01f), d.highDecayMultiplier,
                                         juce::AudioParameterFloatAttributes().withLabel ("x")));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_diffusion", 1 }, "Reverb Diffusion", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.diffusion * 100.0f, percent()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_mod_depth", 1 }, "Reverb Modulation Depth", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.modDepth * 100.0f, percent()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_mod_rate", 1 }, "Reverb Modulation Rate", skewedRange (0.05f, 5.0f, 0.5f, 0.01f), d.modRateHz, hertz()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_width", 1 }, "Reverb Width", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.width * 100.0f, percent()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_early_late", 1 }, "Reverb Early/Late", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.earlyLate * 100.0f, percent()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_lowcut", 1 }, "Reverb Low Cut", skewedRange (20.0f, 1000.0f, 150.0f, 1.0f), d.lowCutHz, hertz()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_highcut", 1 }, "Reverb High Cut", skewedRange (1000.0f, 20000.0f, 6000.0f, 1.0f), d.highCutHz, hertz()));
    layout.add (std::make_unique<Float> (juce::ParameterID { "reverb_ducking", 1 }, "Reverb Ducking", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.ducking * 100.0f, percent()));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "reverb_freeze", 1 }, "Reverb Freeze", false));
}

void ReverbParameters::bind (State& s)
{
    on.bind (s, "reverb_on");
    engine.bind (s, "reverb_engine");
    mix.bind (s, "reverb_mix");
    preDelay.bind (s, "reverb_predelay");
    preDelaySync.bind (s, "reverb_predelay_sync");
    preDelayNote.bind (s, "reverb_predelay_note");
    size.bind (s, "reverb_size");
    decay.bind (s, "reverb_decay");
    lowDecay.bind (s, "reverb_low_decay");
    highDecay.bind (s, "reverb_high_decay");
    diffusion.bind (s, "reverb_diffusion");
    modDepth.bind (s, "reverb_mod_depth");
    modRate.bind (s, "reverb_mod_rate");
    width.bind (s, "reverb_width");
    earlyLate.bind (s, "reverb_early_late");
    lowCut.bind (s, "reverb_lowcut");
    highCut.bind (s, "reverb_highcut");
    ducking.bind (s, "reverb_ducking");
    freeze.bind (s, "reverb_freeze");
}

ampsim::Reverb::Settings ReverbParameters::read (double bpm, int freezeOverride) const noexcept
{
    ampsim::Reverb::Settings s;
    s.engine = (ampsim::Reverb::Engine) juce::jlimit (0, 2, engine.index());
    s.mix = mix.get() / 100.0f;
    s.preDelayMs = preDelaySync.on() ? (float) juce::jmin (ampsim::Reverb::maxPreDelayMs, noteMs (preDelayNote, bpm)) : preDelay.get();
    s.size = size.get() / 100.0f;
    s.decaySeconds = decay.get();
    s.lowDecayMultiplier = lowDecay.get();
    s.highDecayMultiplier = highDecay.get();
    s.diffusion = diffusion.get() / 100.0f;
    s.modDepth = modDepth.get() / 100.0f;
    s.modRateHz = modRate.get();
    s.width = width.get() / 100.0f;
    s.earlyLate = earlyLate.get() / 100.0f;
    s.lowCutHz = lowCut.get();
    s.highCutHz = highCut.get();
    s.ducking = ducking.get() / 100.0f;
    s.freeze = freezeOverride >= 0 ? freezeOverride == 1 : freeze.on();
    return s;
}

} // namespace params
