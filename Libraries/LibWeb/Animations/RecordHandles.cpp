/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/RecordHandles.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/Zoom.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/Slot.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/CompositorAnimationEffectState.h>
#include <LibWeb/Painting/Scrolling.h>
#include <LibWeb/SVG/SVGAnimationElement.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::Animations {

void visit_host_cell(void* visitor, void* cell, RustFFI::HostCellKind kind)
{
    auto& cell_visitor = *static_cast<GC::Cell::Visitor*>(visitor);
    switch (kind) {
    case RustFFI::HostCellKind::Cell:
        cell_visitor.visit(static_cast<GC::Cell*>(cell));
        return;
    case RustFFI::HostCellKind::Element:
        cell_visitor.visit(static_cast<DOM::Element*>(cell));
        return;
    }
    VERIFY_NOT_REACHED();
}

extern "C" {

// Declared in Bindings/Rust/src/css/animation_event.rs.
void web_animation_visit_host_cells(RustFFI::AnimationCell const*, void* visitor);
}

void web_animation_visit_host_cells(RustFFI::AnimationCell const* animation, void* visitor)
{
    RustFFI::animations_animation_visit_host_cells(animation, visitor, visit_host_cell);
}

void keep_animation_alive_in(void* root_vector, RustFFI::AnimationCell const* animation)
{
    static_cast<GC::RootVector<AnimationRef>*>(root_vector)->append(AnimationRef { *animation });
}

i32 AnimationRef::composite_order(AnimationRef const& a, AnimationRef const& b)
{
    return RustFFI::animations_composite_order(&a.record(), &b.record(), compare_elements_in_tree_order_for_composite_order);
}

Optional<CSS::PseudoElement> EffectRef::pseudo_element_type() const
{
    auto pseudo_element = RustFFI::animations_effect_target(&record()).pseudo_element;
    if (!pseudo_element.serialization)
        return {};
    return static_cast<CSS::PseudoElement>(pseudo_element.kind);
}

Optional<DOM::AbstractElement> EffectRef::target_abstract_element() const
{
    if (auto* target = this->target())
        return DOM::AbstractElement { *target, pseudo_element_type() };
    return {};
}

CSS::EasingFunction::Descriptor EffectRef::timing_function_with_points_in(Vector<CSS::EasingFunction::LinearPoint>& storage) const
{
    auto descriptor = RustFFI::animations_effect_timing_function(&record()).descriptor;
    storage.append(descriptor.linear_points, descriptor.linear_point_count);
    descriptor.linear_points = storage.data();
    descriptor.linear_point_count = storage.size();
    return descriptor;
}

RustFFI::FfiResolvedEffectTiming EffectRef::resolve_timing() const
{
    auto animation = associated_animation();
    auto local_time = animation.has_value() ? animation->timing_state().current_time : to_ffi(Optional<TimeValue> {});
    return RustFFI::animations_effect_resolve_timing(&record(), local_time, timing().fill_mode, animation.has_value() ? &animation->record() : nullptr, false);
}

Painting::CompositorAnimationEffectState EffectRef::compositor_animation_state() const
{
    return Painting::CompositorAnimationEffectState { RustFFI::animations_effect_compositor_animation_state(&record(), RustFFI::CompositorAnimationStateAccess::CreateIfMissing) };
}

Optional<Painting::CompositorAnimationEffectState> EffectRef::existing_compositor_animation_state() const
{
    if (auto* state = RustFFI::animations_effect_compositor_animation_state(&record(), RustFFI::CompositorAnimationStateAccess::Existing))
        return Painting::CompositorAnimationEffectState { state };
    return {};
}

bool EffectRef::has_retained_compositor_animations() const
{
    auto state = existing_compositor_animation_state();
    return state.has_value() && state->has_retained();
}

// Whether nothing shows the target's style, as a display: none ancestor leaves the target without a box.
// NB: SVG resources keep their style under display: none, where boxes elsewhere in the document can reference them.
static bool target_is_unseen_in_display_none_subtree(DOM::Element const& target)
{
    if (target.has_layout_box() || !target.has_inclusive_ancestor_with_display_none_ignoring_animations())
        return false;
    bool has_referenceable_svg_element = false;
    target.for_each_in_inclusive_subtree_of_type<SVG::SVGElement>([&](SVG::SVGElement const& element) {
        if (!element.is_referenceable_in_display_none_subtree())
            return TraversalDecision::Continue;
        has_referenceable_svg_element = true;
        return TraversalDecision::Break;
    });
    return !has_referenceable_svg_element;
}

// NB: A custom property can feed the display of the target through var().
bool EffectRef::may_animate_display() const
{
    return any_of(target_properties(), [](auto const& property) {
        return property.id() == CSS::PropertyID::Display || property.is_custom_property();
    });
}

