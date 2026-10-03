#include "Blocks.h"

#include <array>

namespace ui
{

namespace
{
// The order names are AmpSimProcessor::blockName()'s: permanent, saved in states and presets.
const std::array<BlockInfo, numBlocks> blocks { {
    { BlockId::input, "Input", "io", Section::inOut, "", false, "" },
    { BlockId::gateA, "Gate A", "gate_a", Section::pre, "gate_a_on", false, "gate" },
    { BlockId::preCompressor, "Compressor", "comp_pre", Section::pre, "comp_pre_on", false, "comp" },
    { BlockId::boost, "Boost", "boost", Section::pre, "boost_on", false, "boost" },
    { BlockId::overdrive, "Overdrive", "overdrive", Section::pre, "od_on", false, "overdrive" },
    { BlockId::preEq, "EQ", "eq_pre", Section::pre, "eq_pre_on", false, "eq" },
    { BlockId::amp, "Amp", "amp", Section::amp, "", false, "" },
    { BlockId::gateB, "Gate B", "gate_b", Section::amp, "gate_b_on", false, "" },
    { BlockId::cab, "Cab", "cab", Section::cab, "cab_bypass", true, "" },
    { BlockId::postEq, "EQ", "eq_post", Section::post, "eq_post_on", false, "eq" },
    { BlockId::postCompressor, "Compressor", "comp_post", Section::post, "comp_post_on", false, "comp" },
    { BlockId::harmonizer, "Harmonizer", "harmonizer", Section::post, "harm_on", false, "harmonizer" },
    { BlockId::multivoicer, "Multivoicer", "multivoicer", Section::post, "mv_on", false, "multivoicer" },
    { BlockId::bloom, "Bloom", "bloom", Section::post, "bloom_on", false, "bloom" },
    { BlockId::chorus, "Chorus", "chorus", Section::post, "chorus_on", false, "chorus" },
    { BlockId::delay, "Delay", "delay", Section::post, "delay_on", false, "delay" },
    { BlockId::reverb, "Reverb", "reverb", Section::post, "reverb_on", false, "reverb" },
    { BlockId::output, "Output", "io", Section::inOut, "", false, "" },
} };
} // namespace

const BlockInfo& info (BlockId id)
{
    return blocks[(size_t) juce::jlimit (0, numBlocks - 1, (int) id)];
}

juce::Colour sectionColour (Section section)
{
    switch (section)
    {
        case Section::pre:   return theme::sectionPre;
        case Section::amp:   return theme::sectionAmp;
        case Section::cab:   return theme::sectionCab;
        case Section::post:  return theme::sectionPost;
        case Section::inOut: break;
    }
    return theme::sectionInOut;
}

BlockId blockFor (Section section, const juce::String& orderName)
{
    for (const auto& b : blocks)
        if (b.section == section && orderName == b.orderName)
            return b.id;
    return BlockId::count;
}

} // namespace ui
