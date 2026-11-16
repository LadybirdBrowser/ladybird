/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/StyleEngineRustFFI.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

extern "C" {

// Defined in Bindings/Rust/src/css/exports.rs.
void* web_transition_event_create(JSVM*, void* realm, FlatPtr event_name, FlatPtr name, double elapsed_time, FlatPtr pseudo_element, Animations::RustFFI::AnimationCell const* animation);
}

GC::Ref<DOM::Event> CSSTransition::create_event(Animations::AnimationRef const& transition, DOM::Document const& document, Utf16FlyString const& type, double elapsed_time, Utf16String pseudo_element)
{
    auto& realm = HTML::relevant_realm(document);
    auto* event = web_transition_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.to_utf16_string().into_raw(), transition_property_name_and_id(transition).name().to_utf16_string().into_raw(), elapsed_time, move(pseudo_element).into_raw(), &transition.record());
    return GC::Ref { *static_cast<DOM::Event*>(static_cast<GC::Cell*>(event)) };
}

Animations::AnimationRef CSSTransition::start_a_provisional_transition(
    DOM::AbstractElement abstract_element,
    PropertyNameAndID property,
    size_t transition_generation,
    double delay,
    double start_time,
    double end_time,
    NonnullRefPtr<StyleValue const> start_value,
    NonnullRefPtr<StyleValue const> end_value,
    NonnullRefPtr<StyleValue const> reversing_adjusted_start_value,
    double reversing_shortening_factor,
    EasingFunction timing_function)
{
    Animations::RustFFI::FfiCssTransition description {
        .generation = transition_generation,
        .start_time = start_time + delay,
        .end_time = end_time + delay,
        .reversing_shortening_factor = reversing_shortening_factor,
        .custom_name = property.is_custom_property() ? property.name().raw_identity() : 0,
        .start_value = start_value->rust_style_value_data(),
        .end_value = end_value->rust_style_value_data(),
        .reversing_adjusted_start_value = reversing_adjusted_start_value->rust_style_value_data(),
        .property_id = to_underlying(property.id()),
    };
    // NB: Script gets the object of the transition, a CSSTransition, as it first asks for it.
    auto transition = Animations::AnimationRef::adopt(*Animations::RustFFI::animations_css_transition_create(&description));

    // FIXME:
    // Transitions generated using the markup defined in this specification are not added to the global animation list
    // when they are created. Instead, these animations are appended to the global animation list at the first moment
    // when they transition out of the idle play state after being disassociated from their owning element. Transitions
    // that have been disassociated from their owning element but are still idle do not have a defined composite order.

    // Construct a KeyframesEffect for our animation
    // NB: The current style computation collects this effect before publishing its result, so scheduling a second
    //     animated style update here would evaluate the same transition twice. The record keeps the effect the
    //     transition is started with.
    auto keyframe_effect_record = Animations::EffectRef::create_keyframe_effect();
    keyframe_effect_record.set_target(abstract_element, Animations::EffectRef::InvalidateEffect::No);
    keyframe_effect_record.set_keyframe_set(Animations::KeyframeSetHandle::adopt(Animations::RustFFI::animations_keyframe_set_for_transition(
        to_underlying(property.id()),
        property.is_custom_property() ? property.name().raw_identity() : 0,
        start_value->rust_style_value_data(),
        end_value->rust_style_value_data(),
        static_cast<float>(abstract_element.computed_style()->effective_zoom()))));
    Animations::set_owning_element(transition, abstract_element);
    auto* timeline = abstract_element.document().timeline().ptr();
    Animations::RustFFI::animations_animation_set_timeline(&transition.record(), timeline ? &timeline->record() : nullptr, Animations::perform_host_effect);

    // NB: The record sets the timing of the effect, sets it as the transition's associated effect and plays the
    //     transition, which creates its ready promise.
    Utf16FlyString timing_function_serialization { timing_function.to_utf16_string() };
    Animations::RustFFI::FfiProvisionalTransitionTiming timing {
        .delay = delay,
        .duration = end_time - start_time,
        .timing_function = timing_function.descriptor(),
        .timing_function_serialization = timing_function_serialization.raw_identity(),
    };
    HTML::TemporaryExecutionContext context(abstract_element.document().relevant_settings_object());
    Animations::RustFFI::animations_css_transition_start_provisionally(&transition.record(), &keyframe_effect_record.record(), &timing, Animations::perform_host_effect);
    return transition;
}

PropertyNameAndID CSSTransition::transition_property_name_and_id(Animations::AnimationRef const& transition)
{
    auto state = Animations::RustFFI::animations_css_transition(&transition.record());
    if (state.custom_name)
        return PropertyNameAndID::from_name(Utf16FlyString::from_raw(state.custom_name)).release_value();
    return PropertyNameAndID::from_id(static_cast<PropertyID>(state.property_id));
}

NonnullRefPtr<StyleValue const> CSSTransition::transition_start_value(Animations::AnimationRef const& transition)
{
    auto state = Animations::RustFFI::animations_css_transition(&transition.record());
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(state.start_value)));
}

void CSSTransition::commit_provisional_transition(Animations::AnimationRef const& transition, Animations::RustFFI::TransitionOrigin origin)
{
    // NB: The associated effect of a provisional transition is the effect it was started with.
    auto effect = transition.effect();
    VERIFY(effect.has_value());
    auto* target = effect->target();
    VERIFY(target);
    auto owner = Animations::RustFFI::animations_animation_owning_element(&transition.record());
    VERIFY(owner.element);
    Optional<PseudoElement> pseudo_element;
    if (owner.has_pseudo_element)
        pseudo_element = static_cast<PseudoElement>(owner.pseudo_element);
    target->commit_provisional_transition(pseudo_element, transition, origin);
}

