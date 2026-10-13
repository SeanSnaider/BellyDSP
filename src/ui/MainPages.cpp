// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "MainPages.h"
#include "Overlay.h"

#include "platform/AppInfo.h"
#include "platform/DeviceLatency.h"
#include "platform/SampleRateGuard.h"
#include "platform/Updater.h"

namespace ui
{

using namespace theme;

namespace
{
/// A section's title row: its name and a line about it.
constexpr int titleHeight = 28, titleGap = 14;

/// A rate as people say it: "48 kHz", "44.1 kHz".
juce::String khz (double hz)
{
    const auto k = hz / 1000.0;
    return (std::abs (k - std::round (k)) < 0.05 ? juce::String (juce::roundToInt (k)) : juce::String (k, 1)) + " kHz";
}

juce::String ms (int samples, double rate)
{
    return rate > 0.0 ? juce::String (1000.0 * samples / rate, 1) + " ms" : juce::String ("-");
}

/// Rows of a name and its value, the name faint and the value in ink, a hairline between rows.
void paintRows (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<std::pair<juce::String, juce::String>>& rows, int rowHeight = 30)
{
    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto row = area.removeFromTop (rowHeight);
        if (i > 0)
        {
            g.setColour (line1);
            g.fillRect (row.withHeight (1));
        }
        g.setFont (geist (Weight::regular, 13.0f));
        g.setColour (inkDim);
        g.drawText (rows[i].first, row.removeFromLeft (150), juce::Justification::centredLeft, false);
        g.setFont (tabular (geist (Weight::medium, 13.0f)));
        g.setColour (ink);
        g.drawFittedText (rows[i].second, row, juce::Justification::centredLeft, 1, 0.85f);
    }
}

/// Lines of text, wrapped, each a paragraph with a small gap after it.
void paintParagraphs (juce::Graphics& g, juce::Rectangle<int> area, const juce::StringArray& paragraphs, juce::Colour colour, float size = 12.5f)
{
    const auto f = geist (Weight::regular, size);
    for (const auto& p : paragraphs)
    {
        juce::AttributedString text;
        text.append (p, f, colour);
        text.setWordWrap (juce::AttributedString::byWord);
        text.setLineSpacing (3.0f);
        juce::TextLayout layout;
        layout.createLayout (text, (float) area.getWidth());
        const auto h = (int) std::ceil (layout.getHeight());
        layout.draw (g, area.removeFromTop (h).toFloat());
        area.removeFromTop (8);
    }
}
} // namespace

void paintPageHeader (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& subtitle)
{
    const auto titleFont = geist (Weight::medium, 16.0f);
    g.setFont (titleFont);
    g.setColour (ink);
    const auto w = juce::roundToInt (textWidth (titleFont, title)) + 2;
    g.drawText (title, area.removeFromLeft (w), juce::Justification::centredLeft, false);
    area.removeFromLeft (space::m);
    g.setFont (font (Text::label));
    g.setColour (inkFaint);
    g.drawText (subtitle, area, juce::Justification::centredLeft, true);
}

// ---- SettingsSection ---------------------------------------------------------------------------------

juce::Rectangle<int> SettingsSection::panel (juce::Rectangle<int> area, const juce::String& title)
{
    panels.push_back ({ area, title });
    auto inside = area.reduced (padding, padding - 2);
    if (title.isNotEmpty())
        inside.removeFromTop (labelHeight + labelGap);
    return inside;
}

void SettingsSection::paintPanels (juce::Graphics& g) const
{
    for (const auto& p : panels)
    {
        overlay::paintPanel (g, p.area);
        if (p.title.isNotEmpty())
            overlay::paintGroupLabel (g, p.area.reduced (padding, padding - 2).withHeight (labelHeight), p.title);
    }
}

// ---- InputPage ---------------------------------------------------------------------------------------

