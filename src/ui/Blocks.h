// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Theme.h"

namespace ui
{

/// Every block of the signal chain, in signal order (BUILD_PLAN "Default signal chain"): the input, the pre
/// section, the amp, Gate B, the cab, the post section, and the output. On the main screen the pre and post
/// blocks and Gate B are pedals on the board, the amp and the cab are markers between them, and the selected
/// block's controls are in the inspector; the input and the output are in Settings.
enum class BlockId
{
    input,
    gateA,
    preCompressor,
    boost,
    overdrive,
    preEq,
    amp,
    gateB,
    cab,
    postEq,
    postCompressor,
    harmonizer,
    multivoicer,
    bloom,
    chorus,
    delay,
    reverb,
    output,
    count
};

enum class Section
{
    inOut,
    pre,
    amp, // the amp and Gate B, fixed between the sections
    cab,
    post
};

struct BlockInfo
{
    BlockId id;
    const char* name;        // on the pedal, and the inspector's title
    const char* pageName;    // its permanent short name: snapshots (editor_<pageName>.png) and the selection in the app state
    Section section;
    const char* onParameter; // the block's switch ("" for none)
    bool switchInverted;     // the cab's switch is cab_bypass: on when the parameter is off
    const char* orderName;   // its permanent name in a section's saved order ("" for a fixed block)
};

const BlockInfo& info (BlockId id);
juce::Colour sectionColour (Section section);

/// A reorderable block of `section` by its saved order name ("gate", "comp", ...), or count if none.
BlockId blockFor (Section section, const juce::String& orderName);

/// A block by its permanent short name (BlockInfo::pageName; "io" finds the input), or count if none.
BlockId blockNamed (const juce::String& pageName);

constexpr int numBlocks = (int) BlockId::count;

/// What the stage shows (its segmented control, top right): the playing amp, the cab, or an EQ's graph.
enum class StageMode
{
    amp,
    cab,
    eq,
    count
};

constexpr int numStageModes = (int) StageMode::count;

/// A stage mode's permanent name (the app state's "stage last shown", and snapshots), and its label.
const char* stageModeName (StageMode mode);
const char* stageModeLabel (StageMode mode);

/// What opens over the main screen and closes back to it: the tuner, the Match tone page, and Settings (the
/// Input and Output pages: the levels, the limiter, the CPU saver, A/B, the tempo, MIDI).
enum class Overlay
{
    none,
    tuner,
    toneMatch,
    settings
};

/// An overlay's permanent name (snapshots).
const char* overlayName (Overlay overlay);

} // namespace ui
