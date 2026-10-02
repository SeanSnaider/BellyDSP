#include "Cab.h"

#include <cmath>

namespace ampsim
{

namespace
{
std::uint64_t pack (const Cab::Alignment& a)
{
    return (std::uint64_t) (std::uint16_t) a.delayMic1
           | ((std::uint64_t) (std::uint16_t) a.delayMic2 << 16)
           | ((std::uint64_t) (a.invertMic2 ? 1 : 0) << 32)
           | ((std::uint64_t) (a.valid ? 1 : 0) << 33);
}

Cab::Alignment unpack (std::uint64_t bits)
{
    Cab::Alignment a;
    a.delayMic1 = (int) (bits & 0xffff);
    a.delayMic2 = (int) ((bits >> 16) & 0xffff);
    a.invertMic2 = ((bits >> 32) & 1) != 0;
    a.valid = ((bits >> 33) & 1) != 0;
    return a;
}

constexpr int coefficientInterval = 32; // redesign knob-driven filters every 32 samples (foundation decisions)
constexpr float sqrt2 = 1.41421356f;

// 4th-order Butterworth as two 2nd-order sections: Q = 1 / (2 cos(pi/8)) and 1 / (2 cos(3 pi/8)).
constexpr double butterworth4Q1 = 0.54119610014619701;
constexpr double butterworth4Q2 = 1.3065629648763766;
constexpr double butterworth2Q = 0.70710678118654752;
} // namespace

// ---- Loading (loader thread) ------------------------------------------------------------------

CabIR::LoadResult Cab::loadCloseMic (int index, const juce::File& file, CabIR::Channel channel)
{
    index = juce::jlimit (0, numCloseMics - 1, index);
    auto result = closeMics[(size_t) index].loadFile (file, channel);
    if (result.ok)
    {
        const std::scoped_lock lock (packMutex);
        packs[(size_t) index].reset(); // a single IR: the mic is no longer movable
    }
    if (result.ok)
        realign();
    return result;
}

CabIR::LoadResult Cab::loadCloseMicPack (int index, const juce::File& folder, double x, double y)
{
    index = juce::jlimit (0, numCloseMics - 1, index);
    auto pack = std::make_unique<CabPack>();
    const auto packResult = pack->load (folder);

    if (! packResult.ok)
        return { false, packResult.message };

    {
        const std::scoped_lock lock (packMutex);
        packs[(size_t) index] = std::move (pack);
    }

    auto result = moveCloseMic (index, x, y);
    if (result.ok)
        result.message = packResult.message + "; mic at " + juce::String (x, 2) + ", " + juce::String (y, 2);
    return result;
}

CabIR::LoadResult Cab::moveCloseMic (int index, double x, double y)
{
    index = juce::jlimit (0, numCloseMics - 1, index);
    std::vector<float> ir;
    double rate = 0.0;
    juce::String description;

    {
        const std::scoped_lock lock (packMutex);
        const auto& pack = packs[(size_t) index];
        if (pack == nullptr)
            return { false, "This mic has no pack to move in" };
        ir = pack->irAt (x, y);
        rate = pack->getSampleRate();
        description = pack->describe();
    }

    juce::AudioBuffer<float> buffer (1, (int) ir.size());
    buffer.copyFrom (0, 0, ir.data(), (int) ir.size());
    auto result = closeMics[(size_t) index].loadSamples (std::move (buffer), rate, description);
    if (result.ok)
    {
        result.message = description + " at " + juce::String (x, 2) + ", " + juce::String (y, 2);
        realign();
    }
    return result;
}

bool Cab::hasPack (int index) const
{
    const std::scoped_lock lock (packMutex);
    return packs[(size_t) juce::jlimit (0, numCloseMics - 1, index)] != nullptr;
}

std::vector<CabPack::Point> Cab::getPackPoints (int index) const
{
    const std::scoped_lock lock (packMutex);
    const auto& pack = packs[(size_t) juce::jlimit (0, numCloseMics - 1, index)];
    return pack != nullptr ? pack->getPoints() : std::vector<CabPack::Point>();
}

CabPack::Layout Cab::getPackLayout (int index) const
{
    const std::scoped_lock lock (packMutex);
    const auto& pack = packs[(size_t) juce::jlimit (0, numCloseMics - 1, index)];
    return pack != nullptr ? pack->getLayout() : CabPack::Layout::single;
}

CabIR::LoadResult Cab::loadCloseMicSamples (int index, juce::AudioBuffer<float> samples, double rate, const juce::String& name)
{
    auto result = closeMics[(size_t) juce::jlimit (0, numCloseMics - 1, index)].loadSamples (std::move (samples), rate, name);
    if (result.ok)
        realign();
    return result;
}

CabIR::LoadResult Cab::loadRoom (const juce::File& file) { return room.loadFile (file); }

CabIR::LoadResult Cab::loadRoomSamples (juce::AudioBuffer<float> samples, double rate, const juce::String& name)
{
    return room.loadSamples (std::move (samples), rate, name);
}

Cab::Alignment Cab::getAlignment() const
{
    auto a = unpack (packedAlignment.load());
    a.correlation = alignedCorrelation.load();
    return a;
}

Cab::Alignment Cab::computeAlignment (const std::vector<float>& h1, const std::vector<float>& h2, int maxLag)
{
    // Cross-correlation r(tau) = sum_n h1[n] h2[n + tau]. If h2 is h1 delayed by d samples, r peaks at
    // tau = d; if h2 is also inverted, the peak is negative. Searching |r| finds both. Only the first
    // 8192 samples matter: the direct sound and early reflections decide how two mics line up.
    const auto length = (int) std::min<size_t> (8192, std::max (h1.size(), h2.size()));
    const auto at = [length] (const std::vector<float>& h, int i) { return i >= 0 && i < length && i < (int) h.size() ? (double) h[(size_t) i] : 0.0; };

    double energy1 = 0.0, energy2 = 0.0;
    for (int n = 0; n < length; ++n)
    {
        energy1 += at (h1, n) * at (h1, n);
        energy2 += at (h2, n) * at (h2, n);
    }

    Alignment a;
    if (energy1 <= 0.0 || energy2 <= 0.0)
        return a;

    double best = 0.0;
    int bestLag = 0;

    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double r = 0.0;
        for (int n = std::max (0, -lag); n < std::min (length, length - lag); ++n)
            r += at (h1, n) * at (h2, n + lag);

        if (std::abs (r) > std::abs (best))
        {
            best = r;
            bestLag = lag;
        }
    }