bool EffectRef::can_skip_per_frame_style_update() const
{
    if (is_compositor_driven() || is_compositor_replaced())
        return true;
    if (is_offscreen_throttled() && all_of(target_properties(), [](auto const& property) {
            return !first_is_one_of(property.id(), CSS::PropertyID::Transform, CSS::PropertyID::Translate, CSS::PropertyID::Rotate, CSS::PropertyID::Scale);
        }))
        return true;

    auto* target = this->target();
    if (!target)
        return false;

    // An animation inside a box-less iframe can't affect its embedding document's rendering. Check the containers
    // before the target-local cache, since their layout can change without changing the target's style or layout box.
    // Reverse playback and effects with a finite end time need ticks to finish, even with an infinite iteration count.
    auto const timing = this->timing();
    if (auto animation = associated_animation(); animation.has_value()) {
        auto const state = animation->timing_state();
        if (state.playback.playback_rate > 0 && isinf(timing.iteration_count) && isinf(state.end_time.value)) {
            for (auto navigable = target->document().navigable(); navigable;) {
                auto container = navigable->container();
                if (!container)
                    break;
                if (container->document().layout_is_up_to_date() && !container->has_layout_box())
                    return true;
                navigable = container->document().navigable();
            }
        }
    }

    Layout::ForcedReadScope read { target->document() };
    auto skip_key = [&] {
        return RustFFI::PerFrameStyleUpdateSkipKey {
            .target_style_generation = target->animation_style_generation(),
            .target_subtree_style_generation = target->animation_subtree_style_generation(),
            .layout_node = bit_cast<FlatPtr>(target->unsafe_layout_node(read)),
            .target_is_connected = target->is_connected(),
        };
    };
    auto cache_result = [&](bool result) {
        if (target->document().layout_is_up_to_date()) {
            auto key = skip_key();
            RustFFI::animations_effect_note_per_frame_style_update_skip_decision(&record(), &key, result);
        }
        return result;
    };
    if (target->document().layout_is_up_to_date()) {
        auto key = skip_key();
        switch (RustFFI::animations_effect_per_frame_style_update_skip_decision(&record(), &key)) {
        case RustFFI::PerFrameStyleUpdateSkipDecision::CanSkip:
            HTML::note_rendering_trace_event("animation style skip cache hit"sv);
            return true;
        case RustFFI::PerFrameStyleUpdateSkipDecision::CannotSkip:
            HTML::note_rendering_trace_event("animation style skip cache hit"sv);
            return false;
        case RustFFI::PerFrameStyleUpdateSkipDecision::Stale:
            HTML::note_rendering_trace_event("animation style skip cache miss"sv);
            break;
        case RustFFI::PerFrameStyleUpdateSkipDecision::Unknown:
            break;
        }
    }

    // Restrict throttling within a rendered document to infinite transform-only effects until animation overflow
    // updates can also be throttled safely.
    if (!isinf(timing.iteration_count) || pseudo_element_type().has_value())
        return cache_result(false);

    if (!target->is_connected())
        return cache_result(true);
    if (target->document().layout_is_up_to_date() && !may_animate_display() && target_is_unseen_in_display_none_subtree(*target))
        return cache_result(true);
    if (!keyframe_set())
        return false;
    if (target_properties().is_empty() || any_of(target_properties(), [](auto const& property) {
            return !first_is_one_of(property.id(),
                CSS::PropertyID::Transform,
                CSS::PropertyID::Translate,
                CSS::PropertyID::Rotate,
                CSS::PropertyID::Scale);
        }))
        return cache_result(false);

    // NB: Transforms cannot affect rendering without a layout box. This also covers SVG content inside
    //     display-none subtrees and closed details elements.
    if (target->document().layout_is_up_to_date() && !target->has_layout_box())
        return cache_result(true);
    if (target->namespace_uri() == Namespace::SVG)
        return cache_result(false);

    if (is_offscreen_throttled())
        return cache_result(true);

    if (!target->document().layout_is_up_to_date())
        return false;
    auto const* layout_node = target->unsafe_layout_node(read);
    if (!layout_node || layout_node->visibility() != CSS::Visibility::Hidden)
        return cache_result(false);

    bool has_visible_descendant = false;
    layout_node->for_each_in_inclusive_subtree_of_type<Layout::NodeWithStyle>([&](auto const& descendant) {
        if (descendant.visibility() != CSS::Visibility::Visible)
            return TraversalDecision::Continue;
        has_visible_descendant = true;
        return TraversalDecision::Break;
    });
    if (has_visible_descendant)
        return cache_result(false);

    return cache_result(true);
}

void EffectRef::set_composite(CSS::StyleValueFFI::FfiCompositeOperation composite) const
{
    RustFFI::animations_effect_set_composite(&record(), composite);
    invalidate();
}

void EffectRef::set_keyframe_set(KeyframeSetHandle keyframe_set) const
{
    if (this->keyframe_set() == keyframe_set)
        return;

    if (!RustFFI::animations_effect_set_keyframe_set(&record(), keyframe_set.ptr()))
        return;
    if (auto* target = this->target())
        CSS::record_attr_names_read_by_keyframes(*target, this->keyframe_set());
    invalidate();
}

void EffectRef::set_target(DOM::Element* target, InvalidateEffect invalidate) const
{
    // NB: The record associates the associated animation with the new target rather than the old.
    RustFFI::animations_effect_set_target_element(&record(), target, perform_host_effect);
    if (invalidate == InvalidateEffect::No)
        return;

    if (target)
        CSS::record_attr_names_read_by_keyframes(*target, keyframe_set());
    this->invalidate();
    // FIXME: We don't remove the animated style from the old target element as part of normal animated style update and
    //        it will remain "stuck" until it's style is fully invalidated for some other reason.
}

void EffectRef::set_target(DOM::AbstractElement abstract_element, InvalidateEffect invalidate) const
{
    if (invalidate == InvalidateEffect::Yes) {
        set_target(&abstract_element.element());
    } else {
        VERIFY(!associated_animation().has_value());
        set_target(&abstract_element.element(), InvalidateEffect::No);
        CSS::record_attr_names_read_by_keyframes(abstract_element.element(), keyframe_set());
    }
    set_pseudo_element(abstract_element.pseudo_element());
}