InputPage::InputPage (AmpSimProcessor& p) : SettingsSection (p)
{
    input = &addKnob ("input_gain", "Input");
    levelNote = &addLabel ("Before every block: the gates' detectors and the tuner hear it too.", Text::label, inkFaint);
    levelNote->setJustificationType (juce::Justification::topLeft);
    calibrate = &addSwitch ("input_calibrate", "Calibrate to captures");
    calibrate->setTooltip ("A capture that recorded its input level hears the guitar at the level it was made with. A global setting.");
    interfaceLabel = &addLabel ("Interface level at 0 dBFS", Text::body, inkDim);
    interfaceLevel = &addField ("input_level_dbu", " dBu");
    interfaceLevel->setShowsBar (false);
    interfaceLevel->setTooltip ("The analog level that reaches 0 dBFS on your interface. Solo 4th Gen instrument input at minimum gain: +12 dBu.");
    calibrationNote = &addLabel ("A capture that says what level it was trained at hears your guitar at that level: set the interface's level here once. "
                                 "Captures without a recorded level aren't changed. Global settings, never in presets.",
                                 Text::label, inkFaint);
    calibrationNote->setJustificationType (juce::Justification::topLeft);
    routingNote = &addLabel ("The guitar comes in on the interface's input 1 (channel 0). The input gain sits before everything, the "
                             "gates' detectors and the tuner included.",
                             Text::body, inkDim);
    routingNote->setJustificationType (juce::Justification::topLeft);
}

void InputPage::paint (juce::Graphics& g)
{
    paintPageHeader (g, getLocalBounds().withHeight (titleHeight), "Input", "Where the guitar comes in, and at what level");
    paintPanels (g);
}

void InputPage::resized()
{
    panels.clear();
    auto area = getLocalBounds().withTrimmedTop (titleHeight + titleGap);
    auto row = area.removeFromTop (200);

    auto level = panel (row.removeFromLeft (250), "Level");
    input->setBounds (level.removeFromTop (Knob::preferredHeight (Knob::Size::normal)).withSizeKeepingCentre (Knob::preferredWidth (Knob::Size::normal),
                                                                                                         Knob::preferredHeight (Knob::Size::normal)));
    level.removeFromTop (space::s);
    levelNote->setBounds (level);
    row.removeFromLeft (space::m);

    auto calibration = panel (row, "Calibration");
    auto line = calibration.removeFromTop (controlHeight);
    calibrate->setBounds (line.removeFromLeft (calibrate->getPreferredWidth()).withSizeKeepingCentre (calibrate->getPreferredWidth(), Switch::preferredHeight)
                              .translated (-Switch::margin, 0));
    line.removeFromLeft (space::xxl);
    interfaceLabel->setBounds (line.removeFromLeft (170));
    interfaceLevel->setBounds (line.removeFromLeft (104));
    calibration.removeFromTop (space::l);
    calibrationNote->setBounds (calibration.withWidth (juce::jmin (calibration.getWidth(), 620)));

    area.removeFromTop (space::m);
    auto routing = panel (area.removeFromTop (100), "Routing");
    routingNote->setBounds (routing.withWidth (juce::jmin (routing.getWidth(), 760)));
}

// ---- OutputPage --------------------------------------------------------------------------------------

