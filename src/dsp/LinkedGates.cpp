// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "LinkedGates.h"

namespace ampsim
{

void GateB::process (juce::dsp::AudioBlock<float> block, const BlockContext& context)
{
    // Gate A runs every buffer (the chain keeps it running while it's off), so its curve is this
    // buffer's. The length check only guards against a caller that skipped it.
    if (linked && leader.getGainCurveLength() == (int) block.getNumSamples())
        gate.processWithGain (block, leader.getGainCurve());
    else
        gate.process (block, context);
}

} // namespace ampsim
