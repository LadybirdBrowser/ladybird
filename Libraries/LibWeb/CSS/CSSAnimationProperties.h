/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

struct AnimationProperties {
    Variant<double, String> duration;
    EasingFunction timing_function;
    double iteration_count;
    AnimationDirection direction;
    AnimationPlayState play_state;
    double delay;
    AnimationFillMode fill_mode;
    AnimationComposition composition;
    FlyString name;
    GC::Ptr<Animations::AnimationTimeline> timeline;
};

}