OutputPage::OutputPage (AmpSimProcessor& p) : SettingsSection (p)
{
    output = &addKnob ("output_gain", "Output");
    limiter = &addSwitch ("output_limit_on", "Limiter");
    limiter->setTooltip ("The output safety limiter: nothing leaves above the ceiling, with no added latency. Untouched while the level stays "
                         "2 dB under the ceiling. A global setting: presets and scenes never change it");
    ceilingLabel = &addLabel ("Ceiling", Text::label, inkFaint);
    ceiling = &addField ("output_limit_ceiling", " dBFS");
    ceiling->setTooltip ("The limiter's ceiling: the highest level the output can reach");
    ceiling->setShowsBar (false);
    saver = &addSwitch ("cpu_saver", "CPU saver");
    saver->setTooltip ("For slower computers: switched-off drives stop (and take 0.2 s to come on), the drives run at 2x, and Gain "
                       "plays its nearest of the five steps. Costs some tone. A global setting: presets and scenes never change it");
    saverNote = &addLabel ("For slower computers. Costs some tone.", Text::label, inkFaint);
    abA = &addButton ("A", [this] { if (onAbSelect) onAbSelect (false); });
    abB = &addButton ("B", [this] { if (onAbSelect) onAbSelect (true); });
    abA->setConnectedEdges (juce::Button::ConnectedOnRight);
    abB->setConnectedEdges (juce::Button::ConnectedOnLeft);
    abA->setTooltip ("Compare two versions of the settings (captures and IRs stay)");
    abB->setTooltip ("Compare two versions of the settings (captures and IRs stay)");
    abCopy.onClick = [this] { if (onAbCopy) onAbCopy(); };
    abCopy.setTooltip ("Copy what's showing into the other of A and B");
    addAndMakeVisible (abCopy);
    compareNote = &addLabel ("Two versions of the settings; captures and IRs stay. Copy puts this one in the other.", Text::label, inkFaint);
    compareNote->setJustificationType (juce::Justification::topLeft);

    tempo = &addField ("tempo_bpm", " BPM");
    tempo->setShowsBar (false);
    tap = &addButton ("Tap", [this] { if (onTap) onTap(); });
    tap->setTooltip ("Tap the tempo (or tap the footswitch's tap CC)");
    tempoNote = &addLabel ("The delay and the modulation follow it when synced.", Text::label, inkFaint);
    tempoNote->setJustificationType (juce::Justification::topLeft);

    tapLabel = &addLabel ("Tap tempo", Text::body, inkDim);
    freezeLabel = &addLabel ("Reverb freeze", Text::body, inkDim);
    sceneLabel = &addLabel ("Scenes (value 0 to 7 is scene 1 to 8)", Text::body, inkDim);
    tapCc = &addField ("midi_tap_cc", "");
    freezeCc = &addField ("midi_freeze_cc", "");
    sceneCc = &addField ("midi_scene_cc", "");
    for (auto* field : { tapCc, freezeCc, sceneCc })
    {
        field->setFormatter ([] (float cc) { return "CC " + juce::String (juce::roundToInt (cc)); });
        field->setShowsBar (false);
    }
    midi = &addButton ("MIDI mappings...", [this] { if (onMidiMappings) onMidiMappings(); });
    note = &addLabel ("Program change 1 to 3 picks the amp. Right-click any control to map a footswitch or a pedal to it, "
                      "or to see what's mapped to it. The keys below work on the main screen.",
                      Text::label, inkFaint);
    note->setJustificationType (juce::Justification::topLeft);

    addAndMakeVisible (cpu);
    for (auto* b : std::initializer_list<juce::Component*> { abA, abB, &abCopy, tap, midi })
        b->setHasFocusOutline (true);
}

OutputPage::~OutputPage() = default;

void OutputPage::refresh()
{
    const auto onB = ampSim.isOnB();
    abA->setToggleState (! onB, juce::dontSendNotification);
    abB->setToggleState (onB, juce::dontSendNotification);
}

void OutputPage::paint (juce::Graphics& g)
{
    paintPageHeader (g, getLocalBounds().withHeight (titleHeight), "Output and MIDI", "The level out, compare, tempo, the audio thread, and the footswitch");
    paintPanels (g);

    // The keys, as keycaps: what the keyboard does on the main screen.
    const std::vector<std::pair<juce::StringArray, juce::String>> keys {
        { { "@left", "@right" }, "presets" },
        { { "1", "8" }, "scenes" },
        { { "T" }, "tuner" },
        { { "Cmd Z" }, "undo" },
        { { "Shift Cmd Z" }, "redo" },
        { { "Esc" }, "closes an overlay" },
    };
    auto x = (float) keysArea.getX();
    const auto y = (float) keysArea.getCentreY();
    const auto words = geist (Weight::regular, 12.0f);
    for (const auto& [caps, word] : keys)
    {
        for (int i = 0; i < caps.size(); ++i)
        {
            if (i > 0 && caps.size() == 2 && word == "scenes")
            {
                g.setFont (words);
                g.setColour (inkFaint);
                g.drawText ("to", juce::Rectangle<float> (x, y - 8.0f, 18.0f, 16.0f), juce::Justification::centred, false);
                x += 20.0f;
            }
            x += overlay::paintKeycap (g, caps[i], x, y) + 3.0f;
        }
        x += 4.0f;
        g.setFont (words);
        g.setColour (inkDim);
        const auto w = textWidth (words, word);
        g.drawText (word, juce::Rectangle<float> (x, y - 8.0f, w + 2.0f, 16.0f), juce::Justification::centredLeft, false);
        x += w + 22.0f;
    }
}

