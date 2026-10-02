#include "MidiMap.h"

const char* MidiMapping::actionName (Action action)
{
    switch (action)
    {
        case Action::momentary:  return "momentary";
        case Action::continuous: return "continuous";
        case Action::toggle:     break;
    }
    return "toggle";
}

void MidiMap::set (const MidiMapping& mapping)
{
    remove (mapping.cc);
    mappings.push_back (mapping);
}

void MidiMap::remove (int cc)
{
    mappings.erase (std::remove_if (mappings.begin(), mappings.end(), [cc] (const MidiMapping& m) { return m.cc == cc; }), mappings.end());
}

bool MidiMap::handle (int cc, int value, juce::AudioProcessorValueTreeState& state)
{
    if (cc < 0 || cc > 127)
        return false;

    const bool pressed = value >= 64;
    if (learn (cc, state))
        return true;

    bool used = false;
    for (const auto& m : mappings)
    {
        if (m.cc != cc)
            continue;
        auto* parameter = state.getParameter (m.parameterId);
        if (parameter == nullptr)
            continue;

        switch (m.action)
        {
            case MidiMapping::Action::toggle:
                // Every press (64 or more) flips it and releases are ignored, so a switch that sends 127
                // on press and 0 on release, and one that sends 127 on every press, both flip once a press.
                if (pressed)
                    parameter->setValueNotifyingHost (parameter->getValue() >= 0.5f ? 0.0f : 1.0f);
                break;

            case MidiMapping::Action::momentary:
                parameter->setValueNotifyingHost (pressed ? 1.0f : 0.0f);
                break;

            case MidiMapping::Action::continuous:
            {
                const auto plain = m.minimum + (m.maximum - m.minimum) * (float) juce::jlimit (0, 127, value) / 127.0f;
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
                break;
            }
        }
        used = true;
    }
    return used;
}

bool MidiMap::learn (int cc, juce::AudioProcessorValueTreeState& state)
{
    if (learnTarget.isEmpty())
        return false;

    auto* parameter = state.getParameter (learnTarget);
    learnTarget.clear();
    if (parameter == nullptr)
        return false;

    MidiMapping mapping;
    mapping.cc = cc;
    mapping.parameterId = parameter->paramID;
    if (dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr)
    {
        mapping.action = MidiMapping::Action::toggle;
    }
    else
    {
        mapping.action = MidiMapping::Action::continuous;
        mapping.minimum = parameter->getNormalisableRange().start;
        mapping.maximum = parameter->getNormalisableRange().end;
    }
    set (mapping);
    return true;
}

juce::var MidiMap::toVar() const
{
    juce::Array<juce::var> list;
    for (const auto& m : mappings)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("cc", m.cc);
        entry->setProperty ("action", MidiMapping::actionName (m.action));
        entry->setProperty ("parameter", m.parameterId);
        if (m.action == MidiMapping::Action::continuous)
        {
            entry->setProperty ("min", m.minimum);
            entry->setProperty ("max", m.maximum);
        }
        list.add (juce::var (entry));
    }
    return list;
}

MidiMap MidiMap::fromVar (const juce::var& v, juce::AudioProcessorValueTreeState& state, juce::StringArray* warnings)
{
    MidiMap map;
    if (const auto* list = v.getArray())
    {
        for (const auto& entry : *list)
        {
            MidiMapping m;
            m.cc = juce::jlimit (0, 127, (int) entry.getProperty ("cc", 0));
            m.parameterId = entry.getProperty ("parameter", {}).toString();
            const auto action = entry.getProperty ("action", "toggle").toString();
            m.action = action == "continuous" ? MidiMapping::Action::continuous
                       : action == "momentary" ? MidiMapping::Action::momentary
                                               : MidiMapping::Action::toggle;
            m.minimum = (float) (double) entry.getProperty ("min", 0.0);
            m.maximum = (float) (double) entry.getProperty ("max", 1.0);

            if (state.getParameter (m.parameterId) == nullptr)
            {
                if (warnings != nullptr)
                    warnings->add ("MIDI mapping for CC " + juce::String (m.cc) + " names unknown parameter " + m.parameterId + ", skipped");
                continue;
            }
            map.set (m);
        }
    }
    return map;
}

bool CcFifo::push (int cc, int value) noexcept
{
    const auto scope = fifo.write (1);
    if (scope.blockSize1 + scope.blockSize2 == 0)
    {
        dropped.fetch_add (1, std::memory_order_relaxed);
        return false; // full: drop it rather than wait
    }
    auto& e = events[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)];
    e.cc = (juce::uint8) juce::jlimit (0, 127, cc);
    e.value = (juce::uint8) juce::jlimit (0, 127, value);
    return true;
}
