// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "SampleRateGuard.h"

// Builds that aren't the standalone app (the tests, the console tools): there's no device to keep at
// 48 kHz, so nothing happens (SampleRateGuard.h).
namespace platform::samplerate
{
void start() {}
void stop() {}
} // namespace platform::samplerate
