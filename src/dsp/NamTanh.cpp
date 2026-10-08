// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "NamTanh.h"

#include <NAM/activations.h>

#include <memory>

namespace ampsim::namtanh
{
namespace
{
/// tanh on a whole buffer at once with Eigen's packet math (NamTanh.h has the accuracy).
class VectorisedTanh final : public nam::activations::Activation
{
public:
    void apply (float* data, long size) override
    {
        Eigen::Map<Eigen::ArrayXf> values (data, size);
        values = values.tanh();
    }
};

/// NAM core keeps its activations in a protected static map; a class derived from Activation may write it. This is
/// the same map NAM's own enable_fast_tanh() and enable_lut() swap entries in, so nothing in NAM core changes.
struct Registry : nam::activations::Activation
{
    static Ptr get() { return _activations["Tanh"]; }
    static void set (Ptr tanh) { _activations["Tanh"] = std::move (tanh); }
};

struct State
{
    nam::activations::Activation::Ptr stock = Registry::get(); // NAM core's own, kept to switch back to
    nam::activations::Activation::Ptr vectorised = std::make_shared<VectorisedTanh>();
    Tanh current = Tanh::stock;
};

State& state()
{
    static State s; // built on first use, after NAM core's own statics (we're only ever called from code that runs later)
    return s;
}
} // namespace

void use (Tanh which)
{
    auto& s = state();
    Registry::set (which == Tanh::vectorised ? s.vectorised : s.stock);
    s.current = which;
}

void useAppTanh()
{
    static const bool installed = [] { use (Tanh::vectorised); return true; }();
    (void) installed;
}

Tanh current()
{
    return state().current;
}

} // namespace ampsim::namtanh