void OutputPage::resized()
{
    panels.clear();
    auto area = getLocalBounds().withTrimmedTop (titleHeight + titleGap);
    const auto gap = space::m;

    // Row one: the level and the limiter, A/B, the tempo, the CPU.
    auto row = area.removeFromTop (190);
    auto level = panel (row.removeFromLeft (310), "Level and limiter");
    output->setBounds (level.removeFromLeft (110).withSizeKeepingCentre (Knob::preferredWidth (Knob::Size::normal), Knob::preferredHeight (Knob::Size::normal))
                           .withY (level.getY()));
    level.removeFromLeft (space::l);
    {
        auto column = level;
        limiter->setBounds (column.removeFromTop (Switch::preferredHeight).withWidth (limiter->getPreferredWidth()).translated (-Switch::margin, 0));
        column.removeFromTop (space::m);
        ceilingLabel->setBounds (column.removeFromTop (16));
        column.removeFromTop (space::xs);
        ceiling->setBounds (column.removeFromTop (controlHeight).withWidth (130));
    }
    row.removeFromLeft (gap);

    const auto rest = (row.getWidth() - 2 * gap) / 3;
    auto compare = panel (row.removeFromLeft (rest), "A/B compare");
    {
        auto line = compare.removeFromTop (controlHeight);
        abA->setBounds (line.removeFromLeft (54));
        abB->setBounds (line.removeFromLeft (54));
        line.removeFromLeft (space::s);
        abCopy.setBounds (line.removeFromLeft (controlHeight));
        compare.removeFromTop (space::m);
        compareNote->setBounds (compare);
    }
    row.removeFromLeft (gap);

    auto tempoPanel = panel (row.removeFromLeft (rest), "Tempo");
    {
        auto line = tempoPanel.removeFromTop (controlHeight);
        tempo->setBounds (line.removeFromLeft (124));
        line.removeFromLeft (space::s);
        tap->setBounds (line.removeFromLeft (64));
        tempoPanel.removeFromTop (space::m);
        tempoNote->setBounds (tempoPanel);
    }
    row.removeFromLeft (gap);

    auto cpuPanel = panel (row, "Audio thread");
    {
        cpu.setBounds (cpuPanel.removeFromTop (16).withWidth (juce::jmin (cpuPanel.getWidth(), 150)));
        cpuPanel.removeFromTop (space::m);
        saver->setBounds (cpuPanel.removeFromTop (Switch::preferredHeight).withWidth (saver->getPreferredWidth()).translated (-Switch::margin, 0));
        cpuPanel.removeFromTop (space::s);
        saverNote->setBounds (cpuPanel.removeFromTop (32));
        saverNote->setJustificationType (juce::Justification::topLeft);
    }
    area.removeFromTop (gap);

    // Row two: the footswitch's controllers, the MIDI mappings, and the keys.
    auto foot = panel (area.removeFromTop (juce::jmin (area.getHeight(), 226)), "Footswitch, MIDI, and keys");
    keysArea = foot.removeFromBottom (24);
    foot.removeFromBottom (space::m);
    auto left = foot.removeFromLeft (juce::jmin (foot.getWidth() / 2, 430));
    for (auto [label, field] : { std::pair<juce::Label*, ValueField*> { tapLabel, tapCc }, { freezeLabel, freezeCc }, { sceneLabel, sceneCc } })
    {
        auto l = left.removeFromTop (controlHeight);
        field->setBounds (l.removeFromRight (100));
        label->setBounds (l);
        left.removeFromTop (space::s);
    }
    foot.removeFromLeft (space::xxl);
    midi->setBounds (foot.removeFromTop (controlHeight).withWidth (160));
    foot.removeFromTop (space::m);
    note->setBounds (foot);
}

// ---- AudioDevicePage ---------------------------------------------------------------------------------

AudioDevicePage::AudioDevicePage (AmpSimProcessor& p) : SettingsSection (p)
{
    deviceButton = &addButton ("Audio device settings...", [] { platform::device::showSettings(); });
    deviceButton->setTooltip ("The interface, its rate and buffer, and its inputs (the standalone app's own audio settings)");
    deviceButton->setHasFocusOutline (true);
}

bool AudioDevicePage::isRateOk() const
{
    return ampSim.isSampleRateOk();
}

juce::String AudioDevicePage::getRateText() const
{
    const auto rate = ampSim.getSampleRate();
    if (rate <= 0.0)
        return "No audio is running yet.";
    if (isRateOk())
        return "Running at " + khz (rate) + ".";
    return "The device is at " + khz (rate) + ", so the output is muted. Set it to 48 kHz.";
}

void AudioDevicePage::refresh()
{
    const auto info = platform::device::currentDevice();
    const auto rate = getRateText(), dev = info.name + info.type + juce::String (info.latency.bufferSize);
    deviceButton->setEnabled (info.known);
    if (rate != shownRate || dev != shownDevice)
    {
        shownRate = rate;
        shownDevice = dev;
        repaint();
    }
}

