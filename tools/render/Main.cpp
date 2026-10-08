// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// ampsim_render: runs a WAV file through the same DSP chain the app uses, offline.
//
// Use it to listen to a capture/IR combination without the interface, to compare against
// NeuralAmpModelerCore's own render tool (--compare), and to measure CPU per audio block. (--slots N loads
// the model into the first N amps; since 2026-10-07 only the selected amp runs, so N > 1 no longer adds CPU.)

#include "dsp/Chain.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <vector>

namespace
{
struct Options
{
    juce::File model, ir, ir2, room, input, output, compare;
    float inputGainDb = 0.0f, outputGainDb = 0.0f;
    int blockSize = 128;
    int slots = 1;
    bool normalize = true;
    juce::String boost; // "", or the boost block's mode in front of the amp: clean, tight, screamer
    float boostLevelDb = 0.0f;

    // Tone match (docs/TONE_MATCH.md) renders candidates with these. Defaults leave the chain as before.
    float trimDb = 0.0f;                                            // slot 1's Gain, as amp1_input_trim stores it (dB)
    std::array<float, ampsim::AmpTone::numBands> tone {};          // slot 1's Depth, Bass, Mid, Treble, Presence (dB)
    bool postEqOn = false;                                         // the post EQ, parametric mode
    ampsim::Equalizer::Settings postEq;
    juce::File matchCurve;                                         // the match curve's points (after the cab)
    float matchAmountPercent = 100.0f;

