// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <vector>

/// Tone match: reading a target or a DI from a file. Every format JUCE knows on this platform: WAV,
/// AIFF, and FLAC everywhere; on macOS, MP3, M4A (AAC and ALAC), and CAF through Core Audio
/// (CoreAudioFormat); on Windows, MP3, WMA, and M4A through Windows Media Foundation
/// (JUCE_USE_WINDOWS_MEDIA_FORMAT). Mixed to mono and resampled to 48 kHz.
namespace ampsim::tonematch
{

struct AudioFileInput
{
    bool ok = false;
    juce::String error, formatName;
    double sourceSampleRate = 0.0;
    int sourceChannels = 0;
    std::vector<float> samples; ///< 48 kHz mono
    double seconds() const { return (double) samples.size() / 48000.0; }

    /// Any thread but the audio thread (it reads the whole file). maxSeconds caps how much is read.
    static AudioFileInput read (const juce::File& file, double maxSeconds = 600.0);

    /// The file name patterns the formats above accept, for a file chooser ("*.wav;*.aiff;...").
    static juce::String wildcard();

    /// Mono, sampleRate -> 48 kHz (JUCE's ResamplingAudioSource, which low-passes when it downsamples).
    static std::vector<float> resampleTo48k (const std::vector<float>& x, double sampleRate);
};

} // namespace ampsim::tonematch
