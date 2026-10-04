// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Progress.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <vector>

namespace demucscpp
{
struct demucs_model;
}

/// Tone match, Stage C: pulling the guitar out of a song before matching (docs/TONE_MATCH.md).
///
/// The separation is Demucs v4, the hybrid transformer model with six sources ("htdemucs_6s": drums, bass,
/// other, vocals, guitar, piano; Rouard, Massa, and Defossez, "Hybrid Transformers for Music Source
/// Separation", ICASSP 2023), run by demucs.cpp (third_party/demucs.cpp, MIT, on Eigen). Demucs gives every
/// guitar in the mix, not the lead alone: the page asks for a section where the lead dominates.
///
/// The weights (55 MB) are never bundled. On first use they're downloaded from their upstream source, the
/// Demucs author's Hugging Face repository, pinned to a commit and checked against a SHA-256, into the
/// BellyDSP data folder; then converted once into demucs.cpp's tensor file (the same f16 tensors, a different
/// container) and the download is deleted. ASSUMPTIONS TM and the BUILD_PLAN decision log say why they
/// aren't re-hosted.
///
/// Everything here runs on worker threads, never the audio thread.
namespace ampsim::tonematch
{

class GuitarSeparator
{
public:
    /// The pinned weights: htdemucs_6s as safetensors, at commit 3c5ee475 of adefossez/HTDemucs-6s.
    static constexpr const char* weightsUrl =
        "https://huggingface.co/adefossez/HTDemucs-6s/resolve/3c5ee475be622df764938de97e4281a7b07ffa58/5c90dfd2.safetensors";
    static constexpr const char* weightsSha256 = "d2a1745f0744721f6b8ca5bf469b67c651ea5ed1b52998cab033b2158609d411";
    static constexpr juce::int64 weightsBytes = 54885744;
    static constexpr int guitarSource = 4; // drums, bass, other, vocals, guitar, piano
    static constexpr double modelSampleRate = 44100.0;
    static constexpr double partSeconds = 10.0;

    /// folder: where the converted model lives (the app passes <data folder>/Separation).
    explicit GuitarSeparator (juce::File folder);
    ~GuitarSeparator();

    static juce::File defaultFolder();

    juce::File modelFile() const { return folder.getChildFile ("htdemucs_6s.demucscpp.bin"); }
    bool isInstalled() const { return modelFile().existsAsFile(); }

    /// Downloads the weights from `url` (weightsUrl in the app; the tests point it at a local copy),
    /// checks the size and SHA-256, and converts them. Progress 0..1. Returns an empty string on success,
    /// otherwise what went wrong (a failed check deletes the download).
    juce::String install (const juce::String& url, const std::atomic<bool>& cancel, const ProgressFn& progress);

    /// safetensors (the download) to demucs.cpp's file: a magic number ("dmc6"), then per tensor its
    /// squeezed shape, its name, and its f16 data, exactly as demucs.cpp's convert-pth-to-ggml.py writes it.
    static juce::String convert (const juce::File& safetensors, const juce::File& out);

    static juce::String sha256Of (const juce::File& file);

    /// Loads the model (about 100 MB of float weights) if it isn't loaded. Empty string on success.
    juce::String load();
    bool isLoaded() const { return model != nullptr; }

    /// The guitar stem of x (48 kHz mono): resampled to 44.1 kHz stereo, split into overlapping 10 s parts
    /// that `threads` workers run in parallel (each part through demucs.cpp's own 7.8 s segmentation;
    /// 0: chosen from the memory and cores), crossfaded back together, the guitar source mixed to mono and
    /// resampled to 48 kHz. Empty on cancel or failure (and `error` says which).
    std::vector<float> separate (const std::vector<float>& x, const std::atomic<bool>& cancel, const ProgressFn& progress,
                                 juce::String& error, int threads = 0);

private:
    juce::File folder;
    std::unique_ptr<demucscpp::demucs_model> model;
};

} // namespace ampsim::tonematch