void EffectRef::set_pseudo_element(Optional<CSS::PseudoElement> pseudo_element) const
{
    if (!pseudo_element.has_value()) {
        RustFFI::animations_effect_set_pseudo_element(&record(), { .kind = 0, .serialization = 0 });
        return;
    }
    Utf16FlyString serialization { CSS::Selector::PseudoElementSelector { *pseudo_element }.serialize() };
    RustFFI::animations_effect_set_pseudo_element(&record(), { .kind = to_underlying(*pseudo_element), .serialization = serialization.raw_identity() });
}

void EffectRef::invalidate() const
{
    RustFFI::animations_effect_forget_what_the_host_concluded(&record());
    if (auto* target = this->target())
        target->document().set_needs_animated_style_update(*this);
}

void EffectRef::request_observation_sample() const
{
    if (set_flag(RustFFI::EffectFlag::NeedsObservationSample, true))
        note_observation_sample_requested();
}

bool EffectRef::request_element_scoped_observation_sample(u64 task_generation) const
{
    switch (RustFFI::animations_effect_request_observation_sample_in_task(&record(), task_generation)) {
    case RustFFI::ObservationSampleRequest::AlreadyRequestedInTask:
        return false;
    case RustFFI::ObservationSampleRequest::Requested:
        return true;
    case RustFFI::ObservationSampleRequest::NewlyRequested:
        note_observation_sample_requested();
        return true;
    }
    VERIFY_NOT_REACHED();
}

void EffectRef::note_observation_sample_requested() const
{
    if (auto* target = this->target())
        target->document().set_needs_animated_style_update(*this);
}

bool EffectRef::can_skip_per_frame_animation_tick() const
{
    auto animation = associated_animation();
    auto const state = animation.has_value() ? animation->timing_state() : RustFFI::FfiAnimationTimingState {};
    auto const phase = animation.has_value() ? state.phase : resolve_timing().phase;
    auto const timing = this->timing();

    if (animation.has_value() && !state.playback.has_pending_play_task && !state.playback.has_pending_pause_task
        && state.play_state == RustFFI::AnimationPlayState::Running
        && state.playback.playback_rate > 0
        && state.has_monotonically_increasing_timeline
        && phase == RustFFI::AnimationEffectPhase::Before)
        return true;

    if (!is_compositor_driven() && !is_compositor_replaced() && !can_skip_per_frame_style_update())
        return false;

    // NB: Playing backward, an effect with an infinite iteration count finishes at time zero, and with a zero iteration
    //     duration it finishes at its end time. No timer wakes the main thread for either.
    if (animation.has_value() && isinf(timing.iteration_count) && (state.playback.playback_rate < 0 || !isinf(state.end_time.value)))
        return false;

    // A compositor-handled one-iteration effect has a document timer that wakes the main thread at its active end.
    if ((is_compositor_driven() || is_compositor_replaced()) && (!isinf(timing.iteration_count) || is_observation_relevant_compositor_animation()))
        return true;

    // NB: Infinite effects cannot reach their natural end, and finite offscreen paint effects have an end timer.
    //     Neither needs a continuous tick for animationend listeners.
    // NB: Starting or cancelling an active animation requests an update independently of playback.
    //     Only iteration events require future updates while a visually throttled effect runs.
    return !css_animation_events_are_heard(state, phase == RustFFI::AnimationEffectPhase::Active);
}

// Whether a listener on `target`, the nodes its events bubble to or its window hears an event `hears` names.
template<typename Hears>
static bool event_is_heard_at(DOM::Element& target, Hears const& hears)
{
    for (auto* node = static_cast<DOM::Node*>(&target); node;) {
        if (hears(*node))
            return true;
        if (auto assigned_slot = DOM::assigned_slot_for_node(*node))
            node = assigned_slot.ptr();
        else
            node = node->parent_or_shadow_host();
    }
    auto window = target.document().window();
    return window && hears(*window);
}

// Whether a listener hears the CSS animation events of the effect, whose associated animation's timing is `state`,
// that need a tick: only its iteration events if `only_iteration_events`.
bool EffectRef::css_animation_events_are_heard(RustFFI::FfiAnimationTimingState const& state, bool only_iteration_events) const
{
    auto animation = associated_animation();

    // NB: SMIL animations raise time events at their animation element instead, which do not bubble.
    if (animation.has_value() && state.kind == RustFFI::FfiAnimationKind::Smil) {
        auto& animation_element = *static_cast<SVG::SVGAnimationElement*>(RustFFI::animations_smil_animation_element(&animation->record()));
        if (only_iteration_events)
            return animation_element.has_event_listener(HTML::EventNames::repeatEvent);
        return animation_element.has_event_listener(HTML::EventNames::beginEvent)
            || animation_element.has_event_listener(HTML::EventNames::endEvent)
            || animation_element.has_event_listener(HTML::EventNames::repeatEvent);
    }

    // Script animations do not dispatch CSS animation events, even when an ancestor listens for them.
    if (animation.has_value() && state.kind != RustFFI::FfiAnimationKind::CssAnimation)
        return false;

    auto* target = this->target();
    VERIFY(target);
    return event_is_heard_at(*target, [only_iteration_events](DOM::EventTarget const& event_target) {
        if (only_iteration_events)
            return event_target.has_event_listener(HTML::EventNames::animationiteration)
                || event_target.has_event_listener(HTML::EventNames::webkitAnimationIteration);

        return event_target.has_event_listener(HTML::EventNames::animationcancel)
            || event_target.has_event_listener(HTML::EventNames::animationiteration)
            || event_target.has_event_listener(HTML::EventNames::animationstart)
            || event_target.has_event_listener(HTML::EventNames::webkitAnimationIteration)
            || event_target.has_event_listener(HTML::EventNames::webkitAnimationStart);
    });
}

