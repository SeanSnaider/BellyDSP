// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "DeviceLatency.h"

// Builds that aren't the standalone app (the tests, the console tools): no device of our own.
namespace platform::device
{
Latency reportedLatency() { return {}; }
} // namespace platform::device