    a.valid = true;
    a.invertMic2 = best < 0.0;
    a.correlation = (float) (std::abs (best) / std::sqrt (energy1 * energy2));
    // Delay whichever mic arrives first, so the two line up.
    a.delayMic1 = std::min (maxMicDelaySamples, std::max (0, bestLag));
    a.delayMic2 = std::min (maxMicDelaySamples, std::max (0, -bestLag));
    return a;
}

void Cab::realign()
{
    const std::scoped_lock lock (realignMutex);
    const auto h1 = closeMics[0].getLoadedIR();
    const auto h2 = closeMics[1].getLoadedIR();

    Alignment a;
    if (! h1.empty() && ! h2.empty())
        a = computeAlignment (h1, h2, alignmentMaxLag);

    alignedCorrelation = a.correlation;
    packedAlignment = pack (a);
}

void Cab::collectGarbage()
{
    for (auto& mic : closeMics)
        mic.collectGarbage();
    room.collectGarbage();
}

// ---- Settings (audio thread) ------------------------------------------------------------------

void Cab::setCloseMic (int index, const CloseMicSettings& settings) { micSettings[(size_t) juce::jlimit (0, numCloseMics - 1, index)] = settings; }
void Cab::setRoom (const RoomSettings& settings) { roomSettings = settings; }
void Cab::setCuts (const CutSettings& settings) { cutSettings = settings; }

bool Cab::hasAnyImpulseResponse() const noexcept
{
    return closeMics[0].hasImpulseResponse() || closeMics[1].hasImpulseResponse() || room.hasImpulseResponse();
}