static bool is_in_display_none_subtree_ignoring_animations(DOM::AbstractElement abstract_element)
{
    if (abstract_element.pseudo_element().has_value() && abstract_element.element().installed_style(abstract_element.pseudo_element()).display_is_none_ignoring_animations())
        return true;
    return abstract_element.element().has_inclusive_ancestor_with_display_none_ignoring_animations();
}

void EffectRef::update_computed_properties(AnimationUpdateContext& context) const
{
    auto* target = this->target();
    if (!target || !target->is_connected())
        return;

    // An effect that animates `display` can be the very thing holding its `display: none` target visible, and
    // the update being skipped is the one that applies or removes that override, so it must always run.
    // NB: A descendant's retained style can still describe a display-none subtree while ancestor visibility
    //     reactions are propagating, so inspect the current target and ancestor styles before skipping its update.
    // NB: Only a pseudo-element can skip it. What an element animates reaches the SVG paint servers below it, which
    //     keep their style under display: none.
    DOM::AbstractElement abstract_element { *target, pseudo_element_type() };
    if (abstract_element.pseudo_element().has_value()
        && !target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Display))
        && is_in_display_none_subtree_ignoring_animations(abstract_element))
        return;

    target->update_animated_properties({}, pseudo_element_type(), *this, context);
}

extern "C" {

// Declared in Bindings/Rust/src/animations/animation.rs.
void* web_animation_object_realm(RustFFI::AnimationCell const*);
void* web_animation_object_event_target(RustFFI::AnimationCell const*);
size_t web_animation_object_memory_size(RustFFI::AnimationCell const*);
void web_animation_perform_promise_effect(JSVM*, RustFFI::FfiHostEffect const*);

// Defined in Bindings/Rust/src/animations/exports.rs.
void* web_animation_playback_event_create(JSVM*, void* realm, FlatPtr event_name, RustFFI::OptionalTimeValue current_time, RustFFI::OptionalTimeValue timeline_time);

// What Bindings/Rust/src/animations asks of the host.
void web_animation_perform_host_effect(RustFFI::FfiHostEffect const*);
void web_animation_update_style_of_owning_element(RustFFI::AnimationCell const*);
void web_animation_update_style_of_owning_element_document(RustFFI::AnimationCell const*);
void* web_animation_realm_of_element(DOM::Element*);
void* web_animation_document_timeline_of_document(DOM::Document*);
void web_animation_visit_named_cells(RustFFI::AnimationCell const*, void* visitor);
}

static Optional<DOM::AbstractElement> owning_element_of(AnimationRef const& animation)
{
    auto owner = RustFFI::animations_animation_owning_element(&animation.record());
    if (!owner.element)
        return {};
    Optional<CSS::PseudoElement> pseudo_element;
    if (owner.has_pseudo_element)
        pseudo_element = static_cast<CSS::PseudoElement>(owner.pseudo_element);
    return DOM::AbstractElement { *static_cast<DOM::Element*>(owner.element), pseudo_element };
}

// NB: Script observes a CSS animation or transition with the style of its owning element computed.
void web_animation_update_style_of_owning_element(RustFFI::AnimationCell const* record)
{
    if (auto owning_element = owning_element_of(AnimationRef { *record }); owning_element.has_value())
        owning_element->document().update_style_for_element(*owning_element, DOM::Document::StyleUpdateMode::OnlyIfNeeded);
}

// NB: Script observes the play state of an animation with the style of the document of its owning element computed.
void web_animation_update_style_of_owning_element_document(RustFFI::AnimationCell const* record)
{
    if (auto owning_element = owning_element_of(AnimationRef { *record }); owning_element.has_value())
        owning_element->document().update_style();
}

void* web_animation_realm_of_element(DOM::Element* element)
{
    return &HTML::relevant_realm(element->document());
}

void* web_animation_document_timeline_of_document(DOM::Document* document)
{
    return document->timeline().ptr();
}

void web_animation_visit_named_cells(RustFFI::AnimationCell const* animation, void* visitor)
{
    RustFFI::animations_animation_visit_named_cells(animation, visitor, visit_host_cell);
}

void set_owning_element(AnimationRef const& animation, DOM::AbstractElement owner)
{
    auto pseudo_element = owner.pseudo_element();
    RustFFI::animations_animation_set_owning_element(&animation.record(), {
                                                                              .element = const_cast<DOM::Element*>(&owner.element()),
                                                                              .pseudo_element = pseudo_element.has_value() ? to_underlying(*pseudo_element) : u8 { 0 },
                                                                              .has_pseudo_element = pseudo_element.has_value(),
                                                                          });
}

size_t object_memory_size(AnimationRef const& animation)
{
    return web_animation_object_memory_size(&animation.record());
}

void AnimationRef::visit_edges(GC::Cell::Visitor& visitor) const
{
    RustFFI::animations_animation_visit_host_cells(&record(), &visitor, visit_host_cell);
}