void AudioDevicePage::paint (juce::Graphics& g)
{
    paintPageHeader (g, getLocalBounds().withHeight (titleHeight), "Audio device", "The interface BellyDSP plays through");
    paintPanels (g);

    // The requirement: 48 kHz, large, with what it means and what the device is doing now.
    {
        auto in = ratePanel;
        auto big = in.removeFromLeft (190);
        g.setFont (tabular (geist (Weight::light, 44.0f)));
        g.setColour (ink);
        g.drawText ("48 kHz", big.removeFromTop (54), juce::Justification::centredLeft, false);
        g.setFont (geist (Weight::regular, 12.0f));
        g.setColour (inkFaint);
        g.drawText ("only", big.removeFromTop (16), juce::Justification::centredLeft, false);

        in.removeFromLeft (space::l);
        const auto ok = isRateOk() && ampSim.getSampleRate() > 0.0;
        auto status = in.removeFromTop (24);
        g.setColour (ok ? accent : inkDim);
        g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ (float) status.getX() + 4.0f, (float) status.getCentreY() }));
        g.setFont (geist (Weight::medium, 14.0f));
        g.setColour (ink);
        g.drawText (getRateText(), status.withTrimmedLeft (18), juce::Justification::centredLeft, true);
        in.removeFromTop (space::s);
        paintParagraphs (g, in, { "The built-in captures are trained at 48 kHz, so BellyDSP runs at 48 kHz and nothing else. At any other rate the output "
                                  "is muted and the app says why. In the app, when the device can do 48 kHz, BellyDSP switches it back by itself "
                                  "(another app, such as a music player, can move it)." },
                         inkDim);
    }

    // The device as it runs now.
    const auto info = platform::device::currentDevice();
    if (info.known)
    {
        const auto& l = info.latency;
        paintRows (g, deviceRows,
                   { { "Device", info.name },
                     { "Driver", info.type },
                     { "Rate", khz (l.sampleRate) },
                     { "Buffer", juce::String (l.bufferSize) + " samples (" + ms (l.bufferSize, l.sampleRate) + ")" },
                     { "Latency in, out", ms (l.inputSamples, l.sampleRate) + ", " + ms (l.outputSamples, l.sampleRate) },
                     { "Round trip", ms (l.inputSamples + l.outputSamples, l.sampleRate) + " (as the device reports it)" } });
    }
    else
    {
        paintRows (g, deviceRows, { { "Rate", ampSim.getSampleRate() > 0.0 ? khz (ampSim.getSampleRate()) : juce::String ("-") } });
        paintParagraphs (g, deviceRows.withTrimmedTop (40),
                         { "No audio device of its own here: the device, its buffer, and its latencies show in the standalone app." }, inkFaint, 12.0f);
    }

    paintParagraphs (g, setupArea,
                     { "Set the interface to 48 kHz.",
                       "Turn its input gain to minimum. The level is set here, under Input, so the guitar never clips the interface.",
                       "Plug the guitar into input 1.",
                       "On Windows, use the interface's ASIO driver when it has one: it has the lowest latency." },
                     inkDim, 13.0f);
}

void AudioDevicePage::resized()
{
    panels.clear();
    auto area = getLocalBounds().withTrimmedTop (titleHeight + titleGap);
    ratePanel = panel (area.removeFromTop (150), "Sample rate");
    area.removeFromTop (space::m);
    auto row = area.removeFromTop (juce::jmin (area.getHeight(), 282));
    auto device = panel (row.removeFromLeft ((row.getWidth() - space::m) / 2), "This device");
    deviceButton->setBounds (device.removeFromBottom (controlHeight).withWidth (200));
    device.removeFromBottom (space::m);
    deviceRows = device;
    row.removeFromLeft (space::m);
    setupArea = panel (row, "Setup");
}

// ---- AboutPage ---------------------------------------------------------------------------------------

AboutPage::AboutPage (AmpSimProcessor& p) : SettingsSection (p)
{
    licences = &addButton ("All licences and notices...", [this] { if (onLicences) onLicences(); });
    licences->setTooltip ("Every licence and notice, BellyDSP's and the libraries' (THIRD_PARTY_NOTICES.txt)");
    updates = &addButton ("Check for updates...", [] { platform::updater::checkNow(); });
    source = &addButton ("Source code for this version", [] { juce::URL (platform::sourceUrlForThisVersion()).launchInDefaultBrowser(); });
    source->setTooltip (platform::sourceUrlForThisVersion());
    for (auto* b : { licences, updates, source })
        b->setHasFocusOutline (true);
    updates->setEnabled (platform::updater::isRunning());
}