void Cab::applyTargets()
{
    const auto alignment = unpack (packedAlignment.load (std::memory_order_relaxed));
    const bool aligned = autoAlign && alignment.valid;

    for (int i = 0; i < numCloseMics; ++i)
    {
        const auto& s = micSettings[(size_t) i];
        auto& state = micStates[(size_t) i];

        // With auto alignment on, the alignment decides both mics' delays and mic 2's polarity.
        const bool invert = aligned ? (i == 1 && alignment.invertMic2) : s.invert;
        const auto delay = aligned ? (i == 0 ? alignment.delayMic1 : alignment.delayMic2) : s.delaySamples;

        const auto level = s.mute ? 0.0f : juce::Decibels::decibelsToGain (s.levelDb);
        const auto theta = (juce::jlimit (-1.0f, 1.0f, s.pan) + 1.0f) * juce::MathConstants<float>::pi / 4.0f;
        const auto sign = invert ? -1.0f : 1.0f;

        state.gainLeft.setTargetValue (sign * level * sqrt2 * std::cos (theta));
        state.gainRight.setTargetValue (sign * level * sqrt2 * std::sin (theta));
        state.delay.setDelay (delay);
    }

    roomState.gainLeft.setTargetValue (roomSettings.mute ? 0.0f : juce::Decibels::decibelsToGain (roomSettings.levelDb));
    roomState.delay.setDelay (juce::roundToInt (juce::jlimit (0.0, maxRoomPreDelayMs, (double) roomSettings.preDelayMs) * 0.001 * sampleRate));

    const auto applyCut = [] (CutFilter& cut, bool on, float hz, Slope slope)
    {
        cut.frequency.setTargetValue (juce::jlimit (20.0f, 20000.0f, hz));

        if (on != cut.on)
        {
            // Turning on from fully off: the filter's state is stale, so clear it, and start at the
            // current frequency rather than gliding from wherever it was when it was switched off.
            if (on && ! cut.wet.isSmoothing() && cut.wet.getCurrentValue() <= 0.0f)
            {
                cut.needsReset = true;
                cut.frequency.setCurrentAndTargetValue (cut.frequency.getTargetValue());
                cut.coefficientsDirty = true;
            }

            cut.on = on;
            cut.wet.setTargetValue (on ? 1.0f : 0.0f);
        }

        if (slope != cut.slope)
        {
            cut.slope = slope;
            for (auto& channel : cut.sections)
                channel[1].reset(); // the second section starts clean when it comes into use
            cut.coefficientsDirty = true;
        }
    };

    applyCut (lowCut, cutSettings.lowCutOn, cutSettings.lowCutHz, cutSettings.lowCutSlope);
    applyCut (highCut, cutSettings.highCutOn, cutSettings.highCutHz, cutSettings.highCutSlope);
}

// ---- Processing -------------------------------------------------------------------------------