    // The tone match benchmark (prototypes/tone_bench) renders hidden rigs' pedals and compressors with these,
    // and the oracle's "the true pedal in BellyDSP". Defaults leave the chain as before.
    float boostTiltDb = 0.0f;                                      // the boost's Clean tilt
    bool overdriveOn = false;
    ampsim::Overdrive::Settings overdrive;
    bool preCompOn = false, postCompOn = false;
    ampsim::Compressor::Settings preComp, postComp;
};

// "--od mid:0.6:0.5:0:20": the overdrive's mode (mid, distortion, transparent, fuzz), Drive and Tone (0 to 1),
// Level (dB), and Tight (Hz; 20 is off). With the unity trim the app applies.
bool parseOverdrive (const juce::String& text, ampsim::Overdrive::Settings& od)
{
    const auto f = juce::StringArray::fromTokens (text, ":", "");
    if (f.size() != 5)
        return false;
    if (f[0] == "mid")              od.mode = ampsim::Overdrive::Mode::midDrive;
    else if (f[0] == "distortion")  od.mode = ampsim::Overdrive::Mode::distortion;
    else if (f[0] == "transparent") od.mode = ampsim::Overdrive::Mode::transparent;
    else if (f[0] == "fuzz")        od.mode = ampsim::Overdrive::Mode::fuzz;
    else                            return false;
    od.drive = juce::jlimit (0.0f, 1.0f, f[1].getFloatValue());
    od.tone = juce::jlimit (0.0f, 1.0f, f[2].getFloatValue());
    od.levelDb = f[3].getFloatValue();
    od.tightHz = juce::jmax (20.0f, f[4].getFloatValue());
    od.unityTrim = true;
    return true;
}

// "--pre-comp studio:rms:-24:4:8:120:0:1": mode (studio, pedal), detector (peak, rms), threshold (dB), ratio,
// attack and release (ms), makeup (dB), and mix (0 to 1); fixed release and makeup, a 6 dB knee, the sidechain
// high-pass at its default.
bool parseCompressor (const juce::String& text, ampsim::Compressor::Settings& c)
{
    const auto f = juce::StringArray::fromTokens (text, ":", "");
    if (f.size() != 8)
        return false;
    if (f[0] == "studio")      c.mode = ampsim::Compressor::Mode::studio;
    else if (f[0] == "pedal")  c.mode = ampsim::Compressor::Mode::pedal;
    else                       return false;
    if (f[1] == "peak")        c.detector = ampsim::Compressor::Detector::peak;
    else if (f[1] == "rms")    c.detector = ampsim::Compressor::Detector::rms;
    else                       return false;
    c.thresholdDb = f[2].getFloatValue();
    c.ratio = juce::jmax (1.0f, f[3].getFloatValue());
    c.attackMs = f[4].getFloatValue();
    c.releaseMs = f[5].getFloatValue();
    c.makeupDb = f[6].getFloatValue();
    c.mix = juce::jlimit (0.0f, 1.0f, f[7].getFloatValue());
    c.autoRelease = false;
    c.autoMakeup = false;
    return true;
}

// "--post-eq ls:100:3:0.71,pk:400:-2:1,pk:1000:0:1,pk:3000:1.5:2,hs:8000:-4:0.71": up to five parametric
// bands in order, each type:frequency:gain:q, type one of pk, ls, hs, notch. Bands not given stay flat.
/// A match curve file: JSON as presets store it ([[hz, dB], ...]), or text with one "hz dB" pair per line
/// (commas or spaces between, # starts a comment).
bool readMatchCurve (const juce::File& file, ampsim::MatchCurve::Curve& curve)
{
    const auto text = file.loadFileAsString();
    if (text.trimStart().startsWith ("["))
    {
        curve = ampsim::MatchCurve::Curve::fromVar (juce::JSON::parse (text));
        return ! curve.points.empty();
    }

    std::vector<ampsim::MatchCurve::Point> points;
    for (auto line : juce::StringArray::fromLines (text))
    {
        line = line.upToFirstOccurrenceOf ("#", false, false).replaceCharacter (',', ' ').trim();
        if (line.isEmpty())
            continue;
        const auto fields = juce::StringArray::fromTokens (line, " \t", "");
        if (fields.size() != 2)
            return false;
        points.push_back ({ fields[0].getDoubleValue(), fields[1].getDoubleValue() });
    }
    curve = ampsim::MatchCurve::Curve::fromPoints (std::move (points));
    return ! curve.points.empty();
}

bool parsePostEq (const juce::String& text, ampsim::Equalizer::Settings& eq)
{
    eq.mode = ampsim::Equalizer::Mode::parametric;
    const auto bands = juce::StringArray::fromTokens (text, ",", "");

    if (bands.size() > ampsim::Equalizer::numParametricBands)
        return false;

    for (int b = 0; b < bands.size(); ++b)
    {
        const auto fields = juce::StringArray::fromTokens (bands[b], ":", "");
        if (fields.size() != 4)
            return false;

        auto& band = eq.bands[(size_t) b];
        const auto& t = fields[0];
        if (t == "pk")         band.type = ampsim::Equalizer::BandType::peak;
        else if (t == "ls")    band.type = ampsim::Equalizer::BandType::lowShelf;
        else if (t == "hs")    band.type = ampsim::Equalizer::BandType::highShelf;
        else if (t == "notch") band.type = ampsim::Equalizer::BandType::notch;
        else                   return false;

        band.frequency = fields[1].getFloatValue();
        band.gainDb = fields[2].getFloatValue();
        band.q = fields[3].getFloatValue();

        if (band.frequency <= 0.0f || band.q <= 0.0f)
            return false;
    }

    return true;
}

void printUsage()
{
    std::cerr << "Usage: ampsim_render [options] <input.wav> <output.wav>\n"
                 "  --model <file>         amp capture: a .nam, or a gain set's gainset.json (omit for passthrough)\n"
                 "  --ir <file.wav>        cab close mic 1 impulse response (omit for no cab)\n"
                 "  --ir2 <file.wav>       cab close mic 2 impulse response (auto-aligned to mic 1)\n"
                 "  --room <file.wav>      cab room mic impulse response (mono or stereo)\n"
                 "  --input-gain <dB>      input trim (default 0)\n"
                 "  --output-gain <dB>     output level (default 0)\n"
                 "  --block <samples>      block size (default 128)\n"
                 "  --slots <n>            load the model into n of the 9 amps (default 1; only amp 1 runs and is heard)\n"
                 "  --no-normalize         skip loudness normalization of the model\n"
                 "  --boost <mode>         switch the Boost block on in front of the amp: clean, tight, or screamer\n"
                 "                         (the TS808 circuit at minimum drive, tone at noon; 0 dBFS = +12 dBu)\n"
                 "  --boost-level <dB>     the boost's Level (default 0)\n"
                 "  --boost-tilt <dB>      the boost's Clean tilt, highs minus lows (default 0)\n"
                 "  --od <m:d:t:l:h>       switch the overdrive on: mode mid, distortion, transparent, or fuzz, Drive and\n"
                 "                         Tone 0 to 1, Level dB, Tight Hz (20 is off), with the app's unity trim\n"
                 "                         (e.g. mid:0.6:0.5:0:20)\n"
                 "  --pre-comp <settings>  switch the pre compressor on: mode:detector:threshold:ratio:attack:release:\n"
                 "                         makeup:mix (e.g. pedal:peak:-30:3:2:200:8:1); fixed release and makeup\n"
                 "  --post-comp <settings> the post compressor (after the cab and post EQ), the same format\n"
                 "  --compare <ref.wav>    report the difference between the output and a reference\n"
                 "  --trim <dB>            slot 1's Gain as its parameter stores it, -24 to +24 dB (default 0)\n"
                 "  --gain <0-10>          slot 1's Gain as the amp head shows it (5 is --trim 0): a gain set's\n"
                 "                         position across its steps, or a single capture's compensated trim\n"
                 "  --tone <d,b,m,t,p>     slot 1's Depth, Bass, Mid, Treble, Presence in dB (default all 0)\n"
                 "  --post-eq <bands>      switch the post EQ on in parametric mode: up to five comma-separated\n"
                 "                         bands type:freq:gain:q, type pk, ls, hs, or notch\n"
                 "                         (e.g. ls:100:3:0.71,pk:1000:-2:1,hs:8000:-4:0.71)\n"
                 "  --match-curve <file>   switch the match curve on (after the cab, before the post EQ): JSON\n"
                 "                         [[hz, dB], ...] as presets store it, or one \"hz dB\" pair per line\n"
                 "  --match-amount <%>     the match curve's amount, 0 to 100 (default 100)\n";
}

juce::File fileArg (const char* arg)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (arg));
}

