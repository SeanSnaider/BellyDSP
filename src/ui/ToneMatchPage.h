// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Pages.h"
#include "ToneMatchSession.h"

namespace ui
{

/// The tone match page (docs/TONE_MATCH.md), opened from the brand menu's "Match tone...". The UI handoff
/// didn't design it, so it's a quiet page in the handoff's tokens and components (ASSUMPTIONS TM): a header
/// with the honest note, three cards across (Target, Your DI, Match), and the result under them.
///
///   Target    Choose file... (or drop one on the page), the waveform with the range to match (drag to
///             select, drag an edge or the middle to adjust; 3 to 60 s), and, when the build has it,
///             "Separate the guitar first".
///   Your DI   Record (the clean DI, up to a minute, through the app's own input) or Choose file..., and the
///             mode: Same part (you played the same part) or Anything.
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

    /// For tests: what's shown.
    juce::String getResultText() const { return resultText; }
    juce::String getStatusText() const { return statusText; }
    juce::TextButton& getMatchButton() noexcept { return *matchButton; }
    juce::TextButton& getApplyButton() noexcept { return *applyButton; }
    juce::TextButton& getRecordButton() noexcept { return *recordButton; }
    Segmented& getModeChoice() noexcept { return *modeChoice; }

    /// The waveform: the target's peaks, with the selected range. Drag to select.
    class Waveform;
    /// The match EQ over the residual it was fitted to.
    class EqCurve;

private:
    void timerCallback() override;
    void targetChanged();
    void updateResultText();

    ToneMatchSession session;
    std::unique_ptr<Waveform> waveform;
    std::unique_ptr<EqCurve> eqCurve;
    std::unique_ptr<Segmented> modeChoice;
    std::unique_ptr<Switch> separateSwitch;
    juce::TextButton *targetButton = nullptr, *referenceButton = nullptr, *recordButton = nullptr, *matchButton = nullptr,
                     *cancelButton = nullptr, *applyButton = nullptr, *discardButton = nullptr, *closeButton = nullptr;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::String resultText, statusText;
    juce::Rectangle<int> header, targetCard, referenceCard, matchCard, resultCard, progressArea, resultTextArea;
    bool applied = false;
};

} // namespace ui
