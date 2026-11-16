/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/RecordHandles.h>

namespace Web::CSS::ComputedValuesFFI {

struct FfiComputedAnimation;

}

namespace Web::CSS {

// https://www.w3.org/TR/css-animations-2/#cssanimation
// What the style engine does with the records of CSS animations, whose objects Bindings/Rust/src/css implements.
class CSSAnimation {
public:
    // A new AnimationEvent for `animation`, a CSS animation, that bubbles, which is implemented in Rust.
    static GC::Ref<DOM::Event> create_event(Animations::AnimationRef const& animation, DOM::Document const&, Utf16FlyString const& type, double elapsed_time, Utf16String pseudo_element);

    // https://drafts.csswg.org/css-animations-1/#animations
    // Creates the CSS animation `owner` runs for the definition a style computation computed, `definition`, at
    // position `name_index` of its animation-name, running `keyframe_set`.
    static Animations::AnimationRef create(DOM::AbstractElement owner, ComputedValuesFFI::FfiComputedAnimation const& definition, Animations::KeyframeSetHandle keyframe_set, size_t name_index);

    static Utf16FlyString animation_name(Animations::AnimationRef const&);

    // Applies the definition a style computation computed, `definition`, to the CSS animation `animation`, whose
    // keyframes the caller gives its effect.
    static void apply_css_properties(Animations::AnimationRef const& animation, ComputedValuesFFI::FfiComputedAnimation const& definition, DOM::AbstractElement timeline_target);

    static void set_animation_name_index(Animations::AnimationRef const&, size_t index);

    // Looks the named timeline the CSS animation follows up again from its owning element, answering whether that
    // changed its timeline.
    static bool update_named_timeline(Animations::AnimationRef const&);

private:
    using Flag = Animations::RustFFI::CssAnimationFlag;
    static bool has_flag(Animations::AnimationRef const&, Flag);
};

}
