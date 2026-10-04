// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "GainSet.h"

namespace ampsim
{

juce::File GainSet::jsonFor (const juce::File& fileOrFolder)
{
    return fileOrFolder.isDirectory() ? fileOrFolder.getChildFile (conventionalFileName) : fileOrFolder;
}

bool GainSet::isGainSet (const juce::File& fileOrFolder)
{
    const auto json = jsonFor (fileOrFolder);
    if (! json.existsAsFile() || ! json.hasFileExtension ("json") || json.getSize() > 1024 * 1024)
        return false;
    return juce::JSON::parse (json.loadFileAsString()).getProperty ("format", {}).toString() == formatName;
}

bool GainSet::read (const juce::File& fileOrFolder, GainSet& out, juce::String& error)
{
    const auto json = jsonFor (fileOrFolder);
    const auto shown = json.getParentDirectory().getFileName() + "/" + json.getFileName();
    out = {};

    if (! json.existsAsFile())
    {
        error = shown + " doesn't exist";
        return false;
    }

    juce::var root;
    if (const auto parsed = juce::JSON::parse (json.loadFileAsString(), root); parsed.failed() || ! root.isObject())
    {
        error = shown + " isn't valid JSON";
        return false;
    }

    if (root.getProperty ("format", {}).toString() != formatName)
    {
        error = shown + " isn't a gain set (its \"format\" isn't \"" + juce::String (formatName) + "\")";
        return false;
    }

    if ((int) root.getProperty ("version", 0) != 1)
    {
        error = shown + " is gain set version " + root.getProperty ("version", 0).toString() + "; this build reads version 1";
        return false;
    }

    out.name = root.getProperty ("name", json.getParentDirectory().getFileName()).toString();
    out.description = root.getProperty ("description", {}).toString();
    out.toneType = root.getProperty ("tone_type", {}).toString();

    const auto* steps = root.getProperty ("steps", {}).getArray();
    if (steps == nullptr || steps->isEmpty() || steps->size() > maxSteps)
    {
        error = shown + " needs 1 to " + juce::String (maxSteps) + " steps";
        return false;
    }

    for (const auto& s : *steps)
    {
        const auto gainVar = s.getProperty ("gain", {});
        const auto fileName = s.getProperty ("file", {}).toString();
        if (! (gainVar.isDouble() || gainVar.isInt() || gainVar.isInt64()) || fileName.isEmpty())
        {
            error = shown + ": every step needs a \"gain\" (0 to 10) and a \"file\"";
            return false;
        }

        const auto gain = (double) gainVar;
        if (gain < 0.0 || gain > 10.0 || (! out.steps.empty() && gain <= out.steps.back().gain))
        {
            error = shown + ": the steps' gains must be in 0 to 10 and ascending";
            return false;
        }

        const auto file = json.getParentDirectory().getChildFile (fileName);
        if (! file.existsAsFile())
        {
            error = shown + ": its step " + fileName + " is missing";
            return false;
        }

        out.steps.push_back ({ gain, file });
    }

    return true;
}

} // namespace ampsim