void CSSTransition::discard_provisional_transition(Animations::AnimationRef const& transition)
{
    Animations::RustFFI::animations_animation_set_timeline(&transition.record(), nullptr, Animations::perform_host_effect);
    // NB: The record lets the transition have no associated effect.
    Animations::RustFFI::animations_css_transition_discard_provisional(&transition.record());
}

void CSSTransition::adopt_lane_transitions(DOM::Document& document)
{
    auto* arena = document.layout_node_arena_if_created();
    if (!arena)
        return;
    auto& style_computer = document.style_computer();
    auto adopt_value = [](StyleValueFFI::StyleValueData const* value) {
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value));
    };
    auto adopt = [&](u32 style_node, ReadonlySpan<u64> cancelled, ReadonlySpan<u64> seen, ReadonlySpan<StyleValueFFI::FfiLaneTransition> lane_transitions) {
        auto element = style_computer.element_for_lane_style_node(StyleNodeID { style_node });
        if (!element || !element->is_connected())
            return;
        // A transition the lane started that the host took in already runs from when the lane started it.
        auto runs_as_lane_does = [&](Animations::AnimationRef const& transition) {
            return Animations::RustFFI::animations_css_transition_runs_as_lane_does(&transition.record(), lane_transitions);
        };
        // NB: The record of each running transition decides what becomes of it (see LaneAdoption). One the steps saw
        //     end by itself ends as it does, with its events, and so does one script finished meanwhile.
        GC::RootVector<Animations::AnimationRef> ended;
        Vector<PropertyNameAndID> host_decides;
        for (auto const& transition : element->existing_transitions({})) {
            auto timing = transition.timing_state();
            if (timing.play_state == Animations::RustFFI::AnimationPlayState::Idle || timing.playback.is_finished)
                continue;
            switch (Animations::RustFFI::animations_css_transition_lane_adoption(&transition.record(), lane_transitions, cancelled, seen)) {
            case Animations::RustFFI::LaneAdoption::Stands:
                break;
            case Animations::RustFFI::LaneAdoption::Ends:
                ended.append(transition);
                break;
            case Animations::RustFFI::LaneAdoption::HostDecides:
                host_decides.append(transition_property_name_and_id(transition));
                break;
            }
        }
        for (auto const& transition : ended) {
            transition.cancel();
            element->remove_transition({}, transition_property_name_and_id(transition));
        }
        // NB: The start time of a transition is a time of the document timeline, which validating it as script sets
        //     it would only read as milliseconds.
        auto set_start_time = [](Animations::AnimationRef const& transition, double start_time) {
            Animations::RustFFI::animations_animation_set_start_time(&transition.record(), Animations::to_ffi(Optional<Animations::TimeValue> { Animations::TimeValue { Animations::TimeValue::Type::Milliseconds, start_time } }), Animations::perform_host_effect);
        };
        // A transition of the host's whose play is still pending runs from when the lane took it to start.
        for (auto const& lane_transition : lane_transitions) {
            if (lane_transition.host_identity == 0)
                continue;
            auto transition = element->property_transition({}, PropertyNameAndID::from_id(static_cast<PropertyID>(lane_transition.property_id)));
            if (!transition.has_value() || !Animations::RustFFI::animations_css_transition_is_host_transition_of(&transition->record(), &lane_transition))
                continue;
            auto timing = transition->timing_state();
            if (!timing.playback.start_time.has_value && (timing.playback.has_pending_play_task || timing.playback.has_pending_pause_task)
                && timing.play_state != Animations::RustFFI::AnimationPlayState::Paused)
                set_start_time(*transition, lane_transition.start_time);
        }
        for (auto const& lane_transition : lane_transitions) {
            if (lane_transition.host_identity != 0)
                continue;
            auto property = PropertyNameAndID::from_id(static_cast<PropertyID>(lane_transition.property_id));
            if (host_decides.contains_slow(property))
                continue;
            if (auto current = element->property_transition({}, property); current.has_value()) {
                if (runs_as_lane_does(*current))
                    continue;
                // A completed transition of the property gives way to the one the lane started. One the lane saw end,
                // which the host's timeline has yet to reach, ends now.
                auto timing = current->timing_state();
                if (timing.play_state != Animations::RustFFI::AnimationPlayState::Idle && !timing.playback.is_finished)
                    (void)Animations::RustFFI::animations_animation_finish(&current->record(), Animations::perform_host_effect);
                element->remove_transition({}, property);
            }
            auto transition = start_a_provisional_transition(*element, property, document.transition_generation(),
                lane_transition.delay, lane_transition.start_time, lane_transition.start_time + lane_transition.duration,
                adopt_value(lane_transition.start_value), adopt_value(lane_transition.end_value), adopt_value(lane_transition.reversing_adjusted_start_value),
                lane_transition.reversing_shortening_factor, EasingFunction::from_style_value(adopt_value(lane_transition.timing_function)));
            commit_provisional_transition(transition, Animations::RustFFI::TransitionOrigin::Lane);
            set_start_time(transition, lane_transition.start_time);
        }
    };
    // Starting and cancelling transitions settles their promises in the document's realm. The rendering update runs
    // their reactions once every document took in what its lanes did.
    HTML::TemporaryExecutionContext execution_context { document.relevant_settings_object() };
    StyleEngineFFI::style_engine_adopt_lane_transitions(
        arena->host(), &adopt,
        [](void* context, u32 style_node, u64 const* cancelled, size_t cancelled_count, u64 const* seen, size_t seen_count, StyleValueFFI::FfiLaneTransition const* transitions, size_t transition_count) {
            (*static_cast<decltype(adopt)*>(context))(style_node, { cancelled, cancelled_count }, { seen, seen_count }, { transitions, transition_count });
        });
}

}