bool parse (int argc, char* argv[], Options& o)
{
    std::vector<juce::File> positional;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        const bool hasValue = i + 1 < argc;

        if (a == "--model" && hasValue)              o.model = fileArg (argv[++i]);
        else if (a == "--ir" && hasValue)            o.ir = fileArg (argv[++i]);
        else if (a == "--ir2" && hasValue)           o.ir2 = fileArg (argv[++i]);
        else if (a == "--room" && hasValue)          o.room = fileArg (argv[++i]);
        else if (a == "--compare" && hasValue)       o.compare = fileArg (argv[++i]);
        else if (a == "--input-gain" && hasValue)    o.inputGainDb = juce::String (argv[++i]).getFloatValue();
        else if (a == "--output-gain" && hasValue)   o.outputGainDb = juce::String (argv[++i]).getFloatValue();
        else if (a == "--block" && hasValue)         o.blockSize = juce::String (argv[++i]).getIntValue();
        else if (a == "--slots" && hasValue)         o.slots = juce::String (argv[++i]).getIntValue();
        else if (a == "--no-normalize")              o.normalize = false;
        else if (a == "--boost" && hasValue)         o.boost = argv[++i];
        else if (a == "--boost-level" && hasValue)   o.boostLevelDb = juce::String (argv[++i]).getFloatValue();
        else if (a == "--boost-tilt" && hasValue)    o.boostTiltDb = juce::String (argv[++i]).getFloatValue();
        else if (a == "--od" && hasValue)
        {
            if (! parseOverdrive (argv[++i], o.overdrive))
                return false;
            o.overdriveOn = true;
        }
        else if ((a == "--pre-comp" || a == "--post-comp") && hasValue)
        {
            const bool isPre = a == "--pre-comp";
            if (! parseCompressor (argv[++i], isPre ? o.preComp : o.postComp))
                return false;
            (isPre ? o.preCompOn : o.postCompOn) = true;
        }
        else if (a == "--trim" && hasValue)          o.trimDb = juce::String (argv[++i]).getFloatValue();
        else if (a == "--gain" && hasValue)          o.trimDb = ampsim::AmpSection::GainKnob::dbForPosition (juce::String (argv[++i]).getFloatValue());
        else if (a == "--tone" && hasValue)
        {
            const auto values = juce::StringArray::fromTokens (argv[++i], ",", "");
            if (values.size() != ampsim::AmpTone::numBands)
                return false;
            for (int b = 0; b < values.size(); ++b)
                o.tone[(size_t) b] = values[b].getFloatValue();
        }
        else if (a == "--post-eq" && hasValue)
        {
            if (! parsePostEq (argv[++i], o.postEq))
                return false;
            o.postEqOn = true;
        }
        else if (a == "--match-curve" && hasValue)   o.matchCurve = fileArg (argv[++i]);
        else if (a == "--match-amount" && hasValue)  o.matchAmountPercent = juce::String (argv[++i]).getFloatValue();
        else if (a.startsWith ("--"))                return false;
        else                                         positional.push_back (fileArg (argv[i]));
    }

    if (positional.size() != 2 || o.blockSize < 1 || o.slots < 1)
        return false;

    if (o.boost.isNotEmpty() && ! juce::StringArray { "clean", "tight", "screamer" }.contains (o.boost))
        return false;

    o.input = positional[0];
    o.output = positional[1];
    return true;
}