void AboutPage::paint (juce::Graphics& g)
{
    paintPageHeader (g, getLocalBounds().withHeight (titleHeight), "About", "What BellyDSP is, its licences, updates, and its source");
    paintPanels (g);

    // Who and what: the mark, the name, the version, the line about it.
    {
        auto in = identity;
        auto mark = in.removeFromLeft (64).withSizeKeepingCentre (64, 64).toFloat();
        g.setColour (raised);
        g.fillRoundedRectangle (mark, 14.0f);
        g.setColour (line2);
        g.drawRoundedRectangle (mark.reduced (0.5f), 14.0f, 1.0f);
        g.setFont (geist (Weight::semibold, 30.0f));
        g.setColour (ink);
        g.drawText ("B", mark.withTrimmedRight (10.0f), juce::Justification::centred, false);
        g.setColour (accent);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ mark.getCentreX() + 13.0f, mark.getCentreY() + 9.0f }));

        in.removeFromLeft (space::l);
        auto lines = in.withSizeKeepingCentre (in.getWidth(), 64);
        auto top = lines.removeFromTop (30);
        const auto nameFont = geist (Weight::semibold, 22.0f);
        const auto name = juce::String (platform::productName);
        g.setFont (nameFont);
        g.setColour (ink);
        g.drawText (name, top, juce::Justification::centredLeft, false);
        g.setFont (tabular (geist (Weight::regular, 14.0f)));
        g.setColour (inkDim);
        g.drawText ("Version " + platform::appVersion(), top.withTrimmedLeft (juce::roundToInt (textWidth (nameFont, name)) + 14), juce::Justification::centredLeft,
                    false);
        g.setFont (geist (Weight::regular, 13.0f));
        g.setColour (inkDim);
        g.drawText ("Sean Snaider's guitar amp sim and multi-effects, with NAM captures running on NeuralAmpModelerCore. Copyright (C) 2026 Sean Snaider.",
                    lines.removeFromTop (34), juce::Justification::centredLeft, true);
    }

    paintRows (g, licenceRows,
               { { "BellyDSP", "Free software: GNU AGPL v3 or later, no warranty" },
                 { "Amp captures", "The built-in ones are Sean's, CC BY 4.0" },
                 { "Cab IRs", "The built-in ones are CC0" },
                 { "Libraries", "JUCE (AGPLv3), NeuralAmpModelerCore (MIT), Eigen (MPL 2.0), demucs.cpp (MIT)" },
                 { "Fonts", "Geist and Fraunces, SIL Open Font License" } });

    paintParagraphs (g, updatesText, { platform::updater::describe() + "." }, inkDim, 13.0f);
    paintParagraphs (g, sourceText, { "Every download is built from a tagged commit on the public repository, so this exact version's source is there:",
                                      platform::sourceUrlForThisVersion() },
                     inkDim, 13.0f);
}

void AboutPage::resized()
{
    panels.clear();
    auto area = getLocalBounds().withTrimmedTop (titleHeight + titleGap);
    identity = panel (area.removeFromTop (110), {});
    area.removeFromTop (space::m);
    auto row = area.removeFromTop (juce::jmin (area.getHeight(), 290));
    auto lic = panel (row.removeFromLeft (row.getWidth() * 3 / 5), "Licences");
    licences->setBounds (lic.removeFromBottom (controlHeight).withWidth (210));
    lic.removeFromBottom (space::m);
    licenceRows = lic;
    row.removeFromLeft (space::m);

    auto right = row;
    auto up = panel (right.removeFromTop ((right.getHeight() - space::m) * 2 / 5 + 10), "Updates");
    updates->setBounds (up.removeFromBottom (controlHeight).withWidth (180));
    up.removeFromBottom (space::s);
    updatesText = up;
    right.removeFromTop (space::m);
    auto src = panel (right, "Source");
    source->setBounds (src.removeFromBottom (controlHeight).withWidth (230));
    src.removeFromBottom (space::s);
    sourceText = src;
}

// ---- SettingsPage ------------------------------------------------------------------------------------

