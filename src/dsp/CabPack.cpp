// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "CabPack.h"
#include "CabIR.h"
#include "Loudness.h"
#include "MinimumPhase.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <map>

namespace ampsim
{

namespace
{
constexpr double coordinateTolerance = 1.0e-6;

/// Where on the cone a file name says the mic was, from 0 (the dust cap at the centre) to 1 (the
/// edge), or -1 if it doesn't say. Tokens are compared whole, and two adjacent tokens are also tried
/// joined, so "Cap_Edge", "Cap-Edge", and "CapEdge" all match.
double positionFromName (const juce::StringArray& tokens)
{
    static const std::vector<std::pair<juce::String, double>> keywords {
        { "capedge", 0.25 }, { "capcone", 0.4 }, { "cap", 0.0 }, { "center", 0.0 }, { "centre", 0.0 },
        { "ctr", 0.0 }, { "cone", 0.6 }, { "edge", 1.0 },
    };

    for (int i = 0; i < tokens.size(); ++i)
    {
        const auto joined = i + 1 < tokens.size() ? tokens[i] + tokens[i + 1] : juce::String();

        for (const auto& [word, position] : keywords) // most specific first
            if (joined == word)
                return position;

        for (const auto& [word, position] : keywords)
            if (tokens[i] == word)
                return position;
    }

    return -1.0;
}

/// The mic's distance from the grille in millimetres, from text like "1in", "2.5 in", "10mm", "3cm",
/// or -1 if the name doesn't say.
double distanceFromName (const juce::String& lowerName)
{
    const auto text = lowerName.replaceCharacters ("_-", "  ");

    for (int i = 0; i < text.length(); ++i)
    {
        if (! juce::CharacterFunctions::isDigit (text[i]) || (i > 0 && (juce::CharacterFunctions::isDigit (text[i - 1]) || text[i - 1] == '.')))
            continue;

        int end = i;
        while (end < text.length() && (juce::CharacterFunctions::isDigit (text[end]) || text[end] == '.'))
            ++end;

        const auto value = text.substring (i, end).getDoubleValue();
        auto unitStart = end;
        while (unitStart < text.length() && text[unitStart] == ' ')
            ++unitStart;
        const auto rest = text.substring (unitStart);

        if (rest.startsWith ("inch") || rest.startsWith ("in") || rest.startsWith ("\""))
            return value * 25.4;
        if (rest.startsWith ("cm"))
            return value * 10.0;
        if (rest.startsWith ("mm"))
            return value;
    }

    return -1.0;
}

std::vector<double> uniqueSorted (std::vector<double> values)
{
    std::sort (values.begin(), values.end());
    std::vector<double> out;
    for (auto v : values)
        if (out.empty() || std::abs (v - out.back()) > coordinateTolerance)
            out.push_back (v);
    return out;
}

/// The index of the cell [grid[i], grid[i + 1]] containing v (clamped to the grid), and v's position
/// inside it from 0 to 1.
std::pair<size_t, double> locate (const std::vector<double>& grid, double v)
{
    if (grid.size() < 2)
        return { 0, 0.0 };

    v = juce::jlimit (grid.front(), grid.back(), v);
    size_t i = 0;
    while (i + 2 < grid.size() && v > grid[i + 1])
        ++i;

    return { i, (v - grid[i]) / (grid[i + 1] - grid[i]) };
}
} // namespace

std::vector<CabPack::Point> CabPack::placeFromNames (const juce::Array<juce::File>& files, bool& allParsed)
{
    std::vector<Point> placed;
    std::vector<double> distances;
    int withPosition = 0, withDistance = 0;

    for (const auto& file : files)
    {
        const auto lower = file.getFileNameWithoutExtension().toLowerCase();
        juce::StringArray tokens;
        tokens.addTokens (lower.replaceCharacters ("_-.()[]", "       "), " ", "");
        tokens.removeEmptyStrings();

        const auto position = positionFromName (tokens);
        const auto distance = distanceFromName (lower);
        withPosition += position >= 0.0 ? 1 : 0;
        withDistance += distance >= 0.0 ? 1 : 0;

        placed.push_back ({ file, std::max (0.0, position), 0.0 });
        distances.push_back (std::max (0.0, distance));
    }

    const auto n = (int) files.size();
    allParsed = n > 0 && (withPosition == 0 || withPosition == n) && (withDistance == 0 || withDistance == n)
                && (withPosition > 0 || withDistance > 0);

    // Distance becomes 0 (closest) to 1 (farthest).
    const auto [lo, hi] = std::minmax_element (distances.begin(), distances.end());
    for (size_t i = 0; i < placed.size(); ++i)
        placed[i].y = (distances.empty() || *hi - *lo < coordinateTolerance) ? 0.0 : (distances[i] - *lo) / (*hi - *lo);

    // Two files at the same spot (say, two different mics in one folder) can't be told apart.
    for (size_t i = 0; i < placed.size() && allParsed; ++i)
        for (size_t j = i + 1; j < placed.size(); ++j)
            if (std::abs (placed[i].x - placed[j].x) < coordinateTolerance && std::abs (placed[i].y - placed[j].y) < coordinateTolerance)
                allParsed = false;

    return placed;
}

CabPack::LoadResult CabPack::load (const juce::File& folder)
{
    if (! folder.isDirectory())
        return { false, folder.getFullPathName() + " isn't a folder" };

    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.wav;*.aif;*.aiff;*.flac");
    files.sort();

    if (files.isEmpty())
        return { false, folder.getFileName() + " has no IR files" };

    std::vector<Point> placed;
    bool placedFromNames = false;
    const auto manifest = folder.getChildFile ("cabpack.json");

    if (manifest.existsAsFile())
    {
        const auto json = juce::JSON::parse (manifest);
        const auto* entries = json.getProperty ("points", {}).getArray();

        if (entries == nullptr)
            return { false, "cabpack.json has no \"points\" list" };

        for (const auto& entry : *entries)
        {
            const auto file = folder.getChildFile (entry.getProperty ("file", {}).toString());
            if (! file.existsAsFile())
                return { false, "cabpack.json lists " + file.getFileName() + ", which isn't in the folder" };
            placed.push_back ({ file, juce::jlimit (0.0, 1.0, (double) entry.getProperty ("x", 0.0)),
                                juce::jlimit (0.0, 1.0, (double) entry.getProperty ("y", 0.0)) });
        }
    }
    else
    {
        bool allParsed = false;
        placed = placeFromNames (files, allParsed);

        if (! allParsed)
        {
            // Names don't say where the mics were: lay the files out along the cone in name order,
            // which at least gives a usable 1D slider. A cabpack.json places them properly.
            for (size_t i = 0; i < placed.size(); ++i)
                placed[i] = { files[(int) i], placed.size() > 1 ? (double) i / (double) (placed.size() - 1) : 0.0, 0.0 };
        }

        placedFromNames = allParsed;
    }

    // Read every IR's first channel. They must share one sample rate to be morphed together.
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::vector<std::vector<float>> loaded;
    double rate = 0.0;

    for (const auto& point : placed)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (point.file));
        if (reader == nullptr || reader->lengthInSamples <= 0)
            return { false, "Couldn't read " + point.file.getFileName() };