juce::AudioBuffer<float> readMono (const juce::File& file, double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return {};

    juce::AudioBuffer<float> buffer (1, (int) reader->lengthInSamples);
    reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, false); // left channel
    sampleRate = reader->sampleRate;
    return buffer;
}

bool writeFloatWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
        return false;

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (buffer.getNumChannels())
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    auto writer = juce::WavAudioFormat().createWriterFor (stream, options);
    return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

double toDb (double linear)
{
    return linear > 0.0 ? 20.0 * std::log10 (linear) : -400.0;
}

void printLevels (const char* name, const juce::AudioBuffer<float>& buffer)
{
    const auto n = buffer.getNumSamples();
    double sumSquares = 0.0;
    int nonFinite = 0;

    for (int i = 0; i < n; ++i)
    {
        const auto x = buffer.getSample (0, i);
        nonFinite += std::isfinite (x) ? 0 : 1;
        sumSquares += (double) x * x;
    }

    std::cout << "  " << name << ": peak " << juce::String (toDb (buffer.getMagnitude (0, 0, n)), 1)
              << " dBFS, RMS " << juce::String (toDb (std::sqrt (sumSquares / juce::jmax (1, n))), 1)
              << " dBFS, non-finite samples: " << nonFinite << "\n";
}
} // namespace

