/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/RecordHandles.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// What the style engine does with the records of CSS transitions, whose objects Bindings/Rust/src/css implements.
class CSSTransition {
public:
    // A new TransitionEvent for `transition`, a CSS transition, that bubbles, which is implemented in Rust.
    static GC::Ref<DOM::Event> create_event(Animations::AnimationRef const& transition, DOM::Document const&, Utf16FlyString const& type, double elapsed_time, Utf16String pseudo_element);

    // A transition is started provisionally, and associated with its target only once committed.
    static Animations::AnimationRef start_a_provisional_transition(
        DOM::AbstractElement,
        PropertyNameAndID,
        size_t transition_generation,
        double delay,
        double start_time,
        double end_time,
        NonnullRefPtr<StyleValue const> start_value,
        NonnullRefPtr<StyleValue const> end_value,
        NonnullRefPtr<StyleValue const> reversing_adjusted_start_value,
        double reversing_shortening_factor,
        EasingFunction timing_function);

    static void commit_provisional_transition(Animations::AnimationRef const&, Animations::RustFFI::TransitionOrigin = Animations::RustFFI::TransitionOrigin::StyleChange);
    static void discard_provisional_transition(Animations::AnimationRef const&);

    // AD-HOC: Runs the transitions the hover of the document's clock lane left elements running beside a task, which
    //         the screen showed, in place of those its steps ended, as the rendering update that takes them in begins.
    static void adopt_lane_transitions(DOM::Document&);

    static PropertyNameAndID transition_property_name_and_id(Animations::AnimationRef const&);

    static NonnullRefPtr<StyleValue const> transition_start_value(Animations::AnimationRef const&);
};

}
