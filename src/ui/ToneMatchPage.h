// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Pages.h"
#include "ToneMatchSession.h"
#include "tonematch/GuitarSeparator.h"

namespace ui
{

/// The tone match page (docs/TONE_MATCH.md), opened from the brand menu's "Match tone...". The UI handoff
/// didn't design it, so it's a quiet page in the handoff's tokens and components (ASSUMPTIONS TM): a header
/// with the honest note, three cards across (Target, Your DI, Match), and the result under them.
///
///   Target    Choose file... (or drop one on the page), the waveform with the range to match (drag to
///             select, drag an edge or the middle to adjust; 3 to 60 s), Play (the section, looped; Full song
///             or Guitar only once a separated match made a stem; its level), and, when the build has it,
///             "Separate the guitar first"; "Count-in before Play".
///   Your DI   Record: with a target loaded, a play-along take (the count-in, then the section once while
///             your DI records, lined up with it; docs/TONE_MATCH.md, "Play along"); without one, the clean
///             DI up to a minute. Or Choose file... The mode: Same part or Anything. The count-in (on, 4 or
///             2 beats, its tempo with Tap and the section's measured tempo as a suggestion, the click's
///             level) and the latency offset.
///   Match     the button, the progress with its stage, and Cancel.
///   Result    the amp, Gain, tone, cab, and match EQ found; the closeness score with what it does and doesn't
///             mean; the match EQ curve over what it was fitted to; Apply (one undo step) and Discard.
///
/// It owns the ToneMatchSession, which does the work.
class ToneMatchPage final : public ControlGroup, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit ToneMatchPage (AmpSimProcessor& processor);
    ~ToneMatchPage() override;

    ToneMatchSession& getSession() noexcept { return session; }

    /// The page's Close button.
    std::function<void()> onClose;

    void refresh() override;
    void pageShown() override;
    void pageHidden() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    /// What the buttons do (tests call these as the clicks do).
    void chooseTarget();
    void chooseReference();
    void toggleRecording();
    void startMatch();
    void apply();
    void discard();

    /// Where the separation model lives and comes from: the data folder's Separation and the pinned URL,
    /// unless a test points it elsewhere (a temporary folder, a local server). Not while matching.
    void setSeparationSource (const juce::File& modelFolder, const juce::String& url);
    ampsim::tonematch::GuitarSeparator& getSeparator() noexcept { return *separator; }

    /// One line of the result ("Match EQ  low shelf 134 Hz -3.2 dB, ...") broken to fit `width`, only
    /// between items, so a value never parts from its unit. Continuation lines are drawn indented by
    /// the label's width; the first line starts with the label.
    static juce::StringArray packItems (const juce::Font& font, const juce::String& label, const juce::StringArray& items, float width);

    /// For tests: what's shown.
    juce::String getResultText() const { return resultText; }
    juce::String getStatusText() const { return statusText; }
    /// The note above Apply: what it sets, the pre effects it switches off, and which of them are on now.
    juce::String getApplyNote() const;
    juce::TextButton& getMatchButton() noexcept { return *matchButton; }
    juce::TextButton& getApplyButton() noexcept { return *applyButton; }
    juce::TextButton& getRecordButton() noexcept { return *recordButton; }
    juce::TextButton& getTargetPlayButton() noexcept { return *targetPlayButton; }
    juce::TextButton& getTapButton() noexcept { return *tapButton; }
    juce::TextButton& getSuggestionButton() noexcept { return *suggestionButton; }
    Segmented& getModeChoice() noexcept { return *modeChoice; }

    /// The waveform: the target's peaks, with the selected range. Drag to select.
    class Waveform;
    /// The match EQ over the residual it was fitted to, with the target's and the match's long-term spectra.
    class EqCurve;
    /// The comparison's loop: the matched section's waveform, the loop on it (drag to set), and the playhead.
    class LoopStrip;
    /// The preview level: a short horizontal bar, dragged.
    class LevelBar;
    /// A number in a field (the count-in's tempo, the click's level, the latency offset): drag up or down,
    /// scroll, or double-click to type.
    class NumberField;

