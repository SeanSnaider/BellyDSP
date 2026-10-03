// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Theme.h"

namespace ui
{

/// Everything on the chain strip, in signal order (BUILD_PLAN "Default signal chain"): the input, the
/// pre section, the amp, Gate B, the cab, the post section, and the output. Each has a page.
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
    const char* name;        // on the chain block and the page's title
    const char* pageName;    // the page's snapshot name: editor_<pageName>.png
    Section section;
    const char* onParameter; // the block's switch ("" for none)
    bool switchInverted;     // the cab's switch is cab_bypass: on when the parameter is off
    const char* orderName;   // its permanent name in a section's saved order ("" for a fixed block)
};

const BlockInfo& info (BlockId id);
juce::Colour sectionColour (Section section);

/// A reorderable block of `section` by its saved order name ("gate", "comp", ...), or count if none.
BlockId blockFor (Section section, const juce::String& orderName);

constexpr int numBlocks = (int) BlockId::count;

/// The main area's pages (handoff 2 and 4.4): one per node of the signal chain along the bottom, and the
/// tuner, opened from the top bar. The effect blocks live on the Pre FX and Post FX pages as tabs; the
/// EQ node's page shows the post EQ (with a switch to the pre EQ).
enum class PageId
{
    input,
    preFx,
    amp,
    eq,
    cab,
    postFx,
    output,
    tuner,
    count
};

constexpr int numPages = (int) PageId::count;

/// A page's permanent name (its snapshot name, and the app state's "page last shown").
const char* pageName (PageId page);

/// The page a block's editor is on (Gate B is on the Pre FX page's gate tab, with Gate A).
PageId pageFor (BlockId block);

} // namespace ui