        if (rate > 0.0 && std::abs (reader->sampleRate - rate) > 0.5)
            return { false, "The IRs in a pack must share one sample rate (" + point.file.getFileName() + " differs)" };
        rate = reader->sampleRate;

        const auto length = (int) juce::jmin (reader->lengthInSamples, (juce::int64) std::ceil (rate * CabIR::maxIRSeconds));
        juce::AudioBuffer<float> buffer (1, length);
        reader->read (&buffer, 0, length, 0, true, false);
        loaded.emplace_back (buffer.getReadPointer (0), buffer.getReadPointer (0) + length);
    }

    auto result = loadFromMemory (std::move (placed), std::move (loaded), rate, folder.getFileName());
    if (result.ok)
    {
        result.placedFromNames = placedFromNames;
        result.message = describe() + (manifest.existsAsFile() ? ", placed by cabpack.json"
                                       : placedFromNames ? ", placed by file names"
                                                         : ", placed in file order: add cabpack.json to place them");
    }
    return result;
}

CabPack::LoadResult CabPack::loadFromMemory (std::vector<Point> placedPoints, std::vector<std::vector<float>> captured,
                                             double rate, const juce::String& packName)
{
    if (placedPoints.empty() || placedPoints.size() != captured.size())
        return { false, "A pack needs one IR per point" };

    name = packName;
    points = std::move (placedPoints);
    irs = std::move (captured);
    sampleRate = rate;

    // Loudness-match each capture the same way single IRs are, so a morph between two captures
    // interpolates between two equally loud sounds.
    for (auto& h : irs)
    {
        const auto gain = loudness::whiteNoiseMatchingGain ({ h.data() }, (int) h.size(), sampleRate);
        if (gain <= 0.0)
            return { false, "A pack IR is silent" };
        for (auto& v : h)
            v = (float) (v * gain);
    }

    analyse();
    detectLayout();
    return { true, describe() };
}

juce::String CabPack::describe() const
{
    static const char* layoutNames[] = { "1 position", "a line", "a grid", "scattered positions" };
    return name + " (" + juce::String ((int) points.size()) + " IRs on " + layoutNames[(int) layout] + ")";
}

