// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AppSettings.h"
#include "AppInfo.h"

namespace platform::settings
{
namespace
{
juce::File& overrideFile()
{
    static juce::File f;
    return f;
}

/// What this run has set, so a value holds for the run even when the file can't be written.
juce::NamedValueSet& thisRun()
{
    static juce::NamedValueSet values;
    return values;
}

juce::var read()
{
    const auto f = file();
    return f.existsAsFile() ? juce::JSON::parse (f) : juce::var();
}
} // namespace

juce::File file()
{
    return overrideFile() != juce::File() ? overrideFile() : userDataFolder().getChildFile ("settings.json");
}

void setFileForTests (const juce::File& f)
{
    overrideFile() = f;
    thisRun().clear();
}

bool getBool (const juce::String& key, bool defaultValue)
{
    if (const auto* v = thisRun().getVarPointer (key))
        return (bool) *v;
    const auto stored = read();
    return stored.hasProperty (key) ? (bool) stored[juce::Identifier (key)] : defaultValue;
}

bool setBool (const juce::String& key, bool value)
{
    thisRun().set (key, value);
    auto stored = read();
    if (! stored.isObject())
        stored = juce::var (new juce::DynamicObject());
    stored.getDynamicObject()->setProperty (key, value);
    const auto f = file();
    return f.getParentDirectory().createDirectory().wasOk() && f.replaceWithText (juce::JSON::toString (stored, false));
}
} // namespace platform::settings
