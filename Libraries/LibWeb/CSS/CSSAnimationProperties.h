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

struct TransitionProperties {
    Vector<PropertyID> properties;
    double duration;
    EasingFunction timing_function;
    double delay;
    TransitionBehavior transition_behavior;
};

// The timeline an animation definition asks for. A scroll timeline is a GC object, and a definition
// is built for every animation on every style recomputation while the timeline it names almost
// never changes, so the definition carries the description and the object is materialized only
// where one is actually needed.
struct AnimationTimelineSource {
    enum class Kind : u8 {
        Document,
        None,
        Scroll,
    };

    Kind kind { Kind::Document };
    Scroller scroller {};
    Axis axis {};

    bool operator==(AnimationTimelineSource const&) const = default;
};

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
    AnimationTimelineSource timeline;
};

}