void CabPack::analyse()
{
    maxLength = 0;
    for (const auto& h : irs)
        maxLength = std::max (maxLength, (int) h.size());

    fftOrder = minphase::fftOrderFor (maxLength);
    logMagnitudes.clear();
    onsets.clear();

    for (const auto& h : irs)
    {
        logMagnitudes.push_back (minphase::logMagnitude (h, fftOrder));

        // The bulk delay: when the capture's sound arrives, minus the little bit of onset that its own
        // minimum-phase version already has, so a rebuilt IR lands where the capture did.
        const auto ownOnset = minphase::onset (minphase::minimumPhase (h, fftOrder));
        onsets.push_back (std::max (0.0, minphase::onset (h) - ownOnset));
    }
}

void CabPack::detectLayout()
{
    std::vector<double> xs, ys;
    for (const auto& p : points)
    {
        xs.push_back (p.x);
        ys.push_back (p.y);
    }

    gridX = uniqueSorted (xs);
    gridY = uniqueSorted (ys);

    if (points.size() == 1)
        layout = Layout::single;
    else if (gridX.size() == 1 || gridY.size() == 1)
        layout = Layout::line;
    else if (gridX.size() * gridY.size() == points.size())
        layout = Layout::grid; // as many points as cells and no two the same: every x at every y
    else
        layout = Layout::scattered;
}

std::vector<std::pair<int, double>> CabPack::weightsAt (double x, double y) const
{
    const auto indexOf = [this] (double px, double py)
    {
        for (size_t i = 0; i < points.size(); ++i)
            if (std::abs (points[i].x - px) < coordinateTolerance && std::abs (points[i].y - py) < coordinateTolerance)
                return (int) i;
        return -1;
    };

    std::map<int, double> weights;
    const auto add = [&weights] (int index, double w)
    {
        if (index >= 0 && w > 1.0e-12)
            weights[index] += w;
    };

    switch (layout)
    {
        case Layout::single:
            add (0, 1.0);
            break;

        case Layout::line:
        {
            const bool alongX = gridX.size() > 1;
            const auto& axis = alongX ? gridX : gridY;
            const auto [i, t] = locate (axis, alongX ? x : y);
            const auto other = alongX ? gridY.front() : gridX.front();
            add (alongX ? indexOf (axis[i], other) : indexOf (other, axis[i]), 1.0 - t);
            add (alongX ? indexOf (axis[i + 1], other) : indexOf (other, axis[i + 1]), t);
            break;
        }

        case Layout::grid:
        {
            // Bilinear: the four corners of the cell around (x, y), each weighted by the area of the
            // opposite sub-rectangle.
            const auto [i, u] = locate (gridX, x);
            const auto [j, v] = locate (gridY, y);
            add (indexOf (gridX[i], gridY[j]), (1.0 - u) * (1.0 - v));
            add (indexOf (gridX[i + 1], gridY[j]), u * (1.0 - v));
            add (indexOf (gridX[i], gridY[j + 1]), (1.0 - u) * v);
            add (indexOf (gridX[i + 1], gridY[j + 1]), u * v);
            break;
        }

        case Layout::scattered:
        {
            // Inverse-distance weighting over the nearest four points, exact at a captured point.
            std::vector<std::pair<double, int>> byDistance;
            for (size_t k = 0; k < points.size(); ++k)
                byDistance.push_back ({ std::hypot (points[k].x - x, points[k].y - y), (int) k });
            std::sort (byDistance.begin(), byDistance.end());

            if (byDistance.front().first < coordinateTolerance)
            {
                add (byDistance.front().second, 1.0);
                break;
            }

            double total = 0.0;
            for (size_t k = 0; k < std::min<size_t> (4, byDistance.size()); ++k)
                total += 1.0 / (byDistance[k].first * byDistance[k].first);
            for (size_t k = 0; k < std::min<size_t> (4, byDistance.size()); ++k)
                add (byDistance[k].second, 1.0 / (byDistance[k].first * byDistance[k].first) / total);
            break;
        }
    }

    return { weights.begin(), weights.end() };
}

std::vector<float> CabPack::irAt (double x, double y) const
{
    const auto weights = weightsAt (x, y);

    if (weights.size() == 1)
        return irs[(size_t) weights.front().first]; // on a captured point: the capture itself

    // Interpolate the log magnitudes and the delays, then rebuild a minimum-phase IR.
    std::vector<double> logMag (logMagnitudes.front().size(), 0.0);
    double delay = 0.0;

    for (const auto& [index, w] : weights)
    {
        const auto& source = logMagnitudes[(size_t) index];
        for (size_t k = 0; k < logMag.size(); ++k)
            logMag[k] += w * source[k];
        delay += w * onsets[(size_t) index];
    }

    const auto length = std::min (maxLength + (int) std::ceil (delay) + 1, 1 << fftOrder);
    return minphase::fromLogMagnitude (logMag, fftOrder, delay, length);
}

} // namespace ampsim
