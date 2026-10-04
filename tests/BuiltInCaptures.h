// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "PluginProcessor.h"

namespace testing
{
/// While it lives, processors start on their built-in captures as in the app (TestMain turns that off for
/// the rest of the suite, whose tests expect empty slots).
struct WithBuiltInCaptures
{
    WithBuiltInCaptures() { AmpSimProcessor::builtInCapturesForFreshSlots = true; }
    ~WithBuiltInCaptures() { AmpSimProcessor::builtInCapturesForFreshSlots = was; }
    bool was = AmpSimProcessor::builtInCapturesForFreshSlots;
};
} // namespace testing
