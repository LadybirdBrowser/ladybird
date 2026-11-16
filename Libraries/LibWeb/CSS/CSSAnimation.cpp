/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/NamedTimelines.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

namespace Web::CSS {

extern "C" {

// Defined in Bindings/Rust/src/css/exports.rs.
void* web_animation_event_create(JSVM*, void* realm, FlatPtr event_name, FlatPtr name, double elapsed_time, FlatPtr pseudo_element, Animations::RustFFI::AnimationCell const* animation);
}

GC::Ref<DOM::Event> CSSAnimation::create_event(Animations::AnimationRef const& animation, DOM::Document const& document, Utf16FlyString const& type, double elapsed_time, Utf16String pseudo_element)
{
    auto& realm = HTML::relevant_realm(document);
    auto* event = web_animation_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.to_utf16_string().into_raw(), animation_name(animation).to_utf16_string().into_raw(), elapsed_time, move(pseudo_element).into_raw(), &animation.record());
    return GC::Ref { *static_cast<DOM::Event*>(static_cast<GC::Cell*>(event)) };
}

Animations::AnimationRef CSSAnimation::create(DOM::AbstractElement owner, ComputedValuesFFI::FfiComputedAnimation const& definition, Animations::KeyframeSetHandle keyframe_set, size_t name_index)
{
    // An animation applies to an element if its name appears as one of the identifiers in the computed value of the
    // animation-name property and the animation uses a valid @keyframes rule
    // FIXME:
    // CSS Animations generated using the markup defined in this specification are not added to the global animation
    // list when they are created. Instead, these animations are appended to the global animation list at the first
    // moment when they transition out of the idle play state after being disassociated from their owning element. CSS
    // Animations that have been disassociated from their owning element but are still idle do not have a defined
    // composite order.
    // NB: Script gets the object of the animation, a CSSAnimation, as it first asks for it.
    auto animation = Animations::AnimationRef::adopt(*Animations::RustFFI::animations_css_animation_create());
    Animations::RustFFI::animations_css_animation_set_name(&animation.record(), css_string_from_rust(definition.name).raw_identity());
    Animations::set_owning_element(animation, owner);

    auto effect = Animations::EffectRef::create_keyframe_effect();
    Animations::RustFFI::animations_animation_set_effect(&animation.record(), &effect.record(), true, Animations::perform_host_effect);

    apply_css_properties(animation, definition, owner);
    set_animation_name_index(animation, name_index);

    effect.set_keyframe_set(move(keyframe_set));

    effect.set_target(owner);
    return animation;
}

Utf16FlyString CSSAnimation::animation_name(Animations::AnimationRef const& animation)
{
    auto raw = Animations::RustFFI::animations_css_animation_state(&animation.record()).name;
    return raw ? Utf16FlyString::from_raw(raw) : Utf16FlyString {};
}

void CSSAnimation::set_animation_name_index(Animations::AnimationRef const& animation, size_t index)
{
    if (!Animations::RustFFI::animations_css_animation_set_name_index(&animation.record(), static_cast<u32>(index)))
        return;
    if (auto effect = animation.effect(); effect.has_value()) {
        if (auto* target = effect->target())
            target->invalidate_associated_animation_composite_order();
    }
}

using TimelineKind = ComputedValuesFFI::FfiAnimationTimelineKind;

static Animations::ScrollAxis requested_axis(ComputedValuesFFI::FfiComputedAnimation const& definition)
{
    return Animations::scroll_axis_from_css_axis(static_cast<Axis>(definition.scroll_axis));
}

static Animations::AnimationTimeline::AnonymousScrollSource anonymous_scroll_source(ComputedValuesFFI::FfiComputedAnimation const& definition, DOM::AbstractElement target)
{
    return Animations::AnimationTimeline::AnonymousScrollSource {
        .scroller = static_cast<Scroller>(definition.scroll_scroller),
        .target = target,
    };
}

static Utf16FlyString timeline_name(ComputedValuesFFI::FfiComputedAnimation const& definition)
{
    return definition.timeline_name ? css_string_from_rust(definition.timeline_name) : Utf16FlyString {};
}

// NB: Unrelated style changes shouldn't cause us to recreate anonymous timelines, to achieve this we drop updates
//     between two equivalent anonymous timelines. The comparison is made against the description of the requested
//     timeline, so that the timeline a definition names is only ever created when it will replace the one the
//     animation already has.
static bool should_update_timeline(GC::Ptr<Animations::AnimationTimeline> old_timeline, ComputedValuesFFI::FfiComputedAnimation const& definition, DOM::AbstractElement target)
{
    switch (definition.timeline_kind) {
    case TimelineKind::None:
        return old_timeline != nullptr;
    case TimelineKind::Document: {
        GC::Ptr<Animations::AnimationTimeline> const document_timeline = target.document().timeline();
        return old_timeline != document_timeline;
    }
    case TimelineKind::Named:
        return old_timeline != target.document().named_timelines().target_timeline(target, timeline_name(definition));
    case TimelineKind::View: {
        return !old_timeline
            || !old_timeline->is_view_timeline()
            || old_timeline->view_subject() != target
            || old_timeline->axis() != requested_axis(definition)
            || old_timeline->view_inset() != Animations::view_timeline_inset_from_ffi(definition.view_inset);
    }
    case TimelineKind::Scroll:
        break;
    }

    if (!old_timeline || !old_timeline->is_scroll_timeline() || old_timeline->is_view_timeline())
        return true;

    if (old_timeline->axis() != requested_axis(definition))
        return true;

    auto old_source = old_timeline->scroll_source();
    if (!old_source.has<Animations::AnimationTimeline::AnonymousScrollSource>())
        return true;

    return old_source.get<Animations::AnimationTimeline::AnonymousScrollSource>() != anonymous_scroll_source(definition, target);
}

