// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Presets.h"
#include "BinaryData.h"
#include "PluginProcessor.h"
#include "platform/AppInfo.h"

#include <map>

namespace presets
{

namespace
{
const char* micKeys[] = { "mic1", "mic2", "room" };

const juce::String factoryPrefix { "factory:" };

std::map<juce::String, juce::File>& libraryRoots()
{
    static std::map<juce::String, juce::File> roots;
    return roots;
}

/// FNV-1a, 64 bits (Fowler, Noll, Vo): h = (h xor byte) * 1099511628211, from 14695981039346656037.
/// Not cryptographic, and doesn't need to be: it tells files apart, it doesn't defend against forgeries.
struct Fnv1a64
{
    juce::uint64 h = 14695981039346656037ull;
    void add (const void* data, size_t n) noexcept
    {
        const auto* bytes = static_cast<const juce::uint8*> (data);
        for (size_t i = 0; i < n; ++i)
            h = (h ^ bytes[i]) * 1099511628211ull;
    }
    void add (const juce::String& s) { add (s.toRawUTF8(), s.getNumBytesAsUTF8()); }
    juce::String text() const { return "fnv1a64:" + juce::String::toHexString ((juce::int64) h).paddedLeft ('0', 16); }
};

juce::Array<juce::File> filesUnder (const juce::File& folder)
{
    juce::Array<juce::File> files;
    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        files.add (entry.getFile());
    std::sort (files.begin(), files.end(), [&folder] (const juce::File& a, const juce::File& b)
               { return a.getRelativePathFrom (folder) < b.getRelativePathFrom (folder); });
    return files;
}

juce::File resolvePath (const juce::String& path)
{
    // "factory:models/x.nam" is a file bundled with the app (content/ in the repo), wherever it's installed.
    if (path.startsWith (factoryPrefix))
        return libraryRoot ("factory").getChildFile (path.substring (factoryPrefix.length()));
    for (const auto* kind : { "models", "irs" })
        if (path.startsWith (juce::String (kind) + "/"))
            return libraryRoot (kind).getChildFile (path.fromFirstOccurrenceOf ("/", false, false));
    return juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
}

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

juce::File libraryRoot (const juce::String& kind)
{
    if (const auto it = libraryRoots().find (kind); it != libraryRoots().end())
        return it->second;
    if (kind == "factory")
        return platform::factoryContentFolder();
    return platform::userDataFolder().getChildFile (kind); // ~/Library/Application Support/BellyDSP/<kind>
}

void setLibraryRoot (const juce::String& kind, const juce::File& folder)
{
    if (folder == juce::File())
        libraryRoots().erase (kind); // back to the default
    else
        libraryRoots()[kind] = folder;
}

juce::var FileRef::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("path", path);
    o->setProperty ("hash", hash);
    o->setProperty ("size", size);
    return juce::var (o);
}

FileRef FileRef::fromVar (const juce::var& v)
{
    if (v.isString())
        return { v.toString(), {}, 0 }; // a version 1 entry that slipped through
    return { v.getProperty ("path", {}).toString(), v.getProperty ("hash", {}).toString(), (juce::int64) v.getProperty ("size", 0) };
}

juce::String contentHash (const juce::File& f)
{
    Fnv1a64 hash;
    if (f.isDirectory())
    {
        for (const auto& file : filesUnder (f))
        {
            hash.add (file.getRelativePathFrom (f));
            hash.add (contentHash (file));
        }
        return hash.text();
    }

    juce::FileInputStream in (f);
    if (! in.openedOk())
        return {};
    std::array<char, 65536> buffer;
    while (! in.isExhausted())
        if (const auto n = in.read (buffer.data(), (int) buffer.size()); n > 0)
            hash.add (buffer.data(), (size_t) n);
        else
            break;
    return hash.text();
}

juce::int64 contentSize (const juce::File& f)
{
    if (! f.isDirectory())
        return f.getSize();
    juce::int64 total = 0;
    for (const auto& file : filesUnder (f))
        total += file.getSize();
    return total;
}

FileRef makeRef (const juce::File& f, const juce::String& kind)
{
    const auto root = libraryRoot (kind), factory = libraryRoot ("factory");
    const auto path = f.isAChildOf (factory) ? factoryPrefix + f.getRelativePathFrom (factory).replaceCharacter ('\\', '/')
                    : f.isAChildOf (root)    ? kind + "/" + f.getRelativePathFrom (root).replaceCharacter ('\\', '/')
                                             : f.getFullPathName();
    return { path, contentHash (f), contentSize (f) };
}

Resolved resolve (const FileRef& ref, const juce::String& kind)
{
    Resolved r;
    if (ref.path.isEmpty())
        return r;

    if (const auto direct = resolvePath (ref.path); direct != juce::File() && direct.exists())
    {
        r.file = direct;
        r.found = true;
        r.changed = ref.hash.isNotEmpty() && (contentSize (direct) != ref.size || contentHash (direct) != ref.hash);
        return r;
    }

    // A built-in capture from before the gain sets: the set that replaced it, as if the preset named it.
    if (const auto replaced = replacementForRetiredCapture (ref.path); replaced.existsAsFile())
    {
        r.file = replaced;
        r.found = true;
        return r;
    }

    // Gone from its path: look through the library for the same size first (cheap), then the same hash.
    if (ref.hash.isEmpty())
        return r;
    const auto wantFolder = ! ref.path.endsWithIgnoreCase (".nam") && ! ref.path.endsWithIgnoreCase (".json") && ! ref.path.endsWithIgnoreCase (".wav")
                            && ! ref.path.endsWithIgnoreCase (".aif") && ! ref.path.endsWithIgnoreCase (".aiff");
    const auto extension = ref.path.fromLastOccurrenceOf (".", true, false);
    // The user's library first, then the app's bundled content (a factory file the user copied into the
    // library, or a library file that later shipped with the app, is the same file).
    for (const auto& root : { libraryRoot (kind), libraryRoot ("factory") })
    {
        if (! root.isDirectory())
            continue;
        for (const auto& entry : juce::RangedDirectoryIterator (root, true, wantFolder ? "*" : "*" + extension,
                                                               wantFolder ? juce::File::findDirectories : juce::File::findFiles))
        {
            const auto candidate = entry.getFile();
            if (contentSize (candidate) == ref.size && contentHash (candidate) == ref.hash)
            {
                r.file = candidate;
                r.found = r.relinked = true;
                return r;
            }
        }
    }
    return r;
}

juce::var migrateV1toV2 (const juce::var& v1)
{
    // Version 1 stored plain absolute paths (Phase 5). Version 2 stores { path, hash, size }; a migrated
    // entry has no hash, so it loads from its path but can't be relinked if the file moves.
    auto v2 = v1.clone();
    auto* object = v2.getDynamicObject();
    object->setProperty ("format_version", 2);

    juce::Array<juce::var> amps;
    if (const auto* list = v1.getProperty ("amps", {}).getArray())
        for (const auto& path : *list)
            amps.add (FileRef { path.toString(), {}, 0 }.toVar());
    object->setProperty ("amps", amps);

    auto* cab = new juce::DynamicObject();
    for (const auto* mic : micKeys)
        cab->setProperty (mic, FileRef { v1.getProperty ("cab", {}).getProperty (mic, {}).toString(), {}, 0 }.toVar());
    object->setProperty ("cab", juce::var (cab));
    return v2;
}

juce::var migrate (const juce::var& preset)
{
    auto current = preset;
    if ((int) current.getProperty ("format_version", 0) == 1)
        current = migrateV1toV2 (current);
    return current;
}

bool isGlobal (const juce::String& parameterId)
{
    return parameterId == "input_calibrate" || parameterId == "input_level_dbu" || parameterId == "drive_oversampling"
           || parameterId.startsWith ("tuner_") || parameterId.startsWith ("output_limit_");
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
    const auto refFor = [] (const juce::String& path, const char* kind)
    { return juce::File::isAbsolutePath (path) ? makeRef (juce::File (path), kind).toVar() : FileRef {}.toVar(); };

    juce::Array<juce::var> amps;
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
        amps.add (refFor (state.getProperty (AmpSimProcessor::modelPathKey (s)).toString(), "models"));
    root->setProperty ("amps", amps);

    auto* cab = new juce::DynamicObject();
    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
        cab->setProperty (micKeys[m], refFor (state.getProperty (AmpSimProcessor::cabPathKey (m)).toString(), "irs"));
    root->setProperty ("cab", juce::var (cab));

    // Follow amp choice: each slot's cab, and whether switching slots loads it.
    juce::Array<juce::var> assigned;
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
        assigned.add (refFor (state.getProperty (AmpSimProcessor::cabAssignKey (s)).toString(), "irs"));
    root->setProperty ("cab_assign", assigned);
    root->setProperty ("cab_follow", processor.isCabFollowing());

    root->setProperty ("midi", processor.getMidiMap().toVar());
    root->setProperty ("scenes", processor.getScenes().toVar());

    auto* order = new juce::DynamicObject();
    order->setProperty ("pre", toVar (processor.getSectionOrder (ampsim::Chain::Section::pre)));
    order->setProperty ("post", toVar (processor.getSectionOrder (ampsim::Chain::Section::post)));
    order->setProperty ("bloom", toVar (processor.getBloomOrder()));
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
        result.error = "This preset was saved by a newer version of BellyDSP (format " + juce::String (version) + "; this one reads up to "
                       + juce::String (formatVersion) + ")";
    else if (! preset.getProperty ("parameters", {}).isObject())
        result.error = "The preset has no parameters";

    result.ok = result.error.isEmpty();
    return result;
}

ApplyResult apply (AmpSimProcessor& processor, const juce::var& saved)
{
    auto result = validate (saved);
    if (! result.ok)
        return result;

    const auto preset = migrate (saved);

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

    // Captures and IRs: load what's named (relinking what has moved), empty what's blank, and say what's
    // missing or changed; a missing file never stops the rest of the preset.
    const auto describe = [&result] (const juce::String& what, const FileRef& ref, const Resolved& r)
    {
        if (! r.found)
            result.warnings.add (what + " is missing: " + ref.path);
        else if (r.relinked)
            result.warnings.add (what + " was relinked from " + ref.path + " to " + r.file.getFullPathName());
        else if (r.changed)
            result.warnings.add (what + " has changed since the preset was saved: " + r.file.getFullPathName());
    };

    const auto* amps = preset.getProperty ("amps", {}).getArray();
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
    {
        const auto ref = amps != nullptr && s < amps->size() ? FileRef::fromVar (amps->getReference (s)) : FileRef {};
        if (ref.path.isEmpty())
        {
            processor.clearModel (s);
            continue;
        }
        const auto r = resolve (ref, "models");
        describe ("Amp " + juce::String (s + 1) + "'s capture", ref, r);
        if (r.found && r.file.existsAsFile())
            processor.loadModel (s, r.file);
        else
            processor.clearModel (s);
    }

    const auto cab = preset.getProperty ("cab", {});
    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
    {
        const auto ref = FileRef::fromVar (cab.getProperty (micKeys[m], {}));
        if (ref.path.isEmpty())
        {
            processor.clearCabIR (m);
            continue;
        }
        const auto r = resolve (ref, "irs");
        describe (juce::String (micKeys[m]) + "'s IR", ref, r);
        if (r.found)
            processor.loadCabIR (m, r.file);
        else
            processor.clearCabIR (m);
    }

    // Follow amp choice. A preset made before it existed assigns nothing and follows (the default); the
    // preset's own close mic 1 is what plays now, so the slot it selects counts as followed already.
    const auto* assigned = preset.getProperty ("cab_assign", {}).getArray();
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
    {
        const auto ref = assigned != nullptr && s < assigned->size() ? FileRef::fromVar (assigned->getReference (s)) : FileRef {};
        auto file = juce::File();
        if (ref.path.isNotEmpty())
        {
            const auto r = resolve (ref, "irs");
            describe ("Amp " + juce::String (s + 1) + "'s cab", ref, r);
            if (r.found)
                file = r.file;
        }
        processor.setCabAssignment (s, file);
    }
    processor.parameters.state.setProperty (AmpSimProcessor::cabFollowKey, (bool) preset.getProperty ("cab_follow", true), nullptr);
    processor.markCabFollowed();

    // MIDI mappings: a preset without any (made before mappings existed) leaves the current ones alone.
    if (preset.hasProperty ("midi"))
        processor.getMidiMap() = MidiMap::fromVar (preset.getProperty ("midi", {}), processor.parameters, &result.warnings);

    // Scenes belong to the preset: one without any has none.
    processor.getScenes() = Scenes::fromVar (preset.getProperty ("scenes", {}), processor.parameters, &result.warnings);

    const auto order = preset.getProperty ("order", {});
    processor.setSectionOrder (ampsim::Chain::Section::pre, toStrings (order.getProperty ("pre", {})));
    processor.setSectionOrder (ampsim::Chain::Section::post, toStrings (order.getProperty ("post", {})));
    processor.setBloomOrder (toStrings (order.getProperty ("bloom", {})));

    processor.parameters.state.setProperty ("presetName", preset.getProperty ("name", {}).toString(), nullptr);
    processor.parameters.copyState(); // write the new values into the tree now, so they aren't recorded as an edit
    processor.undoManager.clearUndoHistory();
    processor.resetAB();
    return result;
}

juce::Array<juce::var> factoryPresets()
{
    juce::Array<juce::var> list;
    for (const auto* file : { "ModernProg_json", "MathRock_json", "TechDeath_json", "Metal_json", "MidwestEmo_json" })
    {
        int size = 0;
        if (const auto* data = BinaryData::getNamedResource (file, size))
            list.add (juce::JSON::parse (juce::String::fromUTF8 (data, size)));
    }
    return list;
}

juce::String currentFactoryPresetName (const juce::String& name)
{
    if (name == "Polyphia")
        return "Modern Prog";
    if (name == "CHON")
        return "Math Rock";
    return name;
}

juce::String builtInCaptureName (int slot)
{
    static const char* names[] = { "Glass", "Ember", "Monolith" };
    return names[juce::jlimit (0, 2, slot)];
}

juce::String builtInCapturePath (int slot)
{
    return factoryPrefix + "models/" + builtInCaptureName (slot) + "/gainset.json";
}

juce::File replacementForRetiredCapture (const juce::String& path)
{
    // "factory:models/Glass.nam", or a saved absolute path ending in .../content/models/Glass.nam.
    const auto normalised = path.replaceCharacter ('\\', '/');
    for (int slot = 0; slot < 3; ++slot)
    {
        const auto old = "models/" + builtInCaptureName (slot) + ".nam";
        if (normalised == factoryPrefix + old || (juce::File::isAbsolutePath (path) && normalised.endsWith ("content/" + old)))
            return builtInCapture (slot);
    }
    return {};
}

juce::File builtInCapture (int slot)
{
    return resolvePath (builtInCapturePath (slot));
}

bool isBundled (const juce::File& file)
{
    return file.isAChildOf (libraryRoot ("factory"));
}

juce::File bundledElsewhere (const juce::File& savedPath)
{
    // The part after the last "content" folder on the saved path, under this copy's content folder.
    const auto factory = libraryRoot ("factory");
    juce::StringArray parts;
    for (auto f = savedPath; f.getParentDirectory() != f; f = f.getParentDirectory())
    {
        if (f.getFileName() == factory.getFileName() && ! parts.isEmpty())
        {
            const auto candidate = factory.getChildFile (parts.joinIntoString ("/"));
            return candidate != savedPath && candidate.exists() ? candidate : juce::File();
        }
        parts.insert (0, f.getFileName());
    }
    return {};
}

juce::File defaultFolder()
{
    return platform::userDataFolder().getChildFile ("presets");
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
