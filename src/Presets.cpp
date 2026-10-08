// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Presets.h"
#include "BinaryData.h"
#include "PluginProcessor.h"
#include "dsp/GainSet.h"
#include "platform/AppInfo.h"

#include <algorithm>
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

const juce::StringArray& ampKnobNames()
{
    static const juce::StringArray names { "input_trim", "depth", "bass", "mid", "treble", "presence", "output_trim" };
    return names;
}

int builtInAmpFor (const juce::String& path)
{
    // "factory:models/Glass/gainset.json", or a saved absolute path from either OS ending in
    // .../content/models/Glass/gainset.json (any copy of the app, isSavedAbsolutePath), with either separator; and the
    // pre-gain-set single files the first three replaced (content/models/Glass.nam).
    const auto normalised = path.replaceCharacter ('\\', '/');
    for (int amp = 0; amp < numBuiltInAmps; ++amp)
    {
        const auto set = "models/" + builtInAmpName (amp) + "/gainset.json";
        const auto old = "models/" + builtInAmpName (amp) + ".nam";
        for (const auto& tail : { set, old })
            if (normalised == factoryPrefix + tail || (isSavedAbsolutePath (path) && normalised.endsWith ("/content/" + tail)))
                return amp;
    }
    return -1;
}

SlotMapping mapSlots (const std::array<juce::String, 3>& captures, int selectedSlot)
{
    SlotMapping m;
    m.selectedSlot = juce::jlimit (0, 2, selectedSlot);

    // The capture of the user's own that becomes amp 9: the selected slot's if it holds one, else the lowest slot's.
    const auto isOwn = [&captures] (int k) { return captures[(size_t) k].isNotEmpty() && builtInAmpFor (captures[(size_t) k]) < 0; };
    if (isOwn (m.selectedSlot))
        m.yourCaptureSlot = m.selectedSlot;
    else
        for (int k = 0; k < 3 && m.yourCaptureSlot < 0; ++k)
            if (isOwn (k))
                m.yourCaptureSlot = k;

    for (int k = 0; k < 3; ++k)
    {
        const auto& path = captures[(size_t) k];
        const auto builtIn = builtInAmpFor (path);
        m.ampOf[(size_t) k] = path.isEmpty() ? k                     // empty: the built-in its head wore
                            : builtIn >= 0   ? builtIn                // a built-in gain set
                                             : numBuiltInAmps;        // a capture of the user's own: amp 9
    }
    return m;
}

namespace
{
juce::Identifier knobId (int amp, const juce::String& name) { return "amp" + juce::String (amp + 1) + "_" + name; }

/// The slots in the order their knobs are written into the amps' IDs: the others first, `winner` last, so where two
/// slots become one amp, the winner's values are the ones left.
std::array<int, 3> writeOrder (int winner)
{
    std::array<int, 3> order {};
    size_t i = 0;
    for (int k = 0; k < 3; ++k)
        if (k != winner)
            order[i++] = k;
    order[2] = winner;
    return order;
}

/// Moves slot knobs to amp knobs in a set of values (plain values by parameter ID), `winner` winning collisions.
void moveKnobs (juce::NamedValueSet& values, const SlotMapping& m, int winner)
{
    std::array<juce::NamedValueSet, 3> old;
    for (int k = 0; k < 3; ++k)
        for (const auto& name : ampKnobNames())
            if (const auto* v = values.getVarPointer (knobId (k, name)))
            {
                old[(size_t) k].set (name, *v);
                values.remove (knobId (k, name));
            }

    for (const auto k : writeOrder (winner))
        for (const auto& property : old[(size_t) k])
            values.set (knobId (m.ampOf[(size_t) k], property.name.toString()), property.value);
}

/// The old ID a slot-era parameter had, as the amp-era one: amp_slot is amp_model, a slot's knob its amp's knob.
juce::String movedId (const juce::String& id, const SlotMapping& m)
{
    if (id == "amp_slot")
        return "amp_model";
    for (int k = 0; k < 3; ++k)
        for (const auto& name : ampKnobNames())
            if (id == knobId (k, name).toString())
                return knobId (m.ampOf[(size_t) k], name).toString();
    return id;
}
} // namespace

