// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "PreviewPlayer.h"

#include <cmath>
#include <limits>

namespace ampsim
{

namespace
{
/// Linear interpolation in a table sampled every `hop` from `offset`, clamped at both ends.
double lookUp (const std::vector<double>& table, double hop, double offset, double position)
{
    if (table.empty())
        return position;
    const auto f = juce::jlimit (0.0, (double) (table.size() - 1), (position - offset) / hop);
    const auto i = (size_t) f;
    if (i + 1 >= table.size())
        return table.back();
    const auto frac = f - (double) i;
    return table[i] + frac * (table[i + 1] - table[i]);
}

/// The non-negative remainder (C++'s % keeps the dividend's sign).
int64_t positiveModulo (int64_t a, int64_t m)
{
    const auto r = a % m;
    return r < 0 ? r + m : r;
}

float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }
} // namespace

PreviewPlayer::PreviewPlayer()
{
    for (auto& g : sourceGainDb)
        g.store (0.0f);
    for (size_t c = 0; c < (size_t) numClocks; ++c)
    {
        loopStart[c].store (0);
        loopEnd[c].store (std::numeric_limits<int64_t>::max() / 4);
    }
}

PreviewPlayer::~PreviewPlayer()
{
    // The audio thread is gone by now (the chain is being destroyed with the processor).
    if (previous != current)
        delete previous;
    delete current;
}

void PreviewPlayer::setMaterial (std::unique_ptr<Material> material)
{
    handoff.publish (std::move (material));
}

void PreviewPlayer::setSource (int index) noexcept
{
    source.store (juce::jlimit (0, numSources - 1, index));
}

void PreviewPlayer::setCountIn (int beats, double beatSamples) noexcept
{
    countInBeatSamples.store (std::max (1.0, beatSamples));
    countInBeats.store (juce::jlimit (0, maxCountInBeats, beats));
}

void PreviewPlayer::setLoop (int clock, int64_t start, int64_t end) noexcept
{
    const auto c = (size_t) juce::jlimit (0, numClocks - 1, clock);
    start = std::max<int64_t> (0, start);
    end = std::max (end, start + (int64_t) std::ceil (minLoopSeconds * sampleRate));
    loopStart[c].store (start);
    loopEnd[c].store (end);
}

void PreviewPlayer::setSourceGainDb (int index, float db) noexcept
{
    sourceGainDb[(size_t) juce::jlimit (0, numSources - 1, index)].store (db);
}

double PreviewPlayer::alignedPosition (const Material& m, int fromClock, double position)
{
    if (! m.aligned)
        return position;
    return lookUp (fromClock == 0 ? m.diAtTarget : m.targetAtDi, m.hop, m.offset, position);
}

void PreviewPlayer::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    fadeSamples = juce::jmax (1, juce::roundToInt (crossfadeSeconds * sampleRate));
    phaseStep = 1.0 / fadeSamples;
    liveGain.reset (sampleRate, liveFadeSeconds);
    liveGain.setCurrentAndTargetValue (1.0f);
    level.reset (sampleRate, gainSeconds);
    level.setCurrentAndTargetValue (dbToGain (levelDb.load()));
    clickLevel.reset (sampleRate, gainSeconds);
    clickLevel.setCurrentAndTargetValue (dbToGain (clickLevelDb.load()));
    for (size_t s = 0; s < sourceGain.size(); ++s)
    {
        sourceGain[s].reset (sampleRate, gainSeconds);
        sourceGain[s].setCurrentAndTargetValue (dbToGain (sourceGainDb[s].load()));
    }
    reset();
}

void PreviewPlayer::reset()
{
    for (auto& v : voices)
        v.active = false;
    lead = -1;
    wasPlaying = false;
    ended = false;
    countIn.active = false;
    clickBuffer = nullptr;
    clickMaterial = nullptr;
    countInShown.store (0);
    playheadPosition.store (-1);
    liveGain.setCurrentAndTargetValue (1.0f);
    active.store (false);
}

bool PreviewPlayer::usesMaterial (const Material* m) const noexcept
{
    for (const auto& v : voices)
        if (v.active && v.material == m)
            return true;
    return clickBuffer != nullptr && clickMaterial == m;
}

