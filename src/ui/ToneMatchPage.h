// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Overlay.h"
#include "Pages.h"
#include "ToneMatchSession.h"
#include "tonematch/GuitarSeparator.h"

namespace ui
{

/// The tone match overlay (docs/TONE_MATCH.md), opened from the top bar's Match tone or the brand menu's "Match
/// tone...". Laid out as its real flow (the redesign, phase 3; ASSUMPTIONS RD36 to RD44): six numbered steps, read
/// left to right and top to bottom, each a panel with a badge that says where it stands (waiting, next, running,
/// done) and its main action as the panel's primary button.
///
///   1 The song    Choose file... (or drop one on the page), the waveform with the section to match (drag to
///                 select, drag an edge or the middle to adjust; 3 to 60 s), Play (the section, looped; Full song
///                 or Guitar only once a separated match made a stem), the song's level, "Count-in before Play",
///                 and, when the build has it, "Separate the guitar first" with what the download takes.
///   2 Your DI     Record: with a song loaded, a play-along take (the count-in, then the section once while your
///                 DI records, lined up with it; docs/TONE_MATCH.md, "Play along"); without one, the clean DI up to
///                 a minute. Or Choose file... What's recorded and how the take lined up, Save take (for the
///                 learning prototype), and the count-in (on, 4 or 2 beats, its tempo with Tap and the section's
///                 measured tempo, the click's level) with the latency offset.
///   3 Match       what you played (Same part or Anything), "Clean up with my take" (Auto, On, Off) with why it
///                 can't when it can't, Match and Cancel, the progress with its stage, and every error in full.
///   4 Result      the closeness, what it chose (the amp, Gain, the effects, the tone, the cab, the match EQ or
///                 curve), and the curve over what it was fitted to, with the target's and the match's spectra.
///   5 Compare     Target, Match, Current (and Raw after a cleanup), level-matched, on the loop, with the live
///                 mute and the level; Space and 1 to 4.
///   6 Apply       what Apply sets, in one undo step, then Apply and Discard.
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

    // ---- Saving a take (for prototypes/learn_tone.py; docs/TONE_MATCH.md) ---------------------------------
    /// Save take: the section, its stem if there is one, the take, and how it lined up, into a new folder in
    /// the takes folder (ToneMatchSession::saveTake). The quiet button next to the mode.
    void saveTake();
    juce::TextButton& getSaveTakeButton() noexcept { return *saveTakeButton; }
    /// Where Save take writes (ToneMatchSession::defaultTakesFolder unless a test points it elsewhere).
    void setTakesFolder (const juce::File& folder) { takesFolder = folder; }
    /// The folder the last Save take wrote (empty if none, or it failed).
    juce::File getLastSavedTake() const { return lastSavedTake; }

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

    // ---- Cleaning up the target with the take (docs/TONE_MATCH.md) -----------------------------------------
    /// "Clean up with my take", in the Target card: Auto (default), On, Off; disabled when the DI's notes don't line
    /// up with the target's (the caption says why).
    Segmented& getCleanupChoice() noexcept { return *cleanupChoice; }
    /// What the last match's cleanup did, as the Match card's status line says it (empty if it wasn't tried).
    juce::String getCleanupText() const;
    /// The cleanup's caption beside its switch: what it does, or why it can't now.
    juce::String getCleanupCaption() const;
    /// A pedal as the result card lists it ("Overdrive, Distortion;Drive 30;Tone 50;Level +0.0 dB": items split at ';').
    static juce::String pedalText (const ampsim::tonematch::Pedal& pedal);

    // ---- The steps (for tests: the page's organisation) ----------------------------------------------------
    static constexpr int numSteps = 6;
    /// Step 1 to 6's title ("The song", "Your DI", "Match", "Result", "Compare", "Apply").
    static juce::String stepTitle (int step);
    /// Where step 1 to 6 stands now: waiting, next, running, or done.
    overlay::Step getStepState (int step) const;
    /// Step 1 to 6's panel, in the page's coordinates.
    juce::Rectangle<int> getStepArea (int step) const;
    /// What step 1 to 6's heading says at its right ("0:12.0 to 0:42.0", "Recording", "Done in 3.1 s").
    juce::String getStepNote (int step) const;
    /// Every control on the page with the step it belongs to (for tests: each sits inside its step's panel).
    std::vector<std::pair<juce::Component*, int>> getStepControls() const;

private:
    void timerCallback() override;
    void targetChanged();
    void rebuildSourceChoice (bool withRaw);
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
    juce::TextButton *targetPlayButton = nullptr, *tapButton = nullptr, *suggestionButton = nullptr, *saveTakeButton = nullptr;
    juce::File takesFolder { ToneMatchSession::defaultTakesFolder() }, lastSavedTake;
    int savedTakeNumber = -1; // the session's take number Save take last wrote
    juce::Rectangle<int> sectionTextArea, songLevelLabelArea, songLevelValueArea, statusLineArea, modeCaptionArea;
    std::unique_ptr<Switch> separateSwitch;
    std::unique_ptr<Segmented> cleanupChoice;
    juce::Rectangle<int> cleanupCaptionArea, cleanupLabelArea;
    bool sourceChoiceHasRaw = false;
    juce::TextButton *targetButton = nullptr, *referenceButton = nullptr, *recordButton = nullptr, *matchButton = nullptr,
                     *cancelButton = nullptr, *applyButton = nullptr, *discardButton = nullptr, *closeButton = nullptr;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::String resultText, statusText;
    juce::StringArray resultLabels;           // "Amp", "Tone", "Cab", "Match EQ"
    std::vector<juce::StringArray> resultItems; // each line's items, packed at paint time
    juce::Rectangle<int> header, targetCard, referenceCard, matchCard, resultCard, compareCard, applyCard, progressArea, statusArea, resultTextArea,
        applyNoteArea, fileNameArea, takeCaptionArea, hintArea, modeLabelArea, compareStatusArea, appliedArea, separateNoteArea;
    bool applied = false;
};

} // namespace ui
