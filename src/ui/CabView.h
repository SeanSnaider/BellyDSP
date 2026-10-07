// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Pages.h"

namespace ui
{

/// The Cab page (handoff 4.7; ASSUMPTIONS UH8), three columns, 220 / the rest / 240, 40 px apart:
///
///   Cabinet       first the IRs bundled with the app (platform::factoryContentFolder()/irs, ASSUMPTIONS
///                 DS25), under a 12 px faint heading per cab folder ("Built in, Modern 4x12"): each IR
///                 its own entry, never a pack. Then "Your library": the cab packs and IR files in the
///                 library folder (presets' "irs" root). Each entry is a name over a line about it, a
///                 1 px left border that turns emerald on the one in close mic 1, and the list scrolls
///                 past six entries' height. A click loads it into close mic 1, assigns it to the
///                 playing amp slot, and turns Follow off. Then "Follow amp choice", and the dashed drop zone (drop a .wav or a pack
///                 folder; its emerald Browse opens the file chooser).
///   Speaker       the line-drawn speaker at 380 px with mic A (close mic 1, emerald) and mic B (close
///                 mic 2, ink). A marker's distance from the centre over the speaker's radius is the
///                 mic's position across the cone (pos_x: 0 to 0.95 of the radius maps to 0 to 1); its
///                 angle is only for looks and isn't saved. Under it, each mic's zone. A mic with a plain
///                 IR can't move (only packs have positions): its marker is dimmed.
///   Microphones   per mic: its file (a click loads another), Distance (pos_y) and Level knobs, Flip
///                 phase, Mute, and a quiet row of pan, delay, and the file's channel. Then the cuts'
///                 knobs with their switches and slopes, then the room mic and auto-align.
///
/// Under the speaker, at the foot of the centre column, the match curve (dsp/MatchCurve.h; not in the
/// handoff, ASSUMPTIONS MC8): its heading, Amount, and switch on one line, over a read-only view of the
/// curve as it plays (the target at the current amount, +-15 dB, 20 Hz to 20 kHz), drawn like the EQ
/// page's response: an emerald line over a soft emerald fill to 0 dB.
class CabView final : public ControlGroup, public juce::FileDragAndDropTarget
{
public:
    explicit CabView (AmpSimProcessor& processor);
    ~CabView() override;

    /// Browse (the drop zone's link): choose an IR file (pack = false) or a cab pack folder, for close mic
    /// 1 as a pick. The editor opens the chooser and calls pick().
    std::function<void (bool pack)> onBrowse;
    /// A mic's file link: the load menu for that mic (0, 1, or the room).
    std::function<void (int mic, juce::Component& near)> onMicMenu;

    /// A cab picked here (the list, a drop, Browse): into close mic 1, assigned to the playing slot,
    /// Follow off.
    void pick (const juce::File& fileOrPack);

    /// The list's entries, built-in ones first (rescanned each time the page shows).
    struct Entry
    {
        juce::File file;
        juce::String name, description;
        bool pack = false;
        bool builtIn = false;  ///< bundled with the app (content/irs), not from the user's library
        juce::String group;    ///< the heading it's listed under: "Built in, Modern 4x12", "Your library"
    };
    const std::vector<Entry>& getEntries() const noexcept { return entries; }
    void rescan();

    /// For tests: click a library entry; a mic marker's centre (in this page's coordinates); drag a mic.
    void clickEntry (int index);
    /// For tests: the group headings as listed, an entry's top in the list, and the list's viewport.
    juce::StringArray getGroupHeadings() const;
    int entryTop (int index) const;
    juce::Viewport& getListViewport() noexcept { return *listViewport; }
    int getAlignSwitchBottom() const; ///< the lowest control in the left column, which must stay on the page
    juce::Point<float> markerPosition (int mic) const;
    void dragMarker (int mic, juce::Point<float> from, juce::Point<float> to);
    juce::String getReadout() const;
    bool isMarkerDimmed (int mic) const;

    /// For tests: the match curve view's area (in this page's coordinates) and what it draws: the curve's
    /// dB at its points (empty when there's no curve), and whether it's drawn as on.
    juce::Rectangle<int> getMatchCurveViewBounds() const;
    std::vector<float> getMatchCurveDrawnDb() const;
    bool isMatchCurveDrawnOn() const;

    /// The zone a normalised radius (0 centre, 1 the speaker's edge) falls in (handoff 4.7).
    static juce::String zoneFor (float radius);

    void refresh() override;
    void pageShown() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    class LibraryList;
    class DropZone;
    class Speaker;
    class MicLink;
    class MatchCurveView;

    std::unique_ptr<LibraryList> list;
    std::unique_ptr<juce::Viewport> listViewport;
    Switch* follow = nullptr;
    std::unique_ptr<DropZone> dropZone;
    std::unique_ptr<Speaker> speaker;
    std::array<std::unique_ptr<MicLink>, AmpSimProcessor::numCabMics> links;

    struct MicControls
    {
        Knob *distance = nullptr, *level = nullptr;
        Switch *flip = nullptr, *mute = nullptr;
        ValueField *pan = nullptr, *delay = nullptr, *channel = nullptr;
        juce::Label *panLabel = nullptr, *delayLabel = nullptr, *channelLabel = nullptr;
    };
    std::array<MicControls, 2> mics;
    Knob *lowCut = nullptr, *highCut = nullptr;
    Switch *lowCutOn = nullptr, *highCutOn = nullptr, *roomMute = nullptr, *align = nullptr;
    ValueField *lowSlope = nullptr, *highSlope = nullptr, *roomLevel = nullptr, *roomPreDelay = nullptr;
    juce::Label *roomLabel = nullptr, *roomLevelLabel = nullptr, *roomPreDelayLabel = nullptr;
    std::unique_ptr<MatchCurveView> matchView;
    Switch* matchOn = nullptr;
    ValueField* matchAmount = nullptr;
    juce::Label* matchAmountLabel = nullptr;
    juce::Rectangle<int> matchHeading;

    std::vector<Entry> entries;
    // The list's layout: each entry's top, each group heading's top and text, the total height, and
    // where the "your library is empty" hint goes (-1 for none).
    std::vector<int> entryTops;
    std::vector<std::pair<int, juce::String>> headings;
    int listContentHeight = 0, libraryHintTop = -1;
    juce::String scrolledTo;
    void layoutList();
    void scrollSelectionIntoView();
    juce::Rectangle<int> leftColumn, centreColumn, rightColumn, roomHeading;
    std::array<int, 2> micTops {}, micBottoms {};
    juce::String readout;
};

} // namespace ui
