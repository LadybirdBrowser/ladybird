/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// The timing the style engine computes the key an effect samples its keyframes at from: what its animation contributes,
// the effect's own timing, and its timeline's current time.
ComputedValuesFFI::FfiEffectTiming style_engine_effect_timing(Animations::KeyframeEffect const&, Animations::Animation const&);

}