int PreviewPlayer::spawn (const Material* material, int sourceIndex, int64_t position, double startPhase, int64_t delay) noexcept
{
    // A free voice, or else the quietest (only when four are already sounding: switching faster than the
    // 20 ms fades). Taking a voice cuts it, so the quietest is the least audible one to cut.
    int k = -1;
    for (int i = 0; i < maxVoices && k < 0; ++i)
        if (! voices[(size_t) i].active)
            k = i;
    if (k < 0)
    {
        k = 0;
        for (int i = 1; i < maxVoices; ++i)
            if (voices[(size_t) i].phase < voices[(size_t) k].phase)
                k = i;
    }
    voices[(size_t) k] = { material, sourceIndex, position, startPhase, +1, true, std::max<int64_t> (0, delay), false };
    return k;
}

int64_t PreviewPlayer::intoLoop (int clock, int64_t position) const noexcept
{
    // A voice must start at least one fade before the loop's end, so its wrap has room to fade.
    const auto a = loopA[(size_t) clock], b = loopB[(size_t) clock];
    const auto usable = b - a - fadeSamples;
    if (onceNow)
        return juce::jlimit (a, b - 1, position); // once: no wrapping; past the end it just fades out
    if (position >= a && position < a + usable)
        return position;
    return a + positiveModulo (position - a, usable);
}

int64_t PreviewPlayer::convert (const Voice& from, int toSource, const Material* toMaterial) const noexcept
{
    const auto& m = *toMaterial;
    const auto fromClock = m.clock[(size_t) from.source], toClock = m.clock[(size_t) toSource];
    auto p = (double) from.position;
    if (fromClock != toClock)
    {
        if (m.aligned)
            p = alignedPosition (m, fromClock, p);
        else
        {
            // Nothing corresponds (anything mode): keep how far into the loop it was.
            const auto fromA = loopA[(size_t) fromClock];
            const auto toLength = loopB[(size_t) toClock] - loopA[(size_t) toClock];
            p = (double) (loopA[(size_t) toClock] + positiveModulo ((int64_t) p - fromA, toLength));
        }
    }
    return intoLoop (toClock, (int64_t) std::llround (p));
}