void slotValuesToAmps (juce::NamedValueSet& values, const SlotMapping& m)
{
    moveKnobs (values, m, m.selectedSlot);
    values.set ("amp_model", m.ampOf[(size_t) m.selectedSlot]);
}

juce::var slotScenesToAmps (const juce::var& scenes, const SlotMapping& m)
{
    if (! scenes.isObject())
        return scenes;
    auto out = scenes.clone();
    auto* root = out.getDynamicObject();

    juce::Array<juce::var> chosen;
    if (const auto* ids = scenes.getProperty ("parameters", {}).getArray())
        for (const auto& id : *ids)
            chosen.addIfNotAlreadyThere (movedId (id.toString(), m));
    root->setProperty ("parameters", chosen);

    if (auto* list = out.getProperty ("list", {}).getArray())
        for (auto& entry : *list)
            if (auto* values = entry.getProperty ("values", {}).getDynamicObject())
            {
                auto& set = values->getProperties();
                const auto sceneSlot = set.contains ("amp_slot") ? juce::jlimit (0, 2, juce::roundToInt ((double) set["amp_slot"])) : m.selectedSlot;
                moveKnobs (set, m, sceneSlot);
                if (set.contains ("amp_slot"))
                {
                    set.remove ("amp_slot");
                    set.set ("amp_model", m.ampOf[(size_t) sceneSlot]);
                }
            }
    return out;
}

juce::var slotMidiToAmps (const juce::var& midi, const SlotMapping& m)
{
    auto out = midi.clone();
    if (auto* list = out.getArray())
        for (auto& entry : *list)
            if (auto* mapping = entry.getDynamicObject())
            {
                const auto id = mapping->getProperty ("parameter").toString();
                mapping->setProperty ("parameter", movedId (id, m));
                if (id == "amp_slot")
                    for (const auto* bound : { "min", "max" })
                        if (mapping->hasProperty (bound))
                            mapping->setProperty (bound, m.ampOf[(size_t) juce::jlimit (0, 2, juce::roundToInt ((double) mapping->getProperty (bound)))]);
            }
    return out;
}

juce::var migrateV2toV3 (const juce::var& v2)
{
    // Three slots, each any capture, to nine amps (Presets.h, slotsToAmps). The slots' captures decide which amp each
    // becomes; the built-in amps play their own sets (a path with no hash, so retrained captures under the same names
    // never warn), amp 9 the one capture of the user's own that's kept (its reference as it was, hash and all).
    auto v3 = v2.clone();
    auto* object = v3.getDynamicObject();
    object->setProperty ("format_version", 3);

    std::array<juce::String, 3> captures;
    std::array<FileRef, 3> refs;
    if (const auto* list = v2.getProperty ("amps", {}).getArray())
        for (int k = 0; k < juce::jmin (3, list->size()); ++k)
        {
            refs[(size_t) k] = FileRef::fromVar (list->getReference (k));
            captures[(size_t) k] = refs[(size_t) k].path;
        }

    const auto parameters = v2.getProperty ("parameters", {});
    const auto m = mapSlots (captures, juce::roundToInt ((double) parameters.getProperty ("amp_slot", 0)));

    if (const auto* values = parameters.getDynamicObject())
    {
        auto* moved = new juce::DynamicObject (*values);
        slotValuesToAmps (moved->getProperties(), m);
        object->setProperty ("parameters", juce::var (moved));
    }

    juce::Array<juce::var> amps;
    for (int amp = 0; amp < numBuiltInAmps; ++amp)
        amps.add (FileRef { builtInCapturePath (amp), {}, 0 }.toVar());
    amps.add ((m.yourCaptureSlot >= 0 ? refs[(size_t) m.yourCaptureSlot] : FileRef {}).toVar());
    object->setProperty ("amps", amps);

    // Each slot's cab for "Follow amp choice" is now its amp's (the selected slot's wins a collision).
    if (const auto* assigned = v2.getProperty ("cab_assign", {}).getArray())
    {
        std::array<juce::var, numBuiltInAmps + 1> perAmp;
        perAmp.fill (FileRef {}.toVar());
        for (const auto k : writeOrder (m.selectedSlot))
            if (k < assigned->size() && FileRef::fromVar (assigned->getReference (k)).path.isNotEmpty())
                perAmp[(size_t) m.ampOf[(size_t) k]] = assigned->getReference (k);
        juce::Array<juce::var> list;
        for (const auto& v : perAmp)
            list.add (v);
        object->setProperty ("cab_assign", list);
    }

    if (v2.hasProperty ("scenes"))
        object->setProperty ("scenes", slotScenesToAmps (v2.getProperty ("scenes", {}), m));
    if (v2.hasProperty ("midi"))
        object->setProperty ("midi", slotMidiToAmps (v2.getProperty ("midi", {}), m));
    return v3;
}

