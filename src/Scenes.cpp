// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Scenes.h"
#include "Presets.h"

bool Scenes::isSwitch (const juce::String& id)
{
    if (presets::isGlobal (id))
        return false;
    return id == "amp_model" || id == "cab_bypass" || id.endsWith ("_on");
}

void Scenes::setChosen (const juce::String& id, bool shouldBeChosen)
{
    if (shouldBeChosen)
        chosen.addIfNotAlreadyThere (id);
    else
        chosen.removeString (id);
}

void Scenes::store (int index, juce::AudioProcessorValueTreeState& state)
{
    auto& scene = scenes[(size_t) juce::jlimit (0, count - 1, index)];
    scene.values.clear();
    for (auto* parameter : state.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);
            ranged != nullptr && (isSwitch (ranged->paramID) || chosen.contains (ranged->paramID)))
            scene.values[ranged->paramID] = ranged->convertFrom0to1 (ranged->getValue());
    if (scene.name.isEmpty())
        scene.name = "Scene " + juce::String (index + 1);
    scene.stored = true;
    current = index;
}

bool Scenes::recall (int index, juce::AudioProcessorValueTreeState& state)
{
    if (index < 0 || index >= count || ! scenes[(size_t) index].stored)
        return false;

    for (const auto& [id, value] : scenes[(size_t) index].values)
        if (auto* parameter = state.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    current = index;
    return true;
}

void Scenes::clear (int index)
{
    scenes[(size_t) juce::jlimit (0, count - 1, index)] = {};
    if (current == index)
        current = -1;
}

void Scenes::rename (int index, const juce::String& name)
{
    scenes[(size_t) juce::jlimit (0, count - 1, index)].name = name;
}

juce::var Scenes::toVar() const
{
    auto* root = new juce::DynamicObject();
    juce::Array<juce::var> ids;
    for (const auto& id : chosen)
        ids.add (id);
    root->setProperty ("parameters", ids);

    juce::Array<juce::var> list;
    for (const auto& scene : scenes)
    {
        if (! scene.stored)
        {
            list.add (juce::var());
            continue;
        }
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("name", scene.name);
        auto* values = new juce::DynamicObject();
        for (const auto& [id, value] : scene.values)
            values->setProperty (id, value);
        entry->setProperty ("values", juce::var (values));
        list.add (juce::var (entry));
    }
    root->setProperty ("list", list);
    return juce::var (root);
}

Scenes Scenes::fromVar (const juce::var& v, juce::AudioProcessorValueTreeState& state, juce::StringArray* warnings)
{
    Scenes s;
    if (const auto* ids = v.getProperty ("parameters", {}).getArray())
        for (const auto& id : *ids)
            if (state.getParameter (id.toString()) != nullptr)
                s.chosen.addIfNotAlreadyThere (id.toString());

    juce::StringArray unknown;
    if (const auto* list = v.getProperty ("list", {}).getArray())
        for (int i = 0; i < juce::jmin (count, list->size()); ++i)
        {
            const auto& entry = list->getReference (i);
            if (! entry.isObject())
                continue;
            auto& scene = s.scenes[(size_t) i];
            scene.stored = true;
            scene.name = entry.getProperty ("name", "Scene " + juce::String (i + 1)).toString();
            if (const auto* values = entry.getProperty ("values", {}).getDynamicObject())
                for (const auto& property : values->getProperties())
                {
                    const auto id = property.name.toString();
                    if (state.getParameter (id) != nullptr)
                        scene.values[id] = (float) (double) property.value;
                    else
                        unknown.addIfNotAlreadyThere (id);
                }
        }

    if (warnings != nullptr)
        for (const auto& id : unknown)
            warnings->add ("Scenes name unknown parameter " + id + ", skipped");
    return s;
}
