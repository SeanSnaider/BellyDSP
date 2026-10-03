// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <utility>
#include <vector>

namespace ampsim
{

/// A cab pack: IRs of one cab (and one mic model) captured at different mic positions, placed on a
/// map (BUILD_PLAN "Movable mics"). The horizontal axis x runs across the speaker, 0 at the centre of
/// the cone (the dust cap) to 1 at its edge; the vertical axis y is distance from the grille, 0 for
/// the closest capture to 1 for the farthest.
///
/// The map comes from a manifest, cabpack.json, in the pack's folder:
///     { "name": "...", "points": [ { "file": "a.wav", "x": 0.0, "y": 0.0 }, ... ] }
/// Without one, it's guessed from common file-naming patterns (Cap, CapEdge, Cone, Edge, Center;
/// distances like 1in, 2.5cm, 10mm). Packs whose IRs vary along one axis become a 1D slider.
///
/// Moving a mic to any point uses the nearest captures: bilinear weights on a full grid, linear
/// weights along a single axis, inverse-distance weights (nearest four) for scattered points. The
/// morph (minphase.h) interpolates their log-magnitude spectra and arrival delays, so a moving mic
/// never comb filters. Sitting exactly on a captured point returns that capture untouched.
class CabPack
{
public:
    struct Point
    {
        juce::File file;
        double x = 0.0, y = 0.0;
    };

    enum class Layout
    {
        single,  // one IR
        line,    // varies along one axis
        grid,    // every x at every y
        scattered
    };

    struct LoadResult
    {
        bool ok = false;
        juce::String message;
        bool placedFromNames = false; // no manifest: positions guessed from file names
    };

    /// Any non-audio thread. Reads every IR in the folder (left channel), resamples nothing (IRs
    /// must share one sample rate), and precomputes each one's log magnitude and onset.
    LoadResult load (const juce::File& folder);

    /// Positions from file names, or an empty result when a name doesn't say where its mic was.
    /// Exposed for tests.
    static std::vector<Point> placeFromNames (const juce::Array<juce::File>& files, bool& allParsed);

    /// The weights of the captures used for a mic at (x, y). They sum to 1.
    std::vector<std::pair<int, double>> weightsAt (double x, double y) const;

    /// The IR for a mic at (x, y): the original capture when (x, y) is a captured point, otherwise the
    /// minimum-phase morph of the nearest captures with their interpolated delay.
    std::vector<float> irAt (double x, double y) const;

    const std::vector<Point>& getPoints() const { return points; }
    /// "name (4 IRs on a grid)", for status lines.
    juce::String describe() const;
    Layout getLayout() const { return layout; }
    double getSampleRate() const { return sampleRate; }
    const juce::String& getName() const { return name; }

    /// For tests: a pack built from IRs in memory instead of a folder.
    LoadResult loadFromMemory (std::vector<Point> placedPoints, std::vector<std::vector<float>> irs, double rate, const juce::String& packName);

private:
    void analyse();
    void detectLayout();

    juce::String name;
    std::vector<Point> points;
    std::vector<std::vector<float>> irs;
    std::vector<std::vector<double>> logMagnitudes;
    std::vector<double> onsets;
    double sampleRate = 48000.0;
    int fftOrder = 12;
    int maxLength = 0;
    Layout layout = Layout::single;
    std::vector<double> gridX, gridY; // sorted unique coordinates, for grid and line layouts
};

} // namespace ampsim