/// An entry in the section list: the section's name; the chosen one on a lighter fill with an orange bar at its left.
class SettingsPage::SectionButton final : public juce::Button
{
public:
    SectionButton (const juce::String& name, juce::String what) : juce::Button (name), line (std::move (what))
    {
        setClickingTogglesState (false);
        setWantsKeyboardFocus (true);
        setHasFocusOutline (true);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto r = getLocalBounds().toFloat();
        const auto on = getToggleState();
        if (on || highlighted)
        {
            g.setColour (on ? line1 : raised);
            g.fillRoundedRectangle (r, radiusControl);
        }
        if (on)
        {
            g.setColour (accent);
            g.fillRoundedRectangle (juce::Rectangle<float> (r.getX(), r.getY() + 9.0f, 3.0f, r.getHeight() - 18.0f), 1.5f);
        }
        auto words = r.reduced (16.0f, 7.0f);
        g.setFont (geist (on ? Weight::medium : Weight::regular, 14.0f));
        g.setColour (on ? ink : inkDim);
        g.drawText (getName(), words.removeFromTop (18.0f), juce::Justification::centredLeft, false);
        g.setFont (geist (Weight::regular, 11.5f));
        g.setColour (inkFaint);
        g.drawText (line, words, juce::Justification::centredLeft, true);
    }

private:
    juce::String line;
};

juce::String SettingsPage::sectionName (Section s)
{
    switch (s)
    {
        case Section::input:  return "Input";
        case Section::output: return "Output and MIDI";
        case Section::device: return "Audio device";
        case Section::about:  return "About";
    }
    return {};
}

SettingsPage::SettingsPage (AmpSimProcessor& p) : ControlGroup (p), input (p), output (p), device (p), about (p)
{
    const char* lines[] { "Level, calibration", "Level, limiter, tempo, MIDI", "48 kHz, the device", "Version, licences, source" };
    for (int i = 0; i < numSections; ++i)
    {
        auto& b = sectionButtons[(size_t) i];
        b = std::make_unique<SectionButton> (sectionName ((Section) i), lines[i]);
        b->onClick = [this, i] { showSection ((Section) i); };
        addAndMakeVisible (*b);
    }
    close.onClick = [this] { if (onClose) onClose(); };
    close.setTooltip ("Back to the main screen (Esc)");
    close.setHasFocusOutline (true);
    about.onLicences = [this] { if (onLicences) onLicences(); };
    addAndMakeVisible (close);
    addAndMakeVisible (input);
    addChildComponent (output);
    addChildComponent (device);
    addChildComponent (about);
    showSection (Section::input);
}

SettingsPage::~SettingsPage() = default;

SettingsSection& SettingsPage::getSectionPage (Section s)
{
    switch (s)
    {
        case Section::input:  return input;
        case Section::output: return output;
        case Section::device: return device;
        case Section::about:  return about;
    }
    return input;
}

juce::Button& SettingsPage::getSectionButton (Section s) noexcept
{
    return *sectionButtons[(size_t) s];
}

void SettingsPage::showSection (Section s)
{
    section = s;
    for (int i = 0; i < numSections; ++i)
    {
        sectionButtons[(size_t) i]->setToggleState (i == (int) s, juce::dontSendNotification);
        getSectionPage ((Section) i).setVisible (i == (int) s);
    }
    refresh();
}

void SettingsPage::refresh()
{
    getSectionPage (section).refresh();
}

void SettingsPage::pageShown()
{
    refresh();
}

void SettingsPage::paint (juce::Graphics& g)
{
    overlay::paintHeader (g, header, "Settings", "The limiter, the CPU saver, and the calibration are global: presets and scenes never change them.", "Esc|closes");
    // The section list's column, its hairline at the right.
    g.setColour (line1);
    g.fillRect (nav.getRight() + space::s - 1, nav.getY(), 1, nav.getHeight());
}

void SettingsPage::resized()
{
    header = getLocalBounds().withHeight (overlay::headerHeight);
    close.setBounds (overlay::closeBounds (header));
    auto body = getLocalBounds().withTrimmedTop (overlay::headerHeight + space::s);
    nav = body.removeFromLeft (navWidth - space::l);
    auto list = nav;
    for (auto& b : sectionButtons)
    {
        b->setBounds (list.removeFromTop (52));
        list.removeFromTop (space::xs);
    }
    body.removeFromLeft (space::l + space::xs);
    for (int i = 0; i < numSections; ++i)
        getSectionPage ((Section) i).setBounds (body);
}

} // namespace ui