// https://www.w3.org/TR/web-animations-1/#animation-relevant
bool AnimationRef::is_relevant() const
{
    // NB: The local time of the associated effect is the current time of the animation.
    auto state = timing_state();
    return RustFFI::animations_is_relevant(&record(), state.has_effect ? state.current_time : to_ffi(Optional<TimeValue> {}));
}

void AnimationRef::update() const
{
    RustFFI::animations_animation_update(&record(), perform_host_effect);
}

void AnimationRef::disassociate_from_target_if_inert() const
{
    RustFFI::animations_animation_disassociate_from_target_if_inert(&record(), perform_host_effect);
}

void AnimationRef::run_pending_play_task_if_ready() const
{
    RustFFI::animations_animation_run_pending_play_task_if_ready(&record(), perform_host_effect);
}

void AnimationRef::cancel(bool invalidate) const
{
    // Note: When called from JS, we always want to invalidate the animation target's style. However, this method is
    //       also called from the StyleComputer when the animation-name CSS property changes. That happens in the
    //       middle of a cascade, and importantly, _before_ computing the animation effect stack, so there is no
    //       need for another invalidation. It would also schedule a redundant follow-up reaction while the current
    //       style computation is already updating the animation effect stack.

    // NB: Cancelling an animation that is not idle settles its promises and creates its cancel event.
    Optional<HTML::TemporaryExecutionContext> execution_context;
    if (timing_state().play_state != RustFFI::AnimationPlayState::Idle)
        execution_context.emplace(HTML::relevant_realm(relevant_global_object_of(*this)));

    RustFFI::animations_animation_cancel(&record(), invalidate, perform_host_effect);
}

i32 compare_elements_in_tree_order_for_composite_order(void* first, void* second)
{
    auto& first_element = *static_cast<DOM::Element*>(first);
    auto& second_element = *static_cast<DOM::Element*>(second);
    return first_element.document().compare_elements_in_tree_order_for_composite_order(first_element, second_element);
}

// The realm of the object of the animation, or null where it has none.
static JS::Realm* realm_of_object(AnimationRef const& animation)
{
    return static_cast<JS::Realm*>(web_animation_object_realm(&animation.record()));
}

JS::Object& relevant_global_object_of(AnimationRef const& animation)
{
    if (auto* realm = realm_of_object(animation))
        return realm->global_object();
    auto* element = static_cast<DOM::Element*>(RustFFI::animations_animation_owning_element(&animation.record()).element);
    if (!element)
        element = static_cast<DOM::Element*>(RustFFI::animations_smil_animation_element(&animation.record()));
    VERIFY(element);
    return element->document().relevant_settings_object().global_object();
}

// Whether an invalidation changes the animation as the last rendering update planned it for the render clock, rather
// than move it on in time, as a rendering update does.
enum class ChangesPlan : bool {
    No,
    Yes,
};

// Invalidates the style the associated effect of the animation produces.
static void invalidate_effect(AnimationRef const& animation, ChangesPlan changes_plan)
{
    auto effect = animation.effect();
    if (!effect.has_value())
        return;
    // The render clock's lanes tick the document's animations as the last rendering update planned them.
    if (auto* target = effect->target(); changes_plan == ChangesPlan::Yes && target && target->document().layout_node_arena_if_created())
        Layout::RustFFI::document_host_note_animation_change(target->document().layout_node_arena_if_created()->host());
    effect->invalidate();
}

// https://www.w3.org/TR/web-animations-1/#update-an-animations-finished-state
// https://www.w3.org/TR/web-animations-1/#cancel-an-animation
// https://www.w3.org/TR/web-animations-1/#remove-replaced-animations
static Utf16FlyString const& playback_event_type_name(RustFFI::PlaybackEventType type)
{
    switch (type) {
    case RustFFI::PlaybackEventType::Finish:
        return HTML::EventNames::finish;
    case RustFFI::PlaybackEventType::Cancel:
        return HTML::EventNames::cancel;
    case RustFFI::PlaybackEventType::Remove:
        return HTML::EventNames::remove;
    }
    VERIFY_NOT_REACHED();
}

// NB: A remove event is created in the realm of the document whose replaced animations are removed, which is the
//     document for timing of the animation, and whose pending animation event queue it joins.
static JS::Realm& playback_event_realm(JS::Realm& realm, RustFFI::PlaybackEventType type, DOM::Document const* document_for_timing)
{
    if (type == RustFFI::PlaybackEventType::Remove && document_for_timing)
        return HTML::relevant_realm(*document_for_timing);
    return realm;
}

// A new AnimationPlaybackEvent of the type, created in the realm.
// https://drafts.csswg.org/web-animations-2/#animationplaybackevent
static GC::Ref<DOM::Event> create_animation_playback_event(JS::Realm& realm, Utf16FlyString const& type, RustFFI::OptionalTimeValue current_time, RustFFI::OptionalTimeValue timeline_time)
{
    auto* event = web_animation_playback_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.to_utf16_string().into_raw(), current_time, timeline_time);
    return GC::Ref { *static_cast<DOM::Event*>(static_cast<GC::Cell*>(event)) };
}

static DOM::EventTarget& event_target_of_object(AnimationRef const& animation)
{
    return *static_cast<DOM::EventTarget*>(static_cast<GC::Cell*>(web_animation_object_event_target(&animation.record())));
}