void Cab::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    const auto fade = juce::roundToInt (fadeSeconds * sampleRate);
    settleSamples = juce::roundToInt (0.060 * sampleRate);

    for (auto& mic : closeMics)
        mic.prepare (sampleRate, maxBlockSize);
    room.prepare (sampleRate, maxBlockSize);

    micBuffers.setSize (2 * numCloseMics + 2, maxBlockSize);
    dryBuffer.setSize (2, maxBlockSize);

    for (auto* state : { &micStates[0], &micStates[1], &roomState })
    {
        state->gainLeft.reset (sampleRate, gainRampSeconds);
        state->gainRight.reset (sampleRate, gainRampSeconds);
        state->presence.reset (sampleRate, gainRampSeconds);
        state->presence.setCurrentAndTargetValue (0.0f);
        state->samplesSinceReady = 0;
    }

    for (auto& state : micStates)
        state.delay.prepare (maxMicDelaySamples, fade);
    roomState.delay.prepare (juce::roundToInt (maxRoomPreDelayMs * 0.001 * sampleRate), fade);

    passthrough.reset (sampleRate, gainRampSeconds);
    passthrough.setCurrentAndTargetValue (1.0f);

    for (auto* cut : { &lowCut, &highCut })
    {
        cut->frequency.reset (sampleRate, 0.025);
        cut->wet.reset (sampleRate, fadeSeconds);
        for (auto& channel : cut->sections)
            for (auto& section : channel)
                section.reset();
    }

    // Start at the current settings with no ramps: nothing is playing yet.
    applyTargets();
    for (auto* state : { &micStates[0], &micStates[1], &roomState })
    {
        state->gainLeft.setCurrentAndTargetValue (state->gainLeft.getTargetValue());
        state->gainRight.setCurrentAndTargetValue (state->gainRight.getTargetValue());
        state->delay.setDelayImmediately (state->delay.getDelay());
    }

    for (auto* cut : { &lowCut, &highCut })
    {
        cut->frequency.setCurrentAndTargetValue (cut->frequency.getTargetValue());
        cut->wet.setCurrentAndTargetValue (cut->on ? 1.0f : 0.0f);
    }

    updateCutCoefficients (lowCut, Svf::Type::highpass);
    updateCutCoefficients (highCut, Svf::Type::lowpass);

    // A mic whose IR is already installed (loaded before prepare) plays straight away.
    const std::array<std::pair<MicState*, CabIR*>, 3> mics { { { &micStates[0], &closeMics[0] }, { &micStates[1], &closeMics[1] }, { &roomState, &room } } };
    for (const auto& [state, mic] : mics)
        if (mic->hasImpulseResponse())
        {
            state->presence.setCurrentAndTargetValue (1.0f);
            state->samplesSinceReady = settleSamples;
            passthrough.setCurrentAndTargetValue (0.0f);
        }
}

void Cab::updatePresence (MicState& state, const CabIR& mic, int numSamples)
{
    // A mic fades in only once its first IR's engine is in and JUCE's 50 ms crossfade from its
    // identity engine has finished; until then its output (still raw amp) isn't mixed in.
    if (! mic.hasImpulseResponse())
        return;

    if (mic.isEngineReady() && state.samplesSinceReady < settleSamples)
    {
        state.samplesSinceReady += numSamples;
        if (state.samplesSinceReady >= settleSamples)
            state.presence.setTargetValue (1.0f);
    }
}

void Cab::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    applyTargets();

    const auto n = (int) block.getNumSamples();
    auto* left = block.getChannelPointer (0);
    auto* right = block.getChannelPointer (1);
    auto buffers = juce::dsp::AudioBlock<float> (micBuffers).getSubBlock (0, (size_t) n);

    // Pick up IRs loaded since the last buffer, including a mic's first one.
    for (auto& mic : closeMics)
        mic.pollPendingIR();
    room.pollPendingIR();

    // Run every mic that has an IR, from the same mono input, into its own buffers.
    for (size_t i = 0; i < (size_t) numCloseMics; ++i)
    {
        if (! closeMics[i].hasImpulseResponse())
            continue;

        auto mic = buffers.getSingleChannelBlock (2 * i);
        std::copy (left, left + n, mic.getChannelPointer (0));
        closeMics[i].process (mic, {});
        updatePresence (micStates[i], closeMics[i], n);
    }

    if (room.hasImpulseResponse())
    {
        auto roomBlock = buffers.getSubsetChannelBlock (4, 2);
        auto* roomIn = roomBlock.getChannelPointer (0);
        for (int s = 0; s < n; ++s)
            roomIn[s] = roomState.delay.processSample (left[s]); // pre-delay
        room.process (roomBlock, {});
        updatePresence (roomState, room, n);
    }

    bool anyPresent = false;
    for (auto* state : { &micStates[0], &micStates[1], &roomState })
        anyPresent = anyPresent || state->presence.getTargetValue() > 0.0f;
    passthrough.setTargetValue (anyPresent ? 0.0f : 1.0f);

    // Mix. Mics that never got an IR contribute nothing. A centred mic and the passthrough both send
    // the signal at unity to each side.
    const float* micOut[2] = { buffers.getChannelPointer (0), buffers.getChannelPointer (2) };
    const auto* roomLeft = buffers.getChannelPointer (4);
    const auto* roomRight = buffers.getChannelPointer (5);
    const bool micActive[2] = { closeMics[0].hasImpulseResponse(), closeMics[1].hasImpulseResponse() };
    const bool roomActive = room.hasImpulseResponse();
    for (int s = 0; s < n; ++s)
    {
        const auto x = left[s];
        const auto p = passthrough.getNextValue();
        auto l = p * x, r = p * x;

        for (size_t i = 0; i < (size_t) numCloseMics; ++i)
        {
            if (! micActive[i])
                continue;

            auto& state = micStates[i];
            const auto d = state.delay.processSample (micOut[i][s]) * state.presence.getNextValue();
            l += state.gainLeft.getNextValue() * d;
            r += state.gainRight.getNextValue() * d;
        }

        if (roomActive)
        {
            const auto g = roomState.gainLeft.getNextValue() * roomState.presence.getNextValue();
            l += g * roomLeft[s];
            r += g * roomRight[s];
        }

        left[s] = l;
        right[s] = r;
    }

    processCut (lowCut, Svf::Type::highpass, left, right, n);
    processCut (highCut, Svf::Type::lowpass, left, right, n);
}

