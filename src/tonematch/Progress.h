// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace ampsim::tonematch
{
/// Tone match's progress reports: a fraction from 0 to 1 and what's happening, in words. Called on
/// whichever worker thread is busy, so a GUI must hop to the message thread itself.
using ProgressFn = std::function<void (double fraction, const juce::String& stage)>;
} // namespace ampsim::tonematch
