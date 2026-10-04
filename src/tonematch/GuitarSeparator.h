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

    /// The worker rule (docs/TONE_MATCH.md, "Memory"): each worker holds about workerMegabytes while it
    /// runs (measured: the peak grows by 1.9 to 2.1 GB per worker on top of 1.3 GB for the model and the
    /// app's share). The workers get at most memoryBudget of the machine's physical memory, and never
    /// more than is free right now (less a reserve), 1 to maxWorkers, and at most half the cores.
    static constexpr int workerMegabytes = 2200;
    static constexpr double memoryBudget = 0.4;
    static constexpr int reserveMegabytes = 1500;
    static constexpr int maxWorkers = 4;

    /// The download: up to downloadAttempts tries, each resuming where the last stopped (an HTTP Range
    /// request), each allowed downloadTimeoutMs without a byte before it counts as failed.
    int downloadAttempts = 5;
    int downloadTimeoutMs = 60000;
    int retryDelayMs = 2000; ///< doubled after each failed try (the tests shorten it)

    /// folder: where the converted model lives (the app passes <data folder>/Separation).
    explicit GuitarSeparator (juce::File folder);
    ~GuitarSeparator();

    static juce::File defaultFolder();

    juce::File getFolder() const { return folder; }
    juce::File modelFile() const { return folder.getChildFile ("htdemucs_6s.demucscpp.bin"); }
    bool isInstalled() const { return modelFile().existsAsFile(); }

    /// What happened, with the details a one-line message can't hold: the model folder's parent's
    /// separation-log.txt (~/Library/Application Support/BellyDSP/separation-log.txt in the app). Kept
    /// under maxLogBytes: past that, it's renamed to separation-log.old.txt and a new one starts.
    juce::File logFile() const { return folder.getParentDirectory().getChildFile ("separation-log.txt"); }
    static constexpr juce::int64 maxLogBytes = 256 * 1024;
    void log (const juce::String& message) const;

    /// Downloads the weights from `url` (weightsUrl in the app; the tests point it at a local server or
    /// file), checks the size and SHA-256, and converts them. Progress 0..1. Returns an empty string on
    /// success, otherwise what failed, in words for the page (and the details in the log).
    juce::String install (const juce::String& url, const std::atomic<bool>& cancel, const ProgressFn& progress);

    /// safetensors (the download) to demucs.cpp's file: a magic number ("dmc6"), then per tensor its
    /// squeezed shape, its name, and its f16 data, exactly as demucs.cpp's convert-pth-to-ggml.py writes it.
    static juce::String convert (const juce::File& safetensors, const juce::File& out);

    static juce::String sha256Of (const juce::File& file);

    /// Loads the model (about 100 MB of float weights) if it isn't loaded. Empty string on success. A
    /// model file that doesn't load is deleted, so the next try downloads it again.
    juce::String load();
    bool isLoaded() const { return model != nullptr; }

    /// The worker count for a machine with physicalMB of memory, availableMB free now, and `cores`.
    static int chooseWorkers (juce::int64 physicalMB, juce::int64 availableMB, int cores);
    /// Memory the system could hand out now without swapping: free, inactive, purgeable, and file-backed
    /// pages (macOS); elsewhere the physical memory.
    static juce::int64 availableMemoryMB();
    static juce::int64 physicalMemoryMB();

    /// The guitar stem of a 48 kHz mix: resampled to 44.1 kHz, split into overlapping 10 s parts that
    /// `workers` workers run in parallel (each part through demucs.cpp's own 7.8 s segmentation; 0: chosen
    /// by chooseWorkers), crossfaded back together, the guitar source mixed to mono and resampled to 48 kHz.
    /// Stereo in (right empty: mono, fed to both of Demucs's channels). Empty on cancel or failure (and
    /// `error` says which). A part with no signal at all isn't run (Demucs divides by the part's standard
    /// deviation, so digital silence would come back as NaN): its stem is silence.
    std::vector<float> separate (const std::vector<float>& left, const std::vector<float>& right, const std::atomic<bool>& cancel,
                                 const ProgressFn& progress, juce::String& error, int workers = 0);
    std::vector<float> separate (const std::vector<float>& mono, const std::atomic<bool>& cancel, const ProgressFn& progress,
                                 juce::String& error, int workers = 0)
    {
        return separate (mono, {}, cancel, progress, error, workers);
    }

    /// The workers the last separation used.
    int getLastWorkers() const noexcept { return lastWorkers; }

    /// For tests: makes the next separation's workers fail as if out of memory (each throws
    /// std::bad_alloc while this many or more run at once; 1: every worker fails).
    int simulateOutOfMemoryAtWorkers = 0;

private:
    juce::String download (const juce::String& url, const juce::File& part, const std::atomic<bool>& cancel, const ProgressFn& progress);
    std::vector<float> run (const std::vector<float>& left441, const std::vector<float>& right441, const std::atomic<bool>& cancel,
                            const ProgressFn& progress, juce::String& error, int workers, bool& outOfMemory);

    juce::File folder;
    std::unique_ptr<demucscpp::demucs_model> model;
    int lastWorkers = 0;
};

} // namespace ampsim::tonematch
