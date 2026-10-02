#include "Presets.h"
#include "PluginProcessor.h"

namespace presets
{

namespace
{
const char* micKeys[] = { "mic1", "mic2", "room" };

juce::var toVar (const juce::StringArray& strings)
{
    juce::Array<juce::var> array;
    for (const auto& s : strings)
        array.add (s);
    return array;
}

juce::StringArray toStrings (const juce::var& v)
{
    juce::StringArray strings;
    if (const auto* array = v.getArray())
        for (const auto& item : *array)
            strings.add (item.toString());
    return strings;
}
} // namespace

bool isGlobal (const juce::String& parameterId)
{
    return parameterId == "input_calibrate" || parameterId == "input_level_dbu";
}

juce::var capture (AmpSimProcessor& processor, const juce::String& name)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("format_version", formatVersion);
    root->setProperty ("name", name);

    auto* values = new juce::DynamicObject();
    for (auto* parameter : processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter); ranged != nullptr && ! isGlobal (ranged->paramID))
            values->setProperty (ranged->paramID, ranged->convertFrom0to1 (ranged->getValue()));
    root->setProperty ("parameters", juce::var (values));

    const auto& state = processor.parameters.state;
    juce::StringArray amps;
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
        amps.add (state.getProperty (AmpSimProcessor::modelPathKey (s)).toString());
    root->setProperty ("amps", toVar (amps));

    auto* cab = new juce::DynamicObject();
    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
        cab->setProperty (micKeys[m], state.getProperty (AmpSimProcessor::cabPathKey (m)).toString());
    root->setProperty ("cab", juce::var (cab));

    auto* order = new juce::DynamicObject();
    order->setProperty ("pre", toVar (processor.getSectionOrder (ampsim::Chain::Section::pre)));
    order->setProperty ("post", toVar (processor.getSectionOrder (ampsim::Chain::Section::post)));
    root->setProperty ("order", juce::var (order));

    return juce::var (root);
}

ApplyResult validate (const juce::var& preset)
{
    ApplyResult result;
    if (! preset.isObject())
    {
        result.error = "Not a preset (no JSON object)";
        return result;
    }

    const auto version = (int) preset.getProperty ("format_version", 0);
    if (version < 1)
        result.error = "Not a preset (no format_version)";
    else if (version > formatVersion)
        result.error = "This preset was saved by a newer version of Amp Sim (format " + juce::String (version) + "; this one reads up to "
                       + juce::String (formatVersion) + ")";
    else if (! preset.getProperty ("parameters", {}).isObject())
        result.error = "The preset has no parameters";

    result.ok = result.error.isEmpty();
    return result;
}

ApplyResult apply (AmpSimProcessor& processor, const juce::var& preset)
{
    auto result = validate (preset);
    if (! result.ok)
        return result;

    // Migrations from older format versions go here, one small, tested function per step.

    // Every parameter: the preset's value, or the default if the preset doesn't mention it.
    const auto values = preset.getProperty ("parameters", {});
    for (auto* parameter : processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);
        if (ranged == nullptr || isGlobal (ranged->paramID))
            continue;

        const auto value = values.getProperty (ranged->paramID, {});
        const auto normalized = value.isVoid() ? ranged->getDefaultValue() : ranged->convertTo0to1 ((float) (double) value);
        ranged->setValueNotifyingHost (normalized);
    }

    if (const auto* object = values.getDynamicObject())
        for (const auto& property : object->getProperties())
            if (processor.parameters.getParameter (property.name.toString()) == nullptr)
                result.warnings.add ("Unknown parameter " + property.name.toString() + " skipped");

    // Captures and IRs: load what's named, empty what's blank, and say what's missing.
    const auto amps = toStrings (preset.getProperty ("amps", {}));
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
    {
        const auto path = amps[s];
        if (path.isEmpty())
            processor.clearModel (s);
        else if (juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile())
            processor.loadModel (s, juce::File (path));
        else
        {
            processor.clearModel (s);
            result.warnings.add ("Amp " + juce::String (s + 1) + "'s capture is missing: " + path);
        }
    }

    const auto cab = preset.getProperty ("cab", {});
    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
    {
        const auto path = cab.getProperty (micKeys[m], {}).toString();
        if (path.isEmpty())
            processor.clearCabIR (m);
        else if (juce::File::isAbsolutePath (path) && (juce::File (path).existsAsFile() || juce::File (path).isDirectory()))
            processor.loadCabIR (m, juce::File (path));
        else
        {
            processor.clearCabIR (m);
            result.warnings.add (juce::String (micKeys[m]) + "'s IR is missing: " + path);
        }
    }

    const auto order = preset.getProperty ("order", {});
    processor.setSectionOrder (ampsim::Chain::Section::pre, toStrings (order.getProperty ("pre", {})));
    processor.setSectionOrder (ampsim::Chain::Section::post, toStrings (order.getProperty ("post", {})));

    processor.parameters.state.setProperty ("presetName", preset.getProperty ("name", {}).toString(), nullptr);
    return result;
}

juce::File defaultFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support/AmpSim/presets");
}

bool save (const juce::var& preset, const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (juce::JSON::toString (preset));
}

juce::var load (const juce::File& file, juce::String& error)
{
    if (! file.existsAsFile())
    {
        error = "No such preset: " + file.getFullPathName();
        return {};
    }

    juce::var parsed;
    const auto parseResult = juce::JSON::parse (file.loadFileAsString(), parsed);
    if (parseResult.failed())
    {
        error = file.getFileName() + " isn't valid JSON: " + parseResult.getErrorMessage();
        return {};
    }
    return parsed;
}

} // namespace presets
