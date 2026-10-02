#include "Plot.h"

#include <cmath>

namespace testing
{

namespace
{
juce::String tickLabel (double v)
{
    const auto a = std::abs (v);
    if (a >= 1000.0)
        return juce::String (v / 1000.0, a >= 10000.0 || std::fmod (a, 1000.0) == 0.0 ? 0 : 1) + "k";
    if (a == 0.0 || a >= 10.0)
        return juce::String (juce::roundToInt (v));
    if (a >= 1.0)
        return juce::String (v, std::fmod (a, 1.0) == 0.0 ? 0 : 1);
    return juce::String (v, 2);
}

/// A round step (1, 2, or 5 times a power of ten) giving about `count` intervals over `span`.
double niceStep (double span, int count)
{
    const auto raw = span / count;
    const auto power = std::pow (10.0, std::floor (std::log10 (raw)));
    for (auto m : { 1.0, 2.0, 5.0, 10.0 })
        if (m * power >= raw)
            return m * power;
    return 10.0 * power;
}
} // namespace

juce::Colour plotColour (int index)
{
    static const juce::Colour colours[] = { juce::Colour (0xff3fa7d6), juce::Colour (0xffff6b5e), juce::Colour (0xff7ad151),
                                            juce::Colour (0xffffc04d), juce::Colour (0xffc792ea), juce::Colour (0xff4dd0c4),
                                            juce::Colour (0xffe6e6e6), juce::Colour (0xfff78c6c) };
    return colours[(size_t) index % (sizeof (colours) / sizeof (colours[0]))];
}

bool savePlot (const juce::File& file, const PlotOptions& o, const std::vector<PlotSeries>& series)
{
    constexpr float scale = 2.0f;
    juce::Image image (juce::Image::ARGB, juce::roundToInt ((float) o.width * scale), juce::roundToInt ((float) o.height * scale), true);
    {
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::scale (scale));
        g.fillAll (juce::Colour (0xff17191d));

        const auto plot = juce::Rectangle<float> (72.0f, 44.0f, (float) o.width - 96.0f, (float) o.height - 100.0f);
        const auto mapX = [&] (double x)
        {
            const auto t = o.logX ? (std::log (x) - std::log (o.xMin)) / (std::log (o.xMax) - std::log (o.xMin))
                                  : (x - o.xMin) / (o.xMax - o.xMin);
            return plot.getX() + (float) t * plot.getWidth();
        };
        const auto mapY = [&] (double y) { return plot.getBottom() - (float) ((y - o.yMin) / (o.yMax - o.yMin)) * plot.getHeight(); };

        // Grid and tick labels.
        g.setFont (juce::FontOptions (11.0f));
        std::vector<double> xTicks;
        if (o.logX)
        {
            for (auto decade = std::floor (std::log10 (o.xMin)); decade <= std::ceil (std::log10 (o.xMax)); decade += 1.0)
                for (auto m : { 1.0, 2.0, 5.0 })
                    if (const auto v = m * std::pow (10.0, decade); v >= o.xMin * 0.999 && v <= o.xMax * 1.001)
                        xTicks.push_back (v);
        }
        else
        {
            const auto step = niceStep (o.xMax - o.xMin, 10);
            for (auto v = std::ceil (o.xMin / step) * step; v <= o.xMax + 1.0e-9 * step; v += step)
                xTicks.push_back (v);
        }

        const auto yStep = niceStep (o.yMax - o.yMin, 8);
        std::vector<double> yTicks;
        for (auto v = std::ceil (o.yMin / yStep) * yStep; v <= o.yMax + 1.0e-9 * yStep; v += yStep)
            yTicks.push_back (v);

        for (auto v : xTicks)
        {
            const auto x = mapX (v);
            g.setColour (juce::Colour (0x22ffffff));
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
            g.setColour (juce::Colour (0xff9aa0a8));
            g.drawText (tickLabel (v), juce::Rectangle<float> (x - 30.0f, plot.getBottom() + 4.0f, 60.0f, 14.0f), juce::Justification::centred);
        }

        for (auto v : yTicks)
        {
            const auto y = mapY (v);
            g.setColour (juce::Colour (std::abs (v) < 1.0e-9 * yStep ? 0x44ffffff : 0x22ffffff));
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (juce::Colour (0xff9aa0a8));
            g.drawText (tickLabel (v), juce::Rectangle<float> (plot.getX() - 64.0f, y - 7.0f, 58.0f, 14.0f), juce::Justification::centredRight);
        }

        g.setColour (juce::Colour (0x55ffffff));
        g.drawRect (plot, 1.0f);

        // Titles.
        g.setColour (juce::Colour (0xffe6e6e6));
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText (o.title, juce::Rectangle<float> (plot.getX(), 10.0f, plot.getWidth(), 22.0f), juce::Justification::centredLeft);
        g.setFont (juce::FontOptions (12.0f));
        g.setColour (juce::Colour (0xff9aa0a8));
        g.drawText (o.xLabel, juce::Rectangle<float> (plot.getX(), plot.getBottom() + 22.0f, plot.getWidth(), 16.0f), juce::Justification::centred);
        g.saveState();
        g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, 16.0f, plot.getCentreY()));
        g.drawText (o.yLabel, juce::Rectangle<float> (16.0f - plot.getHeight() / 2.0f, plot.getCentreY() - 8.0f, plot.getHeight(), 16.0f),
                    juce::Justification::centred);
        g.restoreState();

        // The series, clipped to the plot.
        g.saveState();
        g.reduceClipRegion (plot.toNearestInt());
        for (const auto& s : series)
        {
            juce::Path path;
            bool started = false;
            for (size_t i = 0; i < std::min (s.x.size(), s.y.size()); ++i)
            {
                if (! std::isfinite (s.y[i]) || (o.logX && s.x[i] <= 0.0))
                {
                    started = false;
                    continue;
                }
                const auto y = juce::jlimit (o.yMin - (o.yMax - o.yMin), o.yMax + (o.yMax - o.yMin), s.y[i]);
                const juce::Point<float> p { mapX (s.x[i]), mapY (y) };
                if (started)
                    path.lineTo (p);
                else
                    path.startNewSubPath (p);
                started = true;
            }

            g.setColour (s.colour);
            if (s.dashed)
            {
                juce::Path dashed;
                const float lengths[] = { 6.0f, 4.0f };
                juce::PathStrokeType (s.thickness).createDashedStroke (dashed, path, lengths, 2);
                g.fillPath (dashed);
            }
            else
            {
                g.strokePath (path, juce::PathStrokeType (s.thickness, juce::PathStrokeType::curved));
            }
        }
        g.restoreState();

        // Legend, top right inside the plot.
        g.setFont (juce::FontOptions (12.0f));
        auto legendY = plot.getY() + 8.0f;
        for (const auto& s : series)
        {
            if (s.name.isEmpty())
                continue;
            const auto width = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), s.name);
            const auto x = plot.getRight() - width - 40.0f;
            g.setColour (juce::Colour (0xcc17191d));
            g.fillRect (juce::Rectangle<float> (x - 6.0f, legendY - 2.0f, width + 42.0f, 18.0f));
            g.setColour (s.colour);
            g.fillRect (juce::Rectangle<float> (x, legendY + 6.0f, 22.0f, 3.0f));
            g.setColour (juce::Colour (0xffe6e6e6));
            g.drawText (s.name, juce::Rectangle<float> (x + 28.0f, legendY - 1.0f, width + 8.0f, 16.0f), juce::Justification::centredLeft);
            legendY += 18.0f;
        }
    }

    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

} // namespace testing