static GC::Ptr<Animations::AnimationTimeline> materialize_timeline(ComputedValuesFFI::FfiComputedAnimation const& definition, DOM::AbstractElement target)
{
    switch (definition.timeline_kind) {
    case TimelineKind::Document:
        return target.document().timeline();
    case TimelineKind::None:
        return {};
    case TimelineKind::Scroll:
        return Animations::AnimationTimeline::create_scroll_timeline(target.document(), anonymous_scroll_source(definition, target), requested_axis(definition));
    case TimelineKind::View:
        return Animations::AnimationTimeline::create_view_timeline(target.document(), target, requested_axis(definition), Animations::view_timeline_inset_from_ffi(definition.view_inset));
    case TimelineKind::Named:
        return target.document().named_timelines().target_timeline(target, timeline_name(definition));
    }
    VERIFY_NOT_REACHED();
}

void CSSAnimation::apply_css_properties(Animations::AnimationRef const& animation, ComputedValuesFFI::FfiComputedAnimation const& definition, DOM::AbstractElement timeline_target)
{
    // FIXME: Don't apply overridden properties as defined here: https://drafts.csswg.org/css-animations-2/#animations

    VERIFY(animation.effect().has_value());

    // NB: Recorded before the early return below, so that the definition always says what the style
    //     computation last computed.
    // NB: An animation that names its timeline looks it up again from its owning element as the timelines its
    //     document declares change, also while the name finds no timeline. The record notes the name it follows.
    auto const follows_named_timeline = definition.timeline_kind == TimelineKind::Named;
    auto const applies_the_same_properties = Animations::RustFFI::animations_css_animation_apply_definition(&animation.record(), &definition, follows_named_timeline ? timeline_name(definition).raw_identity() : 0);

    GC::Ptr<Animations::AnimationTimeline> timeline = static_cast<Animations::AnimationTimeline*>(Animations::RustFFI::animations_animation_timeline(&animation.record()));
    auto const update_timeline = !has_flag(animation, Flag::ScriptSetTimeline)
        && should_update_timeline(timeline, definition, timeline_target);
    if (follows_named_timeline)
        timeline_target.document().named_timelines().track_animation(animation);

    // NB: Style feedback can recompute an animation's owning element. Reapplying unchanged
    //     properties would invalidate the effect and turn that feedback into an endless cycle.
    if (!update_timeline && applies_the_same_properties)
        return;

    HTML::TemporaryExecutionContext context(HTML::relevant_realm(Animations::relevant_global_object_of(animation)));
    if (update_timeline) {
        auto new_timeline = materialize_timeline(definition, timeline_target);
        Animations::RustFFI::animations_animation_set_timeline(&animation.record(), new_timeline ? &new_timeline->record() : nullptr, Animations::perform_host_effect);
    }
    Animations::RustFFI::animations_css_animation_apply_properties(&animation.record(), &definition, Animations::perform_host_effect);
}

bool CSSAnimation::has_flag(Animations::AnimationRef const& animation, Flag flag)
{
    return Animations::RustFFI::animations_css_animation_has_flag(&animation.record(), flag);
}

bool CSSAnimation::update_named_timeline(Animations::AnimationRef const& animation)
{
    auto state = Animations::RustFFI::animations_css_animation_state(&animation.record());
    if (!state.named_timeline || has_flag(animation, Flag::ScriptSetTimeline))
        return false;
    // NB: The owning element of a CSS animation is the element whose style names the timeline, which the animation
    //     keeps alive.
    auto owner = Animations::RustFFI::animations_animation_owning_element(&animation.record());
    VERIFY(owner.element);
    DOM::AbstractElement target { *static_cast<DOM::Element*>(owner.element), owner.has_pseudo_element ? static_cast<PseudoElement>(owner.pseudo_element) : Optional<PseudoElement> {} };
    GC::Ptr<Animations::AnimationTimeline> timeline = target.document().named_timelines().target_timeline(target, Utf16FlyString::from_raw(state.named_timeline));
    if (timeline.ptr() == static_cast<Animations::AnimationTimeline*>(Animations::RustFFI::animations_animation_timeline(&animation.record())))
        return false;
    HTML::TemporaryExecutionContext context { HTML::relevant_realm(Animations::relevant_global_object_of(animation)) };
    Animations::RustFFI::animations_animation_set_timeline(&animation.record(), timeline ? &timeline->record() : nullptr, Animations::perform_host_effect);
    return true;
}

}
