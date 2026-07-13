/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

struct AnimationProperties {
    Variant<double, Utf16String> duration;
    EasingFunction timing_function;
    double iteration_count;
    AnimationDirection direction;
    AnimationPlayState play_state;
    double delay;
    AnimationFillMode fill_mode;
    AnimationComposition composition;
    Utf16FlyString name;
    GC::Ptr<Animations::AnimationTimeline> timeline;
};

}