// https://www.w3.org/TR/web-animations-1/#animation-document-for-timing
static GC::Ptr<DOM::Document> document_for_timing(AnimationRef const& animation)
{
    // An animation’s document for timing is the Document with which its timeline is associated. If an animation is not
    // associated with a timeline, or its timeline is not associated with a document, then it has no document for
    // timing.
    auto* timeline = static_cast<AnimationTimeline*>(RustFFI::animations_animation_timeline(&animation.record()));
    if (!timeline)
        return {};
    return timeline->associated_document();
}

static void queue_playback_event(AnimationRef const& animation, JS::Realm& realm, RustFFI::FfiHostEffect const& effect)
{
    auto is_finish = effect.playback_event_type == RustFFI::PlaybackEventType::Finish;
    auto document = document_for_timing(animation);
    auto& event_realm = playback_event_realm(realm, effect.playback_event_type, document.ptr());

    // If animation has a document for timing, then append the event to its document for timing's pending animation
    // event queue along with its target, animation.
    if (document) {
        // https://www.w3.org/TR/web-animations-1/#animation-time-to-origin-relative-time
        // NB: The procedure converted the time to a timeline time.
        Optional<double> scheduled_event_time;
        auto* timeline = static_cast<AnimationTimeline*>(RustFFI::animations_animation_timeline(&animation.record()));
        if (auto timeline_time = from_ffi(effect.scheduled_timeline_time); timeline_time.has_value()
            && timeline && !timeline->is_inactive() && timeline->can_convert_a_timeline_time_to_an_origin_relative_time())
            scheduled_event_time = timeline->convert_a_timeline_time_to_an_origin_relative_time(timeline_time);
        // NB: The queue holds what the event is, which the document creates as it dispatches it, with the time stamp
        //     of now, when the event is created in the spec.
        document->append_pending_animation_event(animation, {
                                                                .kind = RustFFI::PendingAnimationEventKind::Playback,
                                                                .css_event_type = {},
                                                                .playback_event_type = effect.playback_event_type,
                                                                .time_event_type = {},
                                                                .detail = 0,
                                                                .elapsed_time = 0,
                                                                .current_time = effect.current_time,
                                                                .timeline_time = effect.timeline_time,
                                                                .scheduled_event_time = to_ffi(scheduled_event_time),
                                                                .time_stamp = HighResolutionTime::current_high_resolution_time(event_realm.global_object()),
                                                            });
        return;
    }

    auto event = create_animation_playback_event(event_realm, playback_event_type_name(effect.playback_event_type), effect.current_time, effect.timeline_time);
    auto& target = event_target_of_object(animation);

    // Otherwise, queue a task to dispatch the event at animation. The task source for this task is the DOM manipulation
    // task source.
    if (!is_finish) {
        HTML::queue_global_task(HTML::Task::Source::DOMManipulation, realm.global_object(), GC::create_function(GC::Heap::the(), GC::captures(target, event), [](auto& target, auto& event) {
            target.dispatch_event(event);
        }));
        return;
    }

    // NB: The task to dispatch a finish event is created manually so its ID can be saved.
    auto& window_document = HTML::relevant_window(realm.global_object()).associated_document();
    auto task = HTML::Task::create(HTML::Task::Source::DOMManipulation, &window_document, GC::create_function(GC::Heap::the(), GC::captures(target, event), [](auto& target, auto& event) {
        target.dispatch_event(event);
    }));
    RustFFI::animations_animation_set_finish_notification_task(&animation.record(), task->id().value());
    (void)HTML::main_thread_event_loop().task_queue().add(task);
}

void dispatch_pending_playback_event(AnimationRef const& animation, DOM::Document const& document, RustFFI::FfiPendingAnimationEvent const& pending_event)
{
    auto* realm = realm_of_object(animation);
    VERIFY(realm);
    auto event = create_animation_playback_event(playback_event_realm(*realm, pending_event.playback_event_type, &document), playback_event_type_name(pending_event.playback_event_type), pending_event.current_time, pending_event.timeline_time);
    event->set_time_stamp(pending_event.time_stamp);
    event_target_of_object(animation).dispatch_event(event);
}

void web_animation_perform_host_effect(RustFFI::FfiHostEffect const* host_effect)
{
    perform_host_effect(host_effect);
}