void PreviewPlayer::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    // ---- New material: fade every sounding voice over to it at the same place. The old material is
    // handed back once nothing reads it (one handover at a time: a newer one waits in the Handoff).
    if (previous != nullptr && ! usesMaterial (previous) && handoff.retire (previous))
        previous = nullptr;
    if (previous == nullptr)
    {
        if (auto* incoming = handoff.take())
        {
            if (current != nullptr && usesMaterial (current))
            {
                previous = current;
                current = incoming;
                std::array<bool, maxVoices> moving {};
                for (size_t i = 0; i < voices.size(); ++i)
                {
                    if (voices[i].active && voices[i].material == previous && voices[i].delay > 0)
                        voices[i].material = current; // still counting in, silent: it simply starts on the new one
                    moving[i] = voices[i].active && voices[i].material == previous && voices[i].direction >= 0;
                }
                for (size_t i = 0; i < voices.size(); ++i)
                    if (moving[i])
                    {
                        const auto v = voices[i];
                        fadeOut (voices[i]);
                        const auto k = spawn (current, v.source, v.position, 0.0);
                        if (lead == (int) i)
                            lead = k;
                    }
            }
            else
            {
                if (current != nullptr && ! handoff.retire (current))
                    previous = current; // the return slot is busy: hand it back next buffer
                current = incoming;
            }
        }
    }

    // ---- This buffer's requests.
    songStartIndex = -1;
    const auto play = playing.load (std::memory_order_acquire) && current != nullptr;
    const auto wanted = source.load (std::memory_order_relaxed);
    onceNow = playOnce.load (std::memory_order_relaxed);
    const auto minLoop = (int64_t) (2 * fadeSamples + 1);
    for (size_t c = 0; c < (size_t) numClocks; ++c)
    {
        loopA[c] = loopStart[c].load (std::memory_order_relaxed);
        loopB[c] = std::max (loopEnd[c].load (std::memory_order_relaxed), loopA[c] + minLoop); // the two atomics may disagree for a buffer
    }
    level.setTargetValue (dbToGain (levelDb.load (std::memory_order_relaxed)));
    clickLevel.setTargetValue (dbToGain (clickLevelDb.load (std::memory_order_relaxed)));
    for (size_t s = 0; s < sourceGain.size(); ++s)
        sourceGain[s].setTargetValue (dbToGain (sourceGainDb[s].load (std::memory_order_relaxed)));

    const auto fadeAll = [this] {
        for (auto& v : voices)
            if (v.active)
                fadeOut (v);
        countIn.active = false; // a click already sounding finishes; no more follow
        countInShown.store (0, std::memory_order_relaxed);
    };
    const auto leadSounding = lead >= 0 && voices[(size_t) lead].active && voices[(size_t) lead].direction >= 0;
    const auto serial = startSerial.load (std::memory_order_acquire);
    const auto fresh = ! wasPlaying || serial != seenStartSerial; // a start, or a start again from the top
    auto started = play;
    if (play && fresh && handoff.hasPending())
        started = wasPlaying; // material is on its way: start next buffer, on it
    else if (play && (fresh || (! leadSounding && ! ended)))
    {
        // Start (from the loop's start, after the count-in if one is set), or start again after the voice
        // was lost (no count-in then).
        // Nothing sounding yet: the levels start where they're set (the start fades in anyway), rather
        // than gliding there from wherever the last play left them.
        if (! wasPlaying)
        {
            level.setCurrentAndTargetValue (level.getTargetValue());
            clickLevel.setCurrentAndTargetValue (clickLevel.getTargetValue());
            for (auto& g : sourceGain)
                g.setCurrentAndTargetValue (g.getTargetValue());
        }
        fadeAll();
        seenStartSerial = serial;
        const auto clock = current->clock[(size_t) wanted];
        const auto beats = fresh ? countInBeats.load (std::memory_order_relaxed) : 0;
        const auto beat = countInBeatSamples.load (std::memory_order_relaxed);
        const auto delay = beats > 0 ? (int64_t) std::llround ((double) beats * beat) : (int64_t) 0;
        lead = spawn (current, wanted, loopA[(size_t) clock], 0.0, delay);
        voices[(size_t) lead].fresh = true;
        countIn = { beats > 0, beats, 0, beat, 0, delay };
        ended = false;
        finished.store (false, std::memory_order_release);
        lastSource = wanted;
    }
    else if (! play && wasPlaying)
    {
        fadeAll();
        lead = -1;
    }
    else if (play && wanted != lastSource && leadSounding)
    {
        auto& v = voices[(size_t) lead];
        if (v.delay > 0)
            v.source = wanted; // still counting in, silent: it starts as the new source
        else
        {
            // Switch: the new source starts where the old one is (its own clock), crossfading.
            const auto position = convert (v, wanted, current);
            fadeAll();
            lead = spawn (current, wanted, position, 0.0);
        }
        lastSource = wanted;
    }
    wasPlaying = started;
    liveGain.setTargetValue (play && muteLive.load (std::memory_order_relaxed) ? 0.0f : 1.0f);

    bool anyVoice = false;
    for (const auto& v : voices)
        anyVoice = anyVoice || v.active;
    if (! anyVoice && clickBuffer == nullptr && ! countIn.active && ! liveGain.isSmoothing() && liveGain.getCurrentValue() == 1.0f)
    {
        // Idle: the audio passes untouched, bit for bit.
        level.setCurrentAndTargetValue (level.getTargetValue());
        clickLevel.setCurrentAndTargetValue (clickLevel.getTargetValue());
        for (auto& g : sourceGain)
            g.setCurrentAndTargetValue (g.getTargetValue());
        active.store (false, std::memory_order_relaxed);
        soundingSource.store (-1, std::memory_order_relaxed);
        playheadPosition.store (-1, std::memory_order_relaxed);
        return;
    }
    active.store (true, std::memory_order_relaxed);

    // ---- Per sample: loop wraps, the voices, the live guitar's fade.
    auto* left = block.getChannelPointer (0);
    auto* right = block.getChannelPointer (1);
    const auto halfPi = juce::MathConstants<double>::halfPi;
    for (size_t n = 0; n < block.getNumSamples(); ++n)
    {
        for (int k = 0; k < maxVoices; ++k)
        {
            auto& v = voices[(size_t) k];
            if (! v.active || v.direction < 0 || v.delay > 0)
                continue;
            // A fade before the loop's end (or outside the loop, if the range moved): this voice fades out,
            // reaching the end as its gain reaches 0, while the same source fades in from the start. Once:
            // it just fades out, and the play has finished.
            const auto clock = v.material->clock[(size_t) v.source];
            const auto a = loopA[(size_t) clock], b = loopB[(size_t) clock];
            if (v.position >= b - fadeSamples || v.position < a)
            {
                const auto copy = v;
                fadeOut (v);
                if (onceNow)
                {
                    if (lead == k)
                    {
                        ended = true;
                        finished.store (true, std::memory_order_release);
                    }
                }
                else
                {
                    const auto again = spawn (copy.material, copy.source, a, 0.0);
                    if (lead == k)
                        lead = again;
                }
            }
        }

        std::array<float, numSources> g {};
        for (size_t s = 0; s < g.size(); ++s)
            g[s] = sourceGain[s].getNextValue();

        double sum = 0.0;
        for (int k = 0; k < maxVoices; ++k)
        {
            auto& v = voices[(size_t) k];
            if (! v.active)
                continue;
            if (v.delay > 0)
            {
                --v.delay; // counting in: silent, and its position waits
                continue;
            }
            if (v.fresh)
            {
                v.fresh = false;
                if (songStartIndex < 0)
                    songStartIndex = (int) n; // a start's first sample: this one
            }
            const auto* audio = v.material->audio[(size_t) v.source].get();
            const auto x = (audio != nullptr && v.position >= 0 && v.position < (int64_t) audio->size()) ? (*audio)[(size_t) v.position] : 0.0f;
            sum += (double) x * std::sin (v.phase * halfPi) * (double) g[(size_t) v.source];
            ++v.position;
            if (v.direction != 0)
            {
                v.phase += v.direction * phaseStep;
                if (v.phase >= 1.0)
                {
                    v.phase = 1.0;
                    v.direction = 0;
                }
                else if (v.phase <= 0.0)
                {
                    v.active = false;
                    if (lead == k)
                        lead = -1;
                }
            }
        }

        // The count-in's clicks: click k starts round(k B) samples after the start, the first accented.
        if (countIn.active)
        {
            if (countIn.next < countIn.beats && countIn.sample == (int64_t) std::llround ((double) countIn.next * countIn.beat))
            {
                const auto& buffer = countIn.next == 0 ? current->clickAccent : current->click;
                clickBuffer = buffer != nullptr && ! buffer->empty() ? buffer.get() : nullptr;
                clickMaterial = current;
                clickPosition = 0;
                countInShown.store (countIn.beats - countIn.next, std::memory_order_relaxed);
                ++countIn.next;
            }
            if (++countIn.sample >= countIn.songAt)
            {
                countIn.active = false;
                countInShown.store (0, std::memory_order_relaxed);
            }
        }
        float click = 0.0f;
        const auto clickGain = clickLevel.getNextValue();
        if (clickBuffer != nullptr)
        {
            click = (*clickBuffer)[clickPosition] * clickGain;
            if (++clickPosition >= clickBuffer->size())
            {
                clickBuffer = nullptr;
                clickMaterial = nullptr;
            }
        }

        const auto out = (float) sum * level.getNextValue() + click;
        const auto live = liveGain.getNextValue();
        left[n] = left[n] * live + out;
        right[n] = right[n] * live + out;
    }

    if (lead >= 0 && voices[(size_t) lead].active)
    {
        const auto& v = voices[(size_t) lead];
        const auto clock = v.material->clock[(size_t) v.source];
        const auto a = loopA[(size_t) clock], b = loopB[(size_t) clock];
        playhead.store ((float) juce::jlimit (0.0, 1.0, (double) (v.position - a) / (double) std::max<int64_t> (1, b - a)), std::memory_order_relaxed);
        soundingSource.store (v.source, std::memory_order_relaxed);
        playheadPosition.store (v.position, std::memory_order_relaxed);
    }
    else
    {
        soundingSource.store (-1, std::memory_order_relaxed);
        playheadPosition.store (-1, std::memory_order_relaxed);
    }
}

} // namespace ampsim