int main (int argc, char* argv[])
{
    Options o;

    if (! parse (argc, argv, o))
    {
        printUsage();
        return 1;
    }

    double sampleRate = 0.0;
    const auto input = readMono (o.input, sampleRate);

    if (input.getNumSamples() == 0)
    {
        std::cerr << "Couldn't read " << o.input.getFullPathName() << "\n";
        return 1;
    }

    if (std::abs (sampleRate - ampsim::NamAmp::requiredSampleRate) > 0.5)
    {
        std::cerr << "The input is " << sampleRate << " Hz; the chain runs at 48000 Hz only.\n";
        return 1;
    }

    ampsim::Chain chain;

    if (o.model != juce::File())
    {
        // --slots loads the model into the first N amps. Only the selected one, amp 1, runs and is heard
        // (BUILD_PLAN "Amp switching"), so the render and its timing are amp 1's whatever N is.
        for (int s = 0; s < juce::jmin (o.slots, ampsim::AmpSection::numAmps); ++s)
        {
            const auto result = chain.amp.amp (s).model.loadModel (o.model, o.normalize);

            if (! result.ok)
            {
                std::cerr << result.message << "\n";
                return 1;
            }

            if (s == 0)
                std::cout << "Model: " << result.message << "\n";
        }
    }

    const std::array<std::pair<juce::File, int>, 3> irs { { { o.ir, 0 }, { o.ir2, 1 }, { o.room, 2 } } };
    for (const auto& [file, mic] : irs)
    {
        if (file == juce::File())
            continue;

        const auto result = mic == 2 ? chain.cab.loadRoom (file) : chain.cab.loadCloseMic (mic, file);

        if (! result.ok)
        {
            std::cerr << result.message << "\n";
            return 1;
        }

        std::cout << (mic == 2 ? "Room IR: " : "IR " + juce::String (mic + 1) + ": ") << result.message << "\n";
    }

    if (chain.cab.getAlignment().valid)
    {
        const auto a = chain.cab.getAlignment();
        std::cout << "Close mics aligned: mic 1 +" << a.delayMic1 << ", mic 2 +" << a.delayMic2 << " samples"
                  << (a.invertMic2 ? ", mic 2 inverted" : "") << "\n";
    }

    // The boost, if asked for (tools/content/make_default_captures.py renders Monolith's boost with it).
    // Settings and bypass before prepare(), which snaps both, so it's fully on from the first sample.
    if (o.boost.isNotEmpty())
    {
        ampsim::Boost::Settings b;
        b.mode = o.boost == "screamer" ? ampsim::Boost::Mode::screamer
               : o.boost == "tight"    ? ampsim::Boost::Mode::tight
                                       : ampsim::Boost::Mode::clean;
        b.levelDb = o.boostLevelDb;
        b.tiltDb = o.boostTiltDb;
        chain.boost.setSettings (b);
        chain.setBypassed (ampsim::Chain::Slot::boost, false);
        std::cout << "Boost: " << o.boost << ", level " << juce::String (o.boostLevelDb, 1) << " dB\n";
    }

    if (o.overdriveOn)
    {
        chain.overdrive.setSettings (o.overdrive);
        chain.setBypassed (ampsim::Chain::Slot::overdrive, false);
        std::cout << "Overdrive: on\n";
    }

    if (o.preCompOn)
    {
        chain.preCompressor.setSettings (o.preComp);
        chain.setBypassed (ampsim::Chain::Slot::preCompressor, false);
    }

    if (o.postCompOn)
    {
        chain.postCompressor.setSettings (o.postComp);
        chain.setBypassed (ampsim::Chain::Slot::postCompressor, false);
    }

    // Slot 1's Gain and tone, and the post EQ, also before prepare(), which snaps their smoothers.
    chain.amp.amp (0).inputTrim.setGainDecibels (o.trimDb);
    for (int b = 0; b < ampsim::AmpTone::numBands; ++b)
        chain.amp.amp (0).tone.setGainDb ((ampsim::AmpTone::Band) b, o.tone[(size_t) b]);

    if (o.postEqOn)
    {
        chain.postEq.setSettings (o.postEq);
        chain.setBypassed (ampsim::Chain::Slot::postEq, false);
    }

    // The match curve: designed here (as the app's loader does) before prepare(), which installs it, so it
    // plays from the first sample. Amount 0 is the app's bypass.
    if (o.matchCurve != juce::File())
    {
        ampsim::MatchCurve::Curve curve;
        if (! readMatchCurve (o.matchCurve, curve))
        {
            std::cerr << "Couldn't read a match curve from " << o.matchCurve.getFullPathName() << "\n";
            return 1;
        }
        const auto amount = juce::jlimit (0.0f, 100.0f, o.matchAmountPercent);
        chain.matchCurve.setCurve (curve, amount / 100.0);
        chain.setBypassed (ampsim::Chain::Slot::matchCurve, amount <= 0.0f || curve.isFlat());
        std::cout << "Match curve: " << curve.points.size() << " points, amount " << amount << "%\n";
    }

    // Set the gains before prepare(), which snaps them, so the render doesn't start with a ramp.
    chain.inputGain.setGainDecibels (o.inputGainDb);
    chain.outputGain.setGainDecibels (o.outputGainDb);
    chain.prepare (sampleRate, o.blockSize); // also installs the models and IR with no fade

    const auto numSamples = input.getNumSamples();
    juce::AudioBuffer<float> output (2, numSamples), block (2, o.blockSize);
    std::vector<double> blockMicros;

    for (int start = 0; start < numSamples; start += o.blockSize)
    {
        const auto len = juce::jmin (o.blockSize, numSamples - start);
        block.copyFrom (0, 0, input, 0, start, len);
        block.clear (1, 0, len);

        const auto t0 = std::chrono::steady_clock::now();

        chain.process (juce::dsp::AudioBlock<float> (block).getSubBlock (0, (size_t) len));

        const auto t1 = std::chrono::steady_clock::now();
        blockMicros.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());

        output.copyFrom (0, start, block, 0, 0, len);
        output.copyFrom (1, start, block, 1, 0, len);
    }

    std::cout << "Levels:\n";
    printLevels ("input ", input);
    printLevels ("output", output);

    // CPU: time per block against the real-time deadline (the block's duration).
    auto sorted = blockMicros;
    std::sort (sorted.begin(), sorted.end());
    const auto deadline = 1.0e6 * o.blockSize / sampleRate;
    const auto mean = std::accumulate (sorted.begin(), sorted.end(), 0.0) / (double) sorted.size();
    const auto p99 = sorted[(size_t) (0.99 * (double) (sorted.size() - 1))];
    const auto worst = sorted.back();

    std::cout << "CPU (" << o.slots << " amp" << (o.slots == 1 ? "" : "s") << " loaded, 1 running" << ", " << o.blockSize
              << "-sample blocks, deadline " << juce::String (deadline, 0) << " us):\n"
              << "  mean " << juce::String (mean, 1) << " us (" << juce::String (100.0 * mean / deadline, 1)
              << "% of deadline), p99 " << juce::String (p99, 1) << " us, worst " << juce::String (worst, 1)
              << " us\n"
              << "  real-time factor " << juce::String (deadline / mean, 1) << "x\n";

    if (! writeFloatWav (o.output, output, sampleRate))
    {
        std::cerr << "Couldn't write " << o.output.getFullPathName() << "\n";
        return 1;
    }

    std::cout << "Wrote " << o.output.getFullPathName() << " (32-bit float, stereo)\n";

    if (o.compare != juce::File())
    {
        double refRate = 0.0;
        const auto reference = readMono (o.compare, refRate);
        const auto n = juce::jmin (reference.getNumSamples(), numSamples);

        if (n == 0)
        {
            std::cerr << "Couldn't read " << o.compare.getFullPathName() << "\n";
            return 1;
        }

        double errorSquares = 0.0, refSquares = 0.0, maxError = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double e = (double) output.getSample (0, i) - (double) reference.getSample (0, i);
            errorSquares += e * e;
            refSquares += (double) reference.getSample (0, i) * reference.getSample (0, i);
            maxError = juce::jmax (maxError, std::abs (e));
        }

        std::cout << "Compared with " << o.compare.getFileName() << " over " << n << " samples:\n"
                  << "  max abs difference " << juce::String (maxError, 9) << " ("
                  << juce::String (toDb (maxError), 1) << " dBFS)\n"
                  << "  difference relative to reference RMS: "
                  << juce::String (toDb (std::sqrt (errorSquares / refSquares)), 1) << " dB\n";
    }

    return 0;
}