void perform_host_effect(RustFFI::FfiHostEffect const* host_effect)
{
    auto const& effect = *host_effect;
    AnimationRef animation { *effect.animation };
    auto effect_target = [&] -> DOM::Element* {
        auto associated_effect = animation.effect();
        return associated_effect.has_value() ? associated_effect->target() : nullptr;
    };
    using Kind = RustFFI::FfiHostEffectKind;
    switch (effect.kind) {
    case Kind::ReplaceReadyPromise:
    case Kind::ResolveReadyPromise:
    case Kind::RejectReadyPromise:
    case Kind::ReplaceFinishedPromise:
    case Kind::ResolveFinishedPromise:
    case Kind::RejectFinishedPromise:
        // NB: Only an animation with an object has promises script can observe.
        web_animation_perform_promise_effect(JS::HostABI::vm_to_abi(JS::VM::the()), host_effect);
        return;
    case Kind::QueuePlaybackEvent:
        // NB: Only an animation with an object can hear its playback events.
        if (auto* realm = realm_of_object(animation))
            queue_playback_event(animation, *realm, effect);
        return;
    case Kind::QueueFinishNotificationTask: {
        // NB: The task runs whether or not the animation has an object, as the finish notification steps note that the
        //     animation finished.
        auto& global = relevant_global_object_of(animation);
        auto& document = HTML::relevant_window(global).associated_document();
        auto task = HTML::Task::create(HTML::Task::Source::DOMManipulation, &document, GC::create_function(GC::Heap::the(), GC::captures(animation, global), [](AnimationRef const& animation, JS::Object& global) {
            HTML::TemporaryExecutionContext context { HTML::relevant_realm(global) };
            RustFFI::animations_animation_run_finish_notification_steps(&animation.record(), perform_host_effect);
        }));
        RustFFI::animations_animation_set_finish_notification_task(&animation.record(), task->id().value());
        (void)HTML::main_thread_event_loop().task_queue().add(move(task));
        return;
    }
    case Kind::CancelFinishNotificationTask:
        HTML::main_thread_event_loop().task_queue().remove_tasks_matching([id = HTML::TaskID { effect.task }](auto const& task) {
            return task.id() == id;
        });
        return;
    case Kind::InvalidateEffect:
        invalidate_effect(animation, effect.changes_plan ? ChangesPlan::Yes : ChangesPlan::No);
        return;
    case Kind::InvalidateUnlessThrottled: {
        auto keyframe_effect = animation.effect();
        if (!keyframe_effect.has_value())
            return;
        if (keyframe_effect->can_skip_per_frame_style_update()) {
            if (auto* target = keyframe_effect->target())
                target->document().note_throttled_animation_style_update();
        } else if (effect.invalidate) {
            invalidate_effect(animation, ChangesPlan::No);
        }
        return;
    }
    case Kind::AssociateWithTarget:
        static_cast<DOM::Element*>(effect.element)->associate_with_animation(animation);
        return;
    case Kind::DisassociateFromTarget:
        static_cast<DOM::Element*>(effect.element)->disassociate_with_animation(animation, EffectRef { *effect.effect });
        return;
    case Kind::RecomputeEffectStyle: {
        AnimationUpdateContext context;
        EffectRef { *effect.effect }.update_computed_properties(context);
        return;
    }
    case Kind::RequestRepaint:
        if (auto* target = effect_target())
            target->set_needs_repaint(InvalidateDisplayList::No);
        return;
    case Kind::ScheduleWakeup:
        if (auto* target = effect_target())
            target->document().schedule_compositor_animation_wakeup(effect.delay);
        return;
    }
    VERIFY_NOT_REACHED();
}

// NB: What committing the computed styles of an animation answers its bindings.
enum class CommitStylesResult : u8 {
    Committed,
    TargetCannotHaveInlineStyle,
    TargetIsNotBeingRendered,
};

extern "C" WEB_API CommitStylesResult web_animation_commit_styles(RustFFI::AnimationCell const* animation_cell);