    // ---- Hearing the target, and playing along ----------------------------------------------------------
    /// The Target card's Play: the section, looped.
    void toggleTargetPlay();
    Segmented& getSongChoice() noexcept { return *songChoice; }
    LevelBar& getSongLevelBar() noexcept { return *songLevelBar; }
    Switch& getCountInSwitch() noexcept { return *countInSwitch; }
    Switch& getCountInPlaySwitch() noexcept { return *countInPlaySwitch; }
    Segmented& getBeatsChoice() noexcept { return *beatsChoice; }
    NumberField& getBpmField() noexcept { return *bpmField; }
    NumberField& getClickField() noexcept { return *clickField; }
    NumberField& getOffsetField() noexcept { return *offsetField; }
    /// The line under Your DI's status: what's recording, or how the take was lined up.
    juce::String getReferenceStatus() const;

    // ---- Comparing (A/B) ----------------------------------------------------------------------------------
    /// Play or stop the comparison (the Play button, Space).
    void togglePreview();
    /// Target, Match, or Current (the buttons, 1, 2, 3).
    void selectSource (int source);
    bool keyPressed (const juce::KeyPress& key) override;
    juce::TextButton& getPlayButton() noexcept { return *playButton; }
    Segmented& getSourceChoice() noexcept { return *sourceChoice; }
    Switch& getLevelMatchSwitch() noexcept { return *levelMatchSwitch; }
    Switch& getMuteSwitch() noexcept { return *muteSwitch; }
    LoopStrip& getLoopStrip() noexcept { return *loopStrip; }
    LevelBar& getLevelBar() noexcept { return *levelBar; }
    /// The line under the sources: each one's loudness as it is (LUFS).
    juce::String getLoudnessText() const;

private:
    void timerCallback() override;
    void targetChanged();
    void updateResultText();

    // Declared before the session, so it outlives the session's worker (which may be using it).
    std::unique_ptr<ampsim::tonematch::GuitarSeparator> separator;
    juce::String separationUrl { ampsim::tonematch::GuitarSeparator::weightsUrl };
    ToneMatchSession session;
    std::unique_ptr<Waveform> waveform;
    std::unique_ptr<EqCurve> eqCurve;
    std::unique_ptr<LoopStrip> loopStrip;
    std::unique_ptr<LevelBar> levelBar;
    std::unique_ptr<Segmented> sourceChoice;
    std::unique_ptr<Switch> levelMatchSwitch, muteSwitch;
    juce::TextButton* playButton = nullptr;
    juce::Rectangle<int> compareArea, loudnessRow, levelLabelArea, levelValueArea;
    bool spectraShown = false;
    std::unique_ptr<Segmented> modeChoice, songChoice, beatsChoice;
    std::unique_ptr<LevelBar> songLevelBar;
    std::unique_ptr<NumberField> bpmField, clickField, offsetField;
    std::unique_ptr<Switch> countInSwitch, countInPlaySwitch;
    juce::TextButton *targetPlayButton = nullptr, *tapButton = nullptr, *suggestionButton = nullptr;
    juce::Rectangle<int> sectionTextArea, songLevelLabelArea, songLevelValueArea, statusLineArea, modeCaptionArea;
    std::unique_ptr<Switch> separateSwitch;
    juce::TextButton *targetButton = nullptr, *referenceButton = nullptr, *recordButton = nullptr, *matchButton = nullptr,
                     *cancelButton = nullptr, *applyButton = nullptr, *discardButton = nullptr, *closeButton = nullptr;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::String resultText, statusText;
    juce::StringArray resultLabels;           // "Amp", "Tone", "Cab", "Match EQ"
    std::vector<juce::StringArray> resultItems; // each line's items, packed at paint time
    juce::Rectangle<int> header, targetCard, referenceCard, matchCard, resultCard, progressArea, statusArea, resultTextArea, applyNoteArea;
    bool applied = false;
};

} // namespace ui
