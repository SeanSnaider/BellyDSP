#pragma once

#include "Pages.h"

namespace ui
{

/// The Cab page (handoff 4.7; ASSUMPTIONS H8), three columns, 220 / the rest / 240, 40 px apart:
///
///   Cabinet       the cab packs and IR files in the library folder (presets' "irs" root), each a name
///                 over a line about it, a 1 px left border that turns emerald on the one in close mic 1.
///                 A click loads it into close mic 1, assigns it to the playing amp slot, and turns
///                 Follow off. Then "Follow amp choice", and the dashed drop zone (drop a .wav or a pack
///                 folder; its emerald Browse opens the file chooser).
///   Speaker       the line-drawn speaker at 380 px with mic A (close mic 1, emerald) and mic B (close
///                 mic 2, ink). A marker's distance from the centre over the speaker's radius is the
///                 mic's position across the cone (pos_x: 0 to 0.95 of the radius maps to 0 to 1); its
///                 angle is only for looks and isn't saved. Under it, each mic's zone. A mic with a plain
///                 IR can't move (only packs have positions): its marker is dimmed.
///   Microphones   per mic: its file (a click loads another), Distance (pos_y) and Level knobs, Flip
///                 phase, Mute, and a quiet row of pan, delay, and the file's channel. Then the cuts'
///                 knobs with their switches and slopes, then the room mic and auto-align.
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

    /// The library's entries, as listed (rescanned each time the page shows).
    struct Entry
    {
        juce::File file;
        juce::String name, description;
        bool pack = false;
    };
    const std::vector<Entry>& getEntries() const noexcept { return entries; }
    void rescan();

    /// For tests: click a library entry; a mic marker's centre (in this page's coordinates); drag a mic.
    void clickEntry (int index);
    juce::Point<float> markerPosition (int mic) const;
    void dragMarker (int mic, juce::Point<float> from, juce::Point<float> to);
    juce::String getReadout() const;
    bool isMarkerDimmed (int mic) const;

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

    std::vector<Entry> entries;
    juce::Rectangle<int> leftColumn, centreColumn, rightColumn, roomHeading;
    std::array<int, 2> micTops {}, micBottoms {};
    juce::String readout;
};

} // namespace ui
