// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace testing
{

/// One line on a plot.
struct PlotSeries
{
    juce::String name;
    std::vector<double> x, y;
    juce::Colour colour;
    float thickness = 2.0f;
    bool dashed = false;
};

struct PlotOptions
{
    juce::String title, xLabel, yLabel;
    bool logX = false;
    double xMin = 0.0, xMax = 1.0, yMin = 0.0, yMax = 1.0;
    int width = 1000, height = 560;
};

/// Draws the series as lines over a labelled grid and saves a PNG at 2x scale. Proof images only:
/// frequency responses, transfer curves, envelopes.
bool savePlot (const juce::File& file, const PlotOptions& options, const std::vector<PlotSeries>& series);

/// Distinguishable line colours on the dark background, by index.
juce::Colour plotColour (int index);

} // namespace testing