void Cab::updateCutCoefficients (CutFilter& cut, Svf::Type type)
{
    const double f = cut.frequency.getCurrentValue();

    cut.coefficientsDirty = false;

    for (auto& channel : cut.sections)
    {
        if (cut.slope == Slope::db12)
        {
            channel[0].setCoefficients (Svf::design (type, f, butterworth2Q, 0.0, sampleRate));
        }
        else
        {
            channel[0].setCoefficients (Svf::design (type, f, butterworth4Q1, 0.0, sampleRate));
            channel[1].setCoefficients (Svf::design (type, f, butterworth4Q2, 0.0, sampleRate));
        }
    }
}

void Cab::processCut (CutFilter& cut, Svf::Type type, float* left, float* right, int numSamples)
{
    const bool fading = cut.wet.isSmoothing();

    if (! cut.on && ! fading)
        return; // fully off: skip

    if (cut.needsReset)
    {
        for (auto& channel : cut.sections)
            for (auto& section : channel)
                section.reset();
        cut.needsReset = false;
    }

    if (fading)
    {
        dryBuffer.copyFrom (0, 0, left, numSamples);
        dryBuffer.copyFrom (1, 0, right, numSamples);
    }

    if (cut.coefficientsDirty)
        updateCutCoefficients (cut, type);

    const bool fourthOrder = cut.slope == Slope::db24;
    float* channels[2] = { left, right };

    for (int s = 0; s < numSamples; ++s)
    {
        // Frequency moves: redesign every 32 samples, as for every other knob-driven filter.
        if (cut.samplesUntilUpdate-- <= 0)
        {
            cut.samplesUntilUpdate = coefficientInterval - 1;
            if (cut.frequency.isSmoothing())
            {
                cut.frequency.skip (coefficientInterval);
                updateCutCoefficients (cut, type);
            }
        }

        for (size_t ch = 0; ch < 2; ++ch)
        {
            auto v = (double) channels[ch][s];
            v = cut.sections[ch][0].processSample (v);
            if (fourthOrder)
                v = cut.sections[ch][1].processSample (v);
            channels[ch][s] = (float) v;
        }
    }

    if (fading)
    {
        // On/off crossfade between the unfiltered and filtered signal, linear over 10 ms.
        for (int s = 0; s < numSamples; ++s)
        {
            const auto w = cut.wet.getNextValue();
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto dry = dryBuffer.getSample (ch, s);
                channels[ch][s] = dry + w * (channels[ch][s] - dry);
            }
        }
    }
}

void Cab::reset()
{
    for (auto& mic : closeMics)
        mic.reset();
    room.reset();

    for (auto* state : { &micStates[0], &micStates[1], &roomState })
    {
        state->gainLeft.setCurrentAndTargetValue (state->gainLeft.getTargetValue());
        state->gainRight.setCurrentAndTargetValue (state->gainRight.getTargetValue());
        state->delay.reset();
        state->presence.setCurrentAndTargetValue (state->presence.getTargetValue());
    }

    for (auto* cut : { &lowCut, &highCut })
        for (auto& channel : cut->sections)
            for (auto& section : channel)
                section.reset();
}

} // namespace ampsim
