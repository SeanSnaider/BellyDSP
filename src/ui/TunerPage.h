#pragma once

#include "Pages.h"

namespace ui
{

/// The tuner page (handoff 4.8), opened from the top bar's Tuner (it engages the tuner; leaving it
/// disengages it). A centred column: the note at 180 px (light) with its sharp and octave beside it; the
/// frequency (2 decimals), the cents (a signed integer), and the state (Flat, Sharp, In tune); the 720 px
/// cents scale, -50 to +50, with its needle; the six strings of the chosen tuning; and the tunings, A4
/// with - and +, and Mute output. Within 3 cents the note, the needle, the state, and a soft zone behind 0
/// turn emerald.
///
/// It reads the processor's tuner (its own thread analyses the DI) 30 times a second while showing. The
/// current string is the one nearest the note heard, unless one has been clicked: then the note and the
/// cents are measured against that string, so a string far out (or dropped) can be brought to it; a second
/// click on it lets go (ASSUMPTIONS H9). Strings that have been in tune this session turn emerald. The
/// tuning is the app's view state, saved with it, never a parameter.
class TunerPage final : public ControlGroup, private juce::Timer
{
public:
    explicit TunerPage (AmpSimProcessor& processor);
    ~TunerPage() override;

    struct Tuning
    {
        const char* name;
        std::array<int, 6> midi; // low string (6) to high (1)
    };
    static const std::array<Tuning, 4>& tunings();

    void setTuning (int index);
    int getTuning() const noexcept { return tuning; }
    /// Targets a string (0 = string 6), or lets go of it when it's targeted already.
    void clickString (int index);
    int getTarget() const noexcept { return target; }

    /// Reads the tuner now (what the page's timer does 30 times a second).
    void poll() { timerCallback(); }

    /// For tests and snapshots: draw this reading from now on, instead of the live one.
    void freeze (const ampsim::TunerReading& r);

    /// What's shown, for tests.
    struct Shown
    {
        juce::String note, accidental, octave, hz, cents, state;
        double centsValue = 0.0;
        bool inTune = false;
        int currentString = 0;
    };
    Shown getShown() const { return shown; }
    bool isStringDone (int index) const { return done[(size_t) index]; }

    void refresh() override;
    void pageShown() override;
    void pageHidden() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

    static constexpr float inTuneCents = 3.0f;

private:
    void timerCallback() override;
    void update (const ampsim::TunerReading& r);
    juce::Rectangle<float> stringCircle (int index) const;

    ampsim::TunerReading reading;
    bool frozen = false;
    int tuning = 0, target = -1, lastString = 0;
    std::array<bool, 6> done {};
    Shown shown;

    std::unique_ptr<Segmented> tuningChoice;
    juce::TextButton *a4Down = nullptr, *a4Up = nullptr;
    Switch* mute = nullptr;
    int columnTop = 0;
    juce::Rectangle<int> noteArea, readArea, scaleArea, stringsArea, controlsArea, a4Area;
};

} // namespace ui
