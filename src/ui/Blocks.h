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

} // namespace ui