// https://drafts.csswg.org/web-animations-1/#commit-computed-styles
extern "C" WEB_API CommitStylesResult web_animation_commit_styles(RustFFI::AnimationCell const* animation_cell)
{
    AnimationRef animation { *animation_cell };

    // 1. Let targets be the set of all effect targets for animation effects associated with animation.
    auto effect = animation.effect();
    if (!effect.has_value() || !effect->target())
        return CommitStylesResult::Committed;
    GC::Ref<DOM::Element> target = *effect->target();

    // 2. For each target in targets:
    //    1. If target is not an element capable of having a style attribute (for example, it is a pseudo-element or is
    //       an element in a document format for which style attributes are not defined) throw a
    //       "NoModificationAllowedError" DOMException and abort these steps.
    auto const& namespace_uri = target->namespace_uri();
    if (effect->pseudo_element_type().has_value() || (namespace_uri != Namespace::HTML && namespace_uri != Namespace::SVG && namespace_uri != Namespace::MathML))
        return CommitStylesResult::TargetCannotHaveInlineStyle;

    OrderedHashMap<CSS::PropertyNameAndID, NonnullRefPtr<CSS::StyleValue const>> declarations;
    {
        // 2. If, after applying any pending style changes, target is not being rendered, throw an "InvalidStateError"
        //    DOMException and abort these steps.
        auto& document = target->document();
        Layout::ForcedReadScope read { document };
        document.update_layout(DOM::UpdateLayoutReason::AnimationCommitStyles);
        if (!target->is_connected() || !target->has_style())
            return CommitStylesResult::TargetIsNotBeingRendered;
        auto computed_style = target->computed_style();
        if (!target->unsafe_layout_node(read) && (!computed_style->display().is_contents() || computed_style->in_display_none_subtree()))
            return CommitStylesResult::TargetIsNotBeingRendered;

        // 4. Let targeted properties be the set of physical longhand properties that are a target property for at
        //    least one animation effect associated with animation whose effect target is target.
        HashTable<CSS::PropertyNameAndID> targeted_properties;
        auto add_longhand = [&](CSS::PropertyID property) {
            auto physical_property = CSS::map_logical_alias_to_physical_property(property, { computed_style->writing_mode(), computed_style->direction() });
            targeted_properties.set(CSS::PropertyNameAndID::from_id(physical_property));
        };
        RustFFI::FfiKeyframeSetFacts facts {};
        if (auto keyframe_set = effect->keyframe_set())
            facts = keyframe_set.facts();
        for (auto raw_property : ReadonlySpan<u16> { facts.target_properties, facts.target_property_count }) {
            auto property = static_cast<CSS::PropertyID>(raw_property);
            if (CSS::property_is_shorthand(property)) {
                for (auto longhand : CSS::expanded_longhands_for_shorthand(property))
                    add_longhand(longhand);
            } else {
                add_longhand(property);
            }
        }
        for (auto name : ReadonlySpan<FlatPtr> { reinterpret_cast<FlatPtr const*>(facts.target_custom_properties), facts.target_custom_property_count })
            targeted_properties.set(CSS::PropertyNameAndID::from_name(Utf16FlyString::from_raw(name)).release_value());

        // 5. For each property, property, in targeted properties:
        //    1. Let partialEffectStack be a copy of the effect stack for property on target.
        //    2. If animation's replace state is removed, add all animation effects associated with animation whose
        //       effect target is target and that include property as a target property to partialEffectStack.
        //    3. Remove from partialEffectStack any animation effects whose associated animation has a higher composite
        //       order than animation.
        Vector<EffectRef> effects;
        for (auto other_animation : target->associated_animations_in_composite_order()) {
            auto other_effect = other_animation.effect();
            if (other_effect.has_value() && !other_effect->pseudo_element_type().has_value()
                && RustFFI::animations_composite_order(&other_animation.record(), animation_cell, compare_elements_in_tree_order_for_composite_order) < 0)
                effects.append(other_effect.release_value());
        }
        effects.append(*effect);

        // 4. Let effect value be the result of calculating the result of partialEffectStack for property using
        //    target's computed style (see §5.4.3 Calculating the result of an effect stack) and setting the
        //    endpoint-inclusive active interval flag to true when calculating the animation effect phase
        //    (see §4.6.6 Animation effect phases and states).
        DOM::AbstractElement element { target, {} };
        // NB: A timeline script observed ahead of its current time in this task is sampled at that time, as the style
        //     update above was, so the committed values are those of the time script read.
        RefPtr<CSS::ComputedStyleWorkingSet> committed_style;
        auto commit = [&] { committed_style = document.style_computer().compute_style_for_animation_commit(read, element, effects.span()); };
        AnimationTimeline::at_times_observed_ahead(document.associated_animation_timelines().begin(), document.associated_animation_timelines().end(), commit);
        auto style = committed_style.release_nonnull();
        auto custom_properties = element.custom_property_data();
        if (custom_properties && element.custom_property_data_is_animation_overlay())
            custom_properties = custom_properties->parent();
        for (auto const& property : targeted_properties) {
            if (property.is_custom_property()) {
                RefPtr<CSS::StyleValue const> value;
                if (auto animated_value = style->animated_custom_properties().get(property.name()); animated_value.has_value())
                    value = **animated_value;
                else if (custom_properties) {
                    if (auto base_value = custom_properties->get(property.name()); base_value.has_value())
                        value = base_value->value;
                }
                if (!value) {
                    if (auto registration = document.get_registered_custom_property(property.name()); registration.has_value() && registration->initial_value)
                        value = CSS::compute_registered_custom_property_initial_value(document, *registration);
                }
                if (value)
                    declarations.set(property, value.release_nonnull());
            } else {
                declarations.set(property, CSS::unscaled_by_zoom(property.id(), style->property(property.id()), computed_style->effective_zoom()));
            }
        }
    }

    // 3. Let inline style be the result of getting the CSS declaration block corresponding to target's style
    //    attribute. If target does not have a style attribute, let inline style be a new empty CSS declaration block
    //    with the owner node set to target.
    auto inline_style = target->style();
    // 5.5. Set a CSS declaration of property for effect value in inline style.
    // 6. Update style attribute for inline style.
    inline_style->set_computed_properties(declarations);
    return CommitStylesResult::Committed;
}

// NB: What the reflection of the keyframes of a CSS animation or transition asks of the style of its target.
extern "C" WEB_API bool web_animation_compute_css_keyframe_values(DOM::Element* target, bool has_pseudo_element, u8 pseudo_element, CSS::ComputedValuesFFI::FfiReflectedKeyframeProperty const* properties, size_t property_count, size_t keyframe_count, CSS::ComputedValuesFFI::FfiStyleSheetResourceContext const* resource_context, void* context, CSS::StyleComputer::CSSKeyframeValueCallback emit);
extern "C" WEB_API double web_animation_effective_zoom_of_target(DOM::Element* target, bool has_pseudo_element, u8 pseudo_element);

extern "C" WEB_API bool web_animation_compute_css_keyframe_values(DOM::Element* target, bool has_pseudo_element, u8 pseudo_element, CSS::ComputedValuesFFI::FfiReflectedKeyframeProperty const* properties, size_t property_count, size_t keyframe_count, CSS::ComputedValuesFFI::FfiStyleSheetResourceContext const* resource_context, void* context, CSS::StyleComputer::CSSKeyframeValueCallback emit)
{
    DOM::AbstractElement abstract_target { *target, has_pseudo_element ? static_cast<CSS::PseudoElement>(pseudo_element) : Optional<CSS::PseudoElement> {} };
    if (!abstract_target.installed_style())
        return false;
    target->document().style_computer().compute_css_keyframe_values(abstract_target, { properties, property_count }, keyframe_count, *resource_context, context, emit);
    return true;
}

extern "C" WEB_API double web_animation_effective_zoom_of_target(DOM::Element* target, bool has_pseudo_element, u8 pseudo_element)
{
    DOM::AbstractElement abstract_target { *target, has_pseudo_element ? static_cast<CSS::PseudoElement>(pseudo_element) : Optional<CSS::PseudoElement> {} };
    if (auto style = abstract_target.computed_style())
        return style->effective_zoom();
    return 1;
}

}