juce::var migrate (const juce::var& preset)
{
    auto current = preset;
    if ((int) current.getProperty ("format_version", 0) == 1)
        current = migrateV1toV2 (current);
    if ((int) current.getProperty ("format_version", 0) == 2)
        current = migrateV2toV3 (current);
    return current;
}

bool isGlobal (const juce::String& parameterId)
{
    return parameterId == "input_calibrate" || parameterId == "input_level_dbu" || parameterId == "drive_oversampling"
           || parameterId == "cpu_saver" || parameterId.startsWith ("tuner_") || parameterId.startsWith ("output_limit_");
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

    // A built-in amp playing its own gain set is saved as the factory path alone: no hash, so a preset never warns
    // that a built-in "has changed" when its captures are retrained under the same name.
    juce::Array<juce::var> amps;
    for (int a = 0; a < AmpSimProcessor::numAmps; ++a)
    {
        const auto path = state.getProperty (AmpSimProcessor::modelPathKey (a)).toString();
        if (a < numBuiltInAmps && juce::File::isAbsolutePath (path) && juce::File (path) == builtInCapture (a))
            amps.add (FileRef { builtInCapturePath (a), {}, 0 }.toVar());
        else
            amps.add (refFor (path, "models"));
    }
    root->setProperty ("amps", amps);

    auto* cab = new juce::DynamicObject();
    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
        cab->setProperty (micKeys[m], refFor (state.getProperty (AmpSimProcessor::cabPathKey (m)).toString(), "irs"));
    root->setProperty ("cab", juce::var (cab));

    // Follow amp choice: each amp's cab, and whether switching amps loads it.
    juce::Array<juce::var> assigned;
    for (int a = 0; a < AmpSimProcessor::numAmps; ++a)
        assigned.add (refFor (state.getProperty (AmpSimProcessor::cabAssignKey (a)).toString(), "irs"));
    root->setProperty ("cab_assign", assigned);
    root->setProperty ("cab_follow", processor.isCabFollowing());

    // The match curve's points (only when there is one: a preset without them has none).
    if (const auto curve = processor.getMatchCurve(); ! curve.isFlat())
        root->setProperty ("match_curve", curve.toVar());

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

    // Each amp's capture. One the amp already holds (the built-in amps, nearly always) isn't loaded again: nine
    // reloads would keep a preset change waiting seconds for the same models. The amp the preset plays goes on the
    // (one) loader first, then the cab, then the other amps, which the preset's fade-in doesn't wait for
    // (AmpSimProcessor::isLoadingWhatPlays).
    const auto* amps = preset.getProperty ("amps", {}).getArray();
    const auto loadAmp = [&] (int a)
    {
        const auto ref = amps != nullptr && a < amps->size() ? FileRef::fromVar (amps->getReference (a)) : FileRef {};
        if (ref.path.isEmpty())
        {
            processor.clearModel (a);
            return;
        }
        const auto r = resolve (ref, "models");
        describe ((a == AmpSimProcessor::yourCaptureAmp ? juce::String ("Your capture") : builtInAmpName (a) + "'s capture"), ref, r);
        if (r.found && r.file.existsAsFile())
            processor.loadModelIfChanged (a, r.file);
        else
            processor.clearModel (a);
    };
    const auto playing = processor.getSelectedAmp();
    loadAmp (playing);

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

    for (int a = 0; a < AmpSimProcessor::numAmps; ++a)
        if (a != playing)
            loadAmp (a);

    // Follow amp choice. A preset made before it existed assigns nothing and follows (the default); the
    // preset's own close mic 1 is what plays now, so the amp it selects counts as followed already.
    const auto* assigned = preset.getProperty ("cab_assign", {}).getArray();
    for (int a = 0; a < AmpSimProcessor::numAmps; ++a)
    {
        const auto ref = assigned != nullptr && a < assigned->size() ? FileRef::fromVar (assigned->getReference (a)) : FileRef {};
        auto file = juce::File();
        if (ref.path.isNotEmpty())
        {
            const auto r = resolve (ref, "irs");
            describe ("Amp " + juce::String (a + 1) + "'s cab", ref, r);
            if (r.found)
                file = r.file;
        }
        processor.setCabAssignment (a, file);
    }
    processor.parameters.state.setProperty (AmpSimProcessor::cabFollowKey, (bool) preset.getProperty ("cab_follow", true), nullptr);
    processor.markCabFollowed();

    // The match curve belongs to the preset: one without a "match_curve" (every preset saved before it
    // existed) has none. Points that aren't [hz, dB] pairs are dropped.
    {
        const auto curvePoints = preset.getProperty ("match_curve", {});
        if (! curvePoints.isVoid() && ! curvePoints.isArray())
            result.warnings.add ("The match curve isn't a list of points; skipped");
        processor.setMatchCurve (ampsim::MatchCurve::Curve::fromVar (curvePoints));
    }

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

juce::String builtInAmpName (int amp)
{
    static const char* names[] = { "Glass", "Ember", "Monolith", "Lantern", "Basalt", "Comet", "Forge", "Quartz" };
    static_assert (std::size (names) == (size_t) numBuiltInAmps);
    return names[juce::jlimit (0, numBuiltInAmps - 1, amp)];
}

juce::String builtInCapturePath (int amp)
{
    return factoryPrefix + "models/" + builtInAmpName (amp) + "/gainset.json";
}

juce::File replacementForRetiredCapture (const juce::String& path)
{
    // "factory:models/Glass.nam", or a saved absolute path from either OS (isSavedAbsolutePath: a state saved
    // on one OS can be loaded on the other, and "/Applications/BellyDSP.app/..." isn't absolute to Windows,
    // found by the first Windows CI run) ending in .../content/models/Glass.nam, with either separator.
    const auto normalised = path.replaceCharacter ('\\', '/');
    for (int amp = 0; amp < 3; ++amp) // only the first three ever shipped as single files
    {
        const auto old = "models/" + builtInAmpName (amp) + ".nam";
        if (normalised == factoryPrefix + old || (isSavedAbsolutePath (path) && normalised.endsWith ("/content/" + old)))
            return builtInCapture (amp);
    }
    return {};
}

bool isSavedAbsolutePath (const juce::String& path)
{
    const auto posix = path.startsWithChar ('/');
    const auto windows = path.startsWith ("\\\\") || (juce::CharacterFunctions::isLetter (path[0]) && path[1] == ':');
    return juce::File::isAbsolutePath (path) || posix || windows;
}

juce::File builtInCapture (int amp)
{
    return resolvePath (builtInCapturePath (amp));
}

std::vector<BuiltInAmp> builtInGainSets()
{
    std::vector<BuiltInAmp> sets;
    const auto folder = libraryRoot ("factory").getChildFile ("models");
    for (const auto& dir : folder.findChildFiles (juce::File::findDirectories, false))
    {
        ampsim::GainSet set;
        juce::String error;
        const auto json = dir.getChildFile (ampsim::GainSet::conventionalFileName);
        if (json.existsAsFile() && ampsim::GainSet::read (json, set, error))
            sets.push_back ({ set.name.isNotEmpty() ? set.name : dir.getFileName(), set.description, set.toneType, json });
    }
    // The built-in amps in their order, then any other set by tone type, then name.
    const auto rank = [] (const BuiltInAmp& a)
    {
        for (int amp = 0; amp < numBuiltInAmps; ++amp)
            if (a.file == builtInCapture (amp))
                return amp;
        static const juce::StringArray tones { "clean", "overdrive", "crunch", "hi_gain", "fuzz" };
        const auto t = tones.indexOf (a.toneType);
        return numBuiltInAmps + (t < 0 ? tones.size() : t);
    };
    std::sort (sets.begin(), sets.end(), [&] (const BuiltInAmp& a, const BuiltInAmp& b)
    {
        const auto ra = rank (a), rb = rank (b);
        return ra != rb ? ra < rb : a.name.compareNatural (b.name) < 0;
    });
    return sets;
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
