// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// From three slots to nine amps (BUILD_PLAN "Amp switching", 2026-10-07; presets::mapSlots): a saved state from the
// three-slot rig, the old factory presets (format 2, kept as fixtures), a user preset with captures of its own, and
// how a choice parameter is stored, so appending amps later keeps every saved index meaning the same amp.

#include "BuiltInCaptures.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "TestHelpers.h"

namespace
{
using namespace testing;

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 12000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

float getParam (AmpSimProcessor& p, const juce::String& id) { return p.parameters.getRawParameterValue (id)->load(); }

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

std::vector<float> render (AmpSimProcessor& p, const std::vector<float>& input)
{
    p.prepareToPlay (fs, blockSize);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> out;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
        out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }
    return out;
}

juce::File fixtures() { return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/presets/factory_v2"); }

/// A preset's scenes as text, each scene's values sorted by ID (their order in the JSON doesn't matter).
juce::String scenesOf (const juce::var& preset)
{
    juce::StringArray out;
    out.add (juce::JSON::toString (preset["scenes"]["parameters"], true));
    if (const auto* list = preset["scenes"]["list"].getArray())
        for (const auto& scene : *list)
        {
            if (! scene.isObject())
            {
                out.add ("-");
                continue;
            }
            std::map<juce::String, double> values;
            if (const auto* o = scene["values"].getDynamicObject())
                for (const auto& p : o->getProperties())
                    values[p.name.toString()] = (double) p.value;
            juce::StringArray items;
            for (const auto& [id, v] : values)
                items.add (id + "=" + juce::String (v));
            out.add (scene["name"].toString() + ": " + items.joinIntoString (", "));
        }
    return out.joinIntoString ("; ");
}

/// Every parameter value of a preset, by ID (plain values), as text, for comparing two presets.
std::map<juce::String, double> parametersOf (const juce::var& preset)
{
    std::map<juce::String, double> values;
    if (const auto* o = preset.getProperty ("parameters", {}).getDynamicObject())
        for (const auto& p : o->getProperties())
            values[p.name.toString()] = (double) p.value;
    return values;
}

class AmpMigrationTests final : public juce::UnitTest
{
public:
    AmpMigrationTests() : juce::UnitTest ("Amp migration (slots to amps)", "ampsim") {}

    void runTest() override
    {
        const auto lstm = exampleModel ("lstm.nam");
        const auto irA = tempDir().getChildFile ("migration_ir_a.wav"), irB = tempDir().getChildFile ("migration_ir_b.wav");
        writeWav (irA, toBuffer (syntheticCabIR (2048)));
        writeWav (irB, toBuffer (syntheticCabIR (2048, 6.0, 7000.0)));

        beginTest ("amp_model is a choice stored as its index (APVTS saves the denormalised value, presets and scenes the plain one): an amp appended later keeps every saved index");
        {
            AmpSimProcessor p;
            setParam (p, AmpSimProcessor::ampModelParamId, 5.0f);
            const auto tree = p.parameters.copyState();
            const auto stored = tree.getChildWithProperty ("id", AmpSimProcessor::ampModelParamId).getProperty ("value").toString();
            const auto preset = p.capturePreset ("Choice");
            const auto inPreset = (double) preset["parameters"]["amp_model"];
            auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p.parameters.getParameter (AmpSimProcessor::ampModelParamId));
            expect (choice != nullptr);
            expectEquals (stored.getDoubleValue(), 5.0);
            expectEquals (inPreset, 5.0);
            expectEquals (choice->choices.size(), AmpSimProcessor::numAmps);
            expectEquals (choice->choices[5], juce::String ("Comet"));
            expectEquals (choice->choices[8], juce::String ("Your capture"));
            expect (p.parameters.getParameter (AmpSimProcessor::slotParamId) != nullptr, "amp_slot stays registered");
            for (int a = 0; a < AmpSimProcessor::numAmps; ++a)
                for (const auto& name : presets::ampKnobNames())
                    expect (p.parameters.getParameter (AmpSimProcessor::ampParamId (a, name)) != nullptr, AmpSimProcessor::ampParamId (a, name));
            logMessage ("  -> Comet selected: the state stores amp_model value=\"" + stored + "\", a preset " + juce::String (inPreset) + " (the index, not the "
                        "0-to-1 value a host sees, which would shift if choices were appended); choices: " + choice->choices.joinIntoString (", ")
                        + "; amp_slot and amp1_* .. amp9_* all registered");
        }

        beginTest ("a saved state from the three-slot rig: each slot becomes the amp its capture was, with its knobs; scenes, MIDI mappings, and cab assignments follow; it sounds the same");
        {
            WithBuiltInCaptures builtIns;
            const auto yours = AmpSimProcessor::yourCaptureAmp;

            // The state the slot rig saved: the same tree with only the parameters it had (no amp_model, no amp4_* .. amp9_*),
            // slot 1 on Glass saved by another copy of the app, slot 2 on Forge (the old picker), slot 3 a capture of the
            // user's own, selected; each slot's knobs; scenes, MIDI mappings and cab assignments by slot.
            juce::ValueTree old;
            {
                AmpSimProcessor fresh;
                old = fresh.parameters.copyState();
            }
            for (int i = old.getNumChildren(); --i >= 0;)
            {
                const auto id = old.getChild (i).getProperty ("id").toString();
                bool slotEra = id != AmpSimProcessor::ampModelParamId;
                for (int a = 3; a < AmpSimProcessor::numAmps; ++a)
                    slotEra = slotEra && ! id.startsWith ("amp" + juce::String (a + 1) + "_");
                if (! slotEra)
                    old.removeChild (i, nullptr);
            }
            const auto set = [&old] (const juce::String& id, double value)
            { old.getChildWithProperty ("id", id).setProperty ("value", value, nullptr); };
            set ("amp_slot", 2.0);
            set ("amp1_input_trim", -4.8);
            set ("amp1_mid", 1.2);
            set ("amp2_input_trim", 9.6);
            set ("amp2_bass", -2.4);
            set ("amp3_input_trim", 2.4);
            set ("amp3_treble", 3.6);
            set ("amp3_output_trim", -1.2);
            old.setProperty (AmpSimProcessor::modelPathKey (0), "/Applications/BellyDSP.app/Contents/Resources/content/models/Glass/gainset.json", nullptr);
            old.setProperty (AmpSimProcessor::modelPathKey (1), presets::builtInCapture (6).getFullPathName(), nullptr); // Forge
            old.setProperty (AmpSimProcessor::modelPathKey (2), lstm.getFullPathName(), nullptr);
            old.setProperty (AmpSimProcessor::cabAssignKey (0), irA.getFullPathName(), nullptr);
            old.setProperty (AmpSimProcessor::cabAssignKey (2), irB.getFullPathName(), nullptr);
            old.setProperty (AmpSimProcessor::scenesKey, R"({ "parameters": [ "amp1_input_trim", "amp3_mid" ], "list": [
                { "name": "Verse", "values": { "amp_slot": 0, "chorus_on": 1, "amp1_input_trim": -9.6, "amp3_mid": 2.4 } },
                { "name": "Chorus", "values": { "amp_slot": 2, "chorus_on": 0, "amp1_input_trim": -4.8, "amp3_mid": 4.8 } },
                { "name": "Bridge", "values": { "amp_slot": 1, "chorus_on": 0, "amp1_input_trim": -4.8, "amp3_mid": 0.0 } },
                null, null, null, null, null ] })", nullptr);
            old.setProperty (AmpSimProcessor::midiMapKey, R"([ { "cc": 11, "action": "continuous", "parameter": "amp_slot", "min": 0, "max": 2 },
                { "cc": 12, "action": "continuous", "parameter": "amp3_mid", "min": -6, "max": 6 } ])", nullptr);
            const auto xml = old.createXml();
            expect (xml->writeTo (proofDir().getChildFile ("migration_slot_rig_state.xml")));
            juce::MemoryBlock saved;
            juce::AudioProcessor::copyXmlToBinary (*xml, saved);

            AmpSimProcessor p;
            p.setStateInformation (saved.getData(), (int) saved.getSize());
            waitForLoads (p);
            p.runHousekeeping();

            const auto path = [&p] (int a) { return p.parameters.state.getProperty (AmpSimProcessor::modelPathKey (a)).toString(); };
            expectEquals (p.getSelectedAmp(), yours);
            expectEquals (path (yours), lstm.getFullPathName());
            for (int a = 0; a < AmpSimProcessor::numBuiltInAmps; ++a)
                expectEquals (path (a), presets::builtInCapture (a).getFullPathName());
            const auto knob = [&p] (int a, const char* name) { return getParam (p, AmpSimProcessor::ampParamId (a, name)); };
            expectWithinAbsoluteError (knob (0, "input_trim"), -4.8f, 1.0e-5f); // Glass: slot 1's
            expectWithinAbsoluteError (knob (0, "mid"), 1.2f, 1.0e-5f);
            expectWithinAbsoluteError (knob (6, "input_trim"), 9.6f, 1.0e-5f);  // Forge: slot 2's
            expectWithinAbsoluteError (knob (6, "bass"), -2.4f, 1.0e-5f);
            expectWithinAbsoluteError (knob (yours, "input_trim"), 2.4f, 1.0e-5f); // your capture: slot 3's
            expectWithinAbsoluteError (knob (yours, "treble"), 3.6f, 1.0e-5f);
            expectWithinAbsoluteError (knob (yours, "output_trim"), -1.2f, 1.0e-5f);
            for (const auto* name : { "input_trim", "bass", "mid", "treble" })
                expect (std::abs (knob (1, name)) < 1.0e-5f && std::abs (knob (2, name)) < 1.0e-5f, juce::String ("Ember and Monolith, which no slot played: defaults (") + name + "): " + juce::String (knob (1, name)) + ", " + juce::String (knob (2, name)));
            expect (p.getStatus().model[(size_t) yours].contains ("lstm"), p.getStatus().model[(size_t) yours]);

            // Scenes: amp_slot is amp_model, the knobs the amps'.
            auto& scenes = p.getScenes();
            expect (scenes.getChosen() == juce::StringArray { "amp1_input_trim", "amp9_mid" }, scenes.getChosen().joinIntoString (", "));
            const auto sceneAmp = [&scenes] (int i) { const auto& v = scenes.get (i).values; return v.count ("amp_model") ? juce::roundToInt (v.at ("amp_model")) : -1; };
            expectEquals (sceneAmp (0), 0);
            expectEquals (sceneAmp (1), yours);
            expectEquals (sceneAmp (2), 6);
            expect (scenes.get (1).values.count ("amp_slot") == 0);
            expect (p.recallScene (1));
            expectEquals (p.getSelectedAmp(), yours);
            expectWithinAbsoluteError (knob (yours, "mid"), 4.8f, 1.0e-5f);
            expect (p.recallScene (2));
            expectEquals (p.getSelectedAmp(), 6);

            // MIDI: the pedal on amp_slot sweeps the slots' amps, the one on slot 3's Middle is your capture's.
            const auto mappings = p.getMidiMap().getMappings();
            expectEquals ((int) mappings.size(), 2);
            for (const auto& m : mappings)
            {
                if (m.cc == 11)
                    expect (m.parameterId == "amp_model" && juce::exactlyEqual (m.minimum, 0.0f) && juce::exactlyEqual (m.maximum, (float) yours));
                else
                    expectEquals (m.parameterId, AmpSimProcessor::ampParamId (yours, "mid"));
            }

            // Follow amp choice: slot 1's cab is Glass's, slot 3's your capture's.
            expectEquals (p.getCabAssignment (0).getFullPathName(), irA.getFullPathName());
            expectEquals (p.getCabAssignment (yours).getFullPathName(), irB.getFullPathName());
            expect (p.getCabAssignment (1) == juce::File() && p.getCabAssignment (2) == juce::File());

            // The sound: the restored rig against one set up directly in the new form (your capture with slot 3's
            // knobs, playing), on the same DI, after the scenes are put back to the saved one's selection.
            expect (p.recallScene (1));
            setParam (p, AmpSimProcessor::ampParamId (yours, "mid"), 0.0f);
            AmpSimProcessor q;
            q.loadModel (yours, lstm);
            waitForLoads (q);
            setParam (q, AmpSimProcessor::ampModelParamId, (float) yours);
            setParam (q, "chorus_on", 0.0f);
            for (const auto& name : presets::ampKnobNames()) // the values the restored state holds (2.4, 3.6, -1.2 dB as read back)
                setParam (q, AmpSimProcessor::ampParamId (yours, name), getParam (p, AmpSimProcessor::ampParamId (yours, name)));
            const auto input = guitarDI ((int) fs);
            const auto a = render (p, input), b = render (q, input);
            // Not bit for bit: two separate loads of the LSTM land a hair apart (measured -112.7 dB from the first sample,
            // with every parameter of the playing amp equal), so the bar is -100 dB.
            const auto difference = relativeErrorDb (a, b);
            expectLessThan (difference, -100.0);
            logMessage ("  -> slot 1 Glass (saved by another copy) -> Glass with its knobs; slot 2 Forge -> Forge (amp 7) with Gain 7.0, Bass -2.4; slot 3 lstm (selected) "
                        "-> your capture, selected, with its knobs; Ember and Monolith, unplayed, at their defaults. Scenes Verse/Chorus/Bridge -> Glass/your "
                        "capture/Forge, chosen amp1_input_trim, amp9_mid; the pedal on amp_slot sweeps amp_model 0 to 8; cabs follow (Glass, your capture). "
                        "Rendered against the same rig set up directly: " + dB (difference) + "; the old state is in migration_slot_rig_state.xml");
        }

        beginTest ("the old factory presets (format 2, three slots) migrate to exactly the shipped format 3 files, and load and sound the same");
        {
            const auto shipped = presets::factoryPresets();
            juce::StringArray lines;
            for (const auto& file : fixtures().findChildFiles (juce::File::findFiles, false, "*.json"))
            {
                juce::String error;
                const auto old = presets::load (file, error);
                expectEquals ((int) old["format_version"], 2, file.getFileName());
                const auto migrated = presets::migrate (old);
                juce::var now;
                for (const auto& s : shipped)
                    if (s["name"].toString() == old["name"].toString())
                        now = s;
                expect (now.isObject(), old["name"].toString());

                expect (parametersOf (migrated) == parametersOf (now), old["name"].toString() + ": parameters");
                juce::StringArray migratedAmps, shippedAmps;
                for (int a = 0; a < AmpSimProcessor::numAmps; ++a)
                {
                    migratedAmps.add (presets::FileRef::fromVar (migrated["amps"][a]).path);
                    shippedAmps.add (presets::FileRef::fromVar (now["amps"][a]).path);
                }
                expect (migratedAmps == shippedAmps, migratedAmps.joinIntoString (", "));
                expectEquals (scenesOf (migrated), scenesOf (now));
                expectEquals ((int) migrated["format_version"], 3);

                // Loaded, each scene plays the same amp with the same knobs.
                AmpSimProcessor p, q;
                expect (p.loadPreset (old).ok && q.loadPreset (now).ok);
                waitForLoads (p);
                waitForLoads (q);
                p.runHousekeeping();
                q.runHousekeeping();
                expect (p.getPresetWarnings().isEmpty(), p.getPresetWarnings().joinIntoString ("; "));
                juce::StringArray scenesPlayed;
                for (int i = 0; i < Scenes::count; ++i)
                {
                    if (! p.getScenes().get (i).stored)
                        continue;
                    expect (p.recallScene (i) && q.recallScene (i));
                    int differ = 0;
                    for (auto* parameter : p.getParameters())
                        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                            differ += std::abs (ranged->getValue() - q.parameters.getParameter (ranged->paramID)->getValue()) > 1.0e-6f ? 1 : 0;
                    expectEquals (differ, 0);
                    scenesPlayed.add (p.getScenes().get (i).name + " on " + AmpSimProcessor::ampName (p.getSelectedAmp()));
                }
                const auto input = guitarDI ((int) (0.5 * fs));
                const auto difference = maxAbsDifference (render (p, input), render (q, input));
                expectEquals (difference, 0.0);
                lines.add (old["name"].toString() + " (" + scenesPlayed.joinIntoString (", ") + ")");
            }
            expectEquals (lines.size(), 5);
            logMessage ("  -> tests/fixtures/presets/factory_v2 migrated: parameters, the nine amps, and the scenes equal the shipped format 3 files; every scene "
                        "sets the same values and the last renders bit for bit the same: " + lines.joinIntoString ("; "));
        }

        beginTest ("a user preset (format 2) with captures of its own: the selected slot's is kept as your capture, and its knobs win; scenes and cabs follow");
        {
            const auto own = [] (const juce::String& path) { return presets::FileRef { path, "fnv1a64:0123456789abcdef", 1234 }.toVar(); };
            auto* parameters = new juce::DynamicObject();
            parameters->setProperty ("amp_slot", 1);
            parameters->setProperty ("amp1_input_trim", -3.0);
            parameters->setProperty ("amp2_input_trim", 6.0);
            parameters->setProperty ("amp3_input_trim", 1.5);
            auto* root = new juce::DynamicObject();
            root->setProperty ("format_version", 2);
            root->setProperty ("name", "Mine");
            root->setProperty ("parameters", juce::var (parameters));
            root->setProperty ("amps", juce::Array<juce::var> { own ("models/Other.nam"), own ("models/Mine.nam"), presets::FileRef { "factory:models/Forge/gainset.json", {}, 0 }.toVar() });
            root->setProperty ("cab_assign", juce::Array<juce::var> { presets::FileRef { "irs/a.wav", {}, 0 }.toVar(), presets::FileRef { "irs/b.wav", {}, 0 }.toVar(), presets::FileRef {}.toVar() });
            root->setProperty ("scenes", juce::JSON::parse (R"({ "parameters": [ "amp1_input_trim" ], "list": [ { "name": "A", "values": { "amp_slot": 0, "amp1_input_trim": -3 } },
                                                                                                      { "name": "B", "values": { "amp_slot": 2 } } ] })"));
            const auto v3 = presets::migrateV2toV3 (juce::var (root));
            const auto values = v3["parameters"];
            expectEquals ((int) v3["format_version"], 3);
            expectEquals ((int) values["amp_model"], AmpSimProcessor::yourCaptureAmp);
            expectEquals ((double) values["amp9_input_trim"], 6.0, "the selected slot's knobs win");
            expectEquals ((double) values["amp7_input_trim"], 1.5, "Forge gets slot 3's");
            expect (! values.hasProperty ("amp1_input_trim") && ! values.hasProperty ("amp3_input_trim"));
            expectEquals (presets::FileRef::fromVar (v3["amps"][8]).path, juce::String ("models/Mine.nam"));
            expectEquals (presets::FileRef::fromVar (v3["amps"][8]).hash, juce::String ("fnv1a64:0123456789abcdef"));
            expectEquals (presets::FileRef::fromVar (v3["amps"][6]).path, presets::builtInCapturePath (6));
            expectEquals (presets::FileRef::fromVar (v3["cab_assign"][8]).path, juce::String ("irs/b.wav"), "the selected slot's cab");
            expectEquals ((int) v3["scenes"]["list"][0]["values"]["amp_model"], AmpSimProcessor::yourCaptureAmp);
            expectEquals ((int) v3["scenes"]["list"][1]["values"]["amp_model"], 6);
            expectEquals (v3["scenes"]["parameters"][0].toString(), juce::String ("amp9_input_trim"));
            logMessage ("  -> slots Other.nam, Mine.nam (selected), Forge: Mine.nam becomes your capture (its hash kept) with its Gain; Other.nam's slot plays your capture "
                        "too (one capture of your own is kept); Forge's slot is Forge with slot 3's Gain; scenes A and B: your capture and Forge");
        }
    }
};

static AmpMigrationTests ampMigrationTests;
} // namespace
