/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Animations/AnimationEffect.h>
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

void keep_animation_alive_in(void* root_vector, RustFFI::AnimationCell const* animation)
{
    static_cast<GC::RootVector<AnimationRef>*>(root_vector)->append(AnimationRef { *animation });
}

void in_execution_context_of_animation(void*, RustFFI::AnimationCell const* animation, void (*run)(void*), void* run_context)
{
    HTML::TemporaryExecutionContext execution_context { HTML::relevant_realm(relevant_global_object_of(AnimationRef { *animation })) };
    run(run_context);
}

Optional<CSS::PseudoElement> EffectRef::pseudo_element_type() const
{
    auto pseudo_element = RustFFI::animations_effect_target(&record()).pseudo_element;
    if (!pseudo_element.serialization)
        return {};
    return static_cast<CSS::PseudoElement>(pseudo_element.kind);
}

void EffectRef::invalidate() const
{
    RustFFI::animations_effect_forget_what_the_host_concluded(&record());
    if (auto* target = this->target())
        target->document().set_needs_animated_style_update(*this);
}

// What the animation records ask of the host.
extern "C" void web_animation_set_needs_animated_style_update(void const* effect, DOM::Element* target);
extern "C" RustFFI::FfiTargetFacts web_animation_target_facts(DOM::Element* target, u32 wanted);

void web_animation_set_needs_animated_style_update(void const* effect, DOM::Element* target)
{
    target->document().set_needs_animated_style_update(EffectRef { *static_cast<RustFFI::AnimationEffectCell const*>(effect) });
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

static bool target_fact_holds(DOM::Element& target, u32 fact)
{
    switch (fact) {
    case RustFFI::TARGET_FACT_LAYOUT_IS_UP_TO_DATE:
        return target.document().layout_is_up_to_date();
    case RustFFI::TARGET_FACT_IS_CONNECTED:
        return target.is_connected();
    case RustFFI::TARGET_FACT_HAS_LAYOUT_BOX:
        return target.has_layout_box();
    case RustFFI::TARGET_FACT_IS_SVG:
        return target.namespace_uri() == Namespace::SVG;
    case RustFFI::TARGET_FACT_INSIDE_BOXLESS_CONTAINER:
        for (auto navigable = target.document().navigable(); navigable;) {
            auto container = navigable->container();
            if (!container)
                break;
            if (container->document().layout_is_up_to_date() && !container->has_layout_box())
                return true;
            navigable = container->document().navigable();
        }
        return false;
    case RustFFI::TARGET_FACT_UNSEEN_IN_DISPLAY_NONE_SUBTREE:
        return target_is_unseen_in_display_none_subtree(target);
    case RustFFI::TARGET_FACT_HIDDEN_WITHOUT_VISIBLE_DESCENDANT: {
        Layout::ForcedReadScope read { target.document() };
        auto const* layout_node = target.unsafe_layout_node(read);
        if (!layout_node || layout_node->visibility() != CSS::Visibility::Hidden)
            return false;
        bool has_visible_descendant = false;
        layout_node->for_each_in_inclusive_subtree_of_type<Layout::NodeWithStyle>([&](auto const& descendant) {
            if (descendant.visibility() != CSS::Visibility::Visible)
                return TraversalDecision::Continue;
            has_visible_descendant = true;
            return TraversalDecision::Break;
        });
        return !has_visible_descendant;
    }
    case RustFFI::TARGET_FACT_HEARS_CSS_ANIMATION_EVENTS:
        return event_is_heard_at(target, [](DOM::EventTarget const& event_target) {
            return event_target.has_event_listener(HTML::EventNames::animationcancel)
                || event_target.has_event_listener(HTML::EventNames::animationiteration)
                || event_target.has_event_listener(HTML::EventNames::animationstart)
                || event_target.has_event_listener(HTML::EventNames::webkitAnimationIteration)
                || event_target.has_event_listener(HTML::EventNames::webkitAnimationStart);
        });
    case RustFFI::TARGET_FACT_HEARS_CSS_ANIMATION_ITERATION_EVENTS:
        return event_is_heard_at(target, [](DOM::EventTarget const& event_target) {
            return event_target.has_event_listener(HTML::EventNames::animationiteration)
                || event_target.has_event_listener(HTML::EventNames::webkitAnimationIteration);
        });
    case RustFFI::TARGET_FACT_HEARS_TIME_EVENTS:
        return target.has_event_listener(HTML::EventNames::beginEvent)
            || target.has_event_listener(HTML::EventNames::endEvent)
            || target.has_event_listener(HTML::EventNames::repeatEvent);
    case RustFFI::TARGET_FACT_HEARS_REPEAT_EVENT:
        return target.has_event_listener(HTML::EventNames::repeatEvent);
    }
    VERIFY_NOT_REACHED();
}

RustFFI::FfiTargetFacts web_animation_target_facts(DOM::Element* target, u32 wanted)
{
    RustFFI::FfiTargetFacts answer {};
    for (u32 fact = 1; fact <= wanted; fact <<= 1) {
        if (!(wanted & fact))
            continue;
        if (fact == RustFFI::TARGET_FACT_SKIP_KEY) {
            Layout::ForcedReadScope read { target->document() };
            answer.key = {
                .target_style_generation = target->animation_style_generation(),
                .target_subtree_style_generation = target->animation_subtree_style_generation(),
                .layout_node = bit_cast<FlatPtr>(target->unsafe_layout_node(read)),
                .target_is_connected = target->is_connected(),
            };
        } else if (target_fact_holds(*target, fact)) {
            answer.facts |= fact;
        }
    }
    return answer;
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
        && !target_properties().contains(CSS::PropertyID::Display)
        && is_in_display_none_subtree_ignoring_animations(abstract_element))
        return;

    target->update_animated_properties({}, pseudo_element_type(), *this, context);
}

extern "C" {

// Declared in Bindings/Rust/src/animations/animation.rs.
void* web_animation_object_realm(RustFFI::AnimationCell const*);
void web_animation_perform_object_effect(JSVM*, RustFFI::FfiHostEffect const*);

// What Bindings/Rust/src/animations asks of the host.
enum class StyleUpdateScope : u8 {
    Element,
    Document,
};
void web_animation_update_style_of_element(DOM::Element*, bool has_pseudo_element, u8 pseudo_element, StyleUpdateScope);
void* web_animation_realm_of_element(DOM::Element*);
void* web_animation_realm_of_document(DOM::Document*);
double web_animation_current_high_resolution_time(void* realm);
void web_animation_append_pending_playback_event(DOM::Document*, RustFFI::AnimationCell const*, RustFFI::PlaybackEventType, RustFFI::OptionalTimeValue current_time, RustFFI::OptionalTimeValue timeline_time, RustFFI::OptionalNumber scheduled_event_time, double time_stamp);
void web_animation_event_set_time_stamp(DOM::Event*, double time_stamp);
}

// NB: Script observes a CSS animation or transition with the style of its owning element computed, and the play state
//     of an animation with the style of the document of its owning element computed.
void web_animation_update_style_of_element(DOM::Element* element, bool has_pseudo_element, u8 pseudo_element, StyleUpdateScope scope)
{
    if (scope == StyleUpdateScope::Document) {
        element->document().update_style();
        return;
    }
    DOM::AbstractElement owning_element { *element, has_pseudo_element ? static_cast<CSS::PseudoElement>(pseudo_element) : Optional<CSS::PseudoElement> {} };
    element->document().update_style_for_element(owning_element, DOM::Document::StyleUpdateMode::OnlyIfNeeded);
}

void* web_animation_realm_of_element(DOM::Element* element)
{
    return &HTML::relevant_realm(element->document());
}

void* web_animation_realm_of_document(DOM::Document* document)
{
    return &HTML::relevant_realm(*document);
}

double web_animation_current_high_resolution_time(void* realm)
{
    return HighResolutionTime::current_high_resolution_time(static_cast<JS::Realm*>(realm)->global_object());
}

void web_animation_append_pending_playback_event(DOM::Document* document, RustFFI::AnimationCell const* animation, RustFFI::PlaybackEventType type, RustFFI::OptionalTimeValue current_time, RustFFI::OptionalTimeValue timeline_time, RustFFI::OptionalNumber scheduled_event_time, double time_stamp)
{
    document->append_pending_animation_event(AnimationRef { *animation }, {
                                                                              .kind = RustFFI::PendingAnimationEventKind::Playback,
                                                                              .css_event_type = {},
                                                                              .playback_event_type = type,
                                                                              .time_event_type = {},
                                                                              .detail = 0,
                                                                              .elapsed_time = 0,
                                                                              .current_time = current_time,
                                                                              .timeline_time = timeline_time,
                                                                              .scheduled_event_time = scheduled_event_time,
                                                                              .time_stamp = time_stamp,
                                                                          });
}

void web_animation_event_set_time_stamp(DOM::Event* event, double time_stamp)
{
    event->set_time_stamp(time_stamp);
}

void AnimationRef::visit_edges(GC::Cell::Visitor& visitor) const
{
    RustFFI::animations_animation_visit_host_cells(&record(), &visitor, visit_host_cell);
}

i32 compare_elements_in_tree_order_for_composite_order(void* first, void* second)
{
    auto& first_element = *static_cast<DOM::Element*>(first);
    auto& second_element = *static_cast<DOM::Element*>(second);
    return first_element.document().compare_elements_in_tree_order_for_composite_order(first_element, second_element);
}

JS::Object& relevant_global_object_of(AnimationRef const& animation)
{
    if (auto* realm = static_cast<JS::Realm*>(web_animation_object_realm(&animation.record())))
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

void web_animation_perform_host_effect(RustFFI::FfiHostEffect const* host_effect)
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
    case Kind::QueuePlaybackEvent:
        web_animation_perform_object_effect(JS::HostABI::vm_to_abi(JS::VM::the()), host_effect);
        return;
    case Kind::QueueFinishNotificationTask: {
        // NB: The task runs whether or not the animation has an object, as the finish notification steps note that the
        //     animation finished.
        auto& global = relevant_global_object_of(animation);
        auto& document = HTML::relevant_window(global).associated_document();
        auto task = HTML::Task::create(HTML::Task::Source::DOMManipulation, &document, GC::create_function(GC::Heap::the(), GC::captures(animation, global), [](AnimationRef const& animation, JS::Object& global) {
            HTML::TemporaryExecutionContext context { HTML::relevant_realm(global) };
            RustFFI::animations_animation_run_finish_notification_steps(&animation.record(), web_animation_perform_host_effect);
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

// NB: What the timelines associated with a document ask of it and of its layout.
extern "C" {

// Declared in Rust/src/animations/timeline_host.rs.
RustFFI::DocumentAnimations* web_animation_document_animations(DOM::Document*);
RustFFI::FfiDocumentClock web_animation_document_clock(DOM::Document*);
void web_animation_find_scroll_timeline_boxes(RustFFI::FfiScrollTimelineSource const*, bool is_view, void* context, void (*measure)(void*, RustFFI::FfiScrollTimelineBoxes const*));
DOM::Element* web_animation_scroll_timeline_source(RustFFI::FfiScrollTimelineSource const*, bool is_view);

// Declared in Bindings/Rust/src/animations.
DOM::Element* web_animation_document_scrolling_element(DOM::Document*);
CSS::StyleValueFFI::StyleValueData const* web_animation_absolutize_style_value(DOM::Document*, CSS::StyleValueFFI::StyleValueData const*);
}

RustFFI::DocumentAnimations* web_animation_document_animations(DOM::Document* document)
{
    return &document->document_animations();
}

RustFFI::FfiDocumentClock web_animation_document_clock(DOM::Document* document)
{
    auto& settings = document->relevant_settings_object();
    auto& global = settings.global_object();
    return {
        .now = HighResolutionTime::relative_high_resolution_time(HighResolutionTime::unsafe_shared_current_time(), global),
        .task_generation = settings.responsible_event_loop().task_generation(),
        .timeline_start_time = document->last_animation_frame_timestamp().value_or_lazy_evaluated([&] {
            return HighResolutionTime::relative_high_resolution_time(document->load_timing_info().navigation_start_time, global);
        }),
    };
}

// The target an anonymous scroll timeline finds its scroller from.
static DOM::AbstractElement anonymous_target(RustFFI::FfiScrollTimelineSource const& source)
{
    Optional<CSS::PseudoElement> pseudo_element;
    if (source.has_pseudo_element)
        pseudo_element = static_cast<CSS::PseudoElement>(source.pseudo_element);
    return DOM::AbstractElement { *static_cast<DOM::Element*>(source.target), pseudo_element };
}

// https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-source
static GC::Ptr<DOM::Element const> scroll_timeline_source(RustFFI::FfiScrollTimelineSource const& source)
{
    if (!source.is_anonymous)
        return static_cast<DOM::Element const*>(source.element);
    auto target = anonymous_target(source);
    switch (static_cast<CSS::Scroller>(source.scroller)) {
    case CSS::Scroller::Root:
        return target.document().document_element();
    case CSS::Scroller::Nearest: {
        GC::Ptr<DOM::Element const> ancestor = target.flat_tree_parent_element();

        // Whether a box is a scroll container depends on its used overflow, which is visible for a body whose
        // overflow is propagated to the viewport. The document element stands for the viewport.
        Layout::ForcedReadScope read { target.document() };
        while (ancestor && !ancestor->is_document_element()) {
            auto const* layout_node = ancestor->unsafe_layout_node(read);
            if (layout_node && layout_node->is_scroll_container())
                break;
            ancestor = ancestor->flat_tree_parent_element();
        }

        return ancestor;
    }
    case CSS::Scroller::Self:
        return target.element();
    }
    VERIFY_NOT_REACHED();
}

void web_animation_find_scroll_timeline_boxes(RustFFI::FfiScrollTimelineSource const* source, bool is_view, void* context, void (*measure)(void*, RustFFI::FfiScrollTimelineBoxes const*))
{
    // NB: The record measures the boxes the timeline follows where the render state of the document whose viewport or
    //     element the source propagates to lays them out.
    RustFFI::FfiScrollTimelineBoxes boxes {
        .host = nullptr,
        .read = nullptr,
        .scroller = Compositing::RustFFI::NodeSlotId_INVALID,
        .has_scroll_node = false,
        .scroll_node = {},
        .scroller_style_node = 0,
        .subject = Compositing::RustFFI::NodeSlotId_INVALID,
        .subject_style_node = 0,
    };

    // https://drafts.csswg.org/scroll-animations-1/#scroll-notation
    // References to the root element propagate to the document viewport (which functions as its scroll container).
    auto const element = scroll_timeline_source(*source);
    Variant<GC::Ptr<DOM::Element const>, GC::Ptr<DOM::Document>> propagated_source = element;
    if (element && element.ptr() == element->document().document_element())
        propagated_source = element->owner_document();
    if (propagated_source.visit([](auto const& source) { return source == nullptr; }))
        return measure(context, &boxes);

    Layout::ForcedReadScope read { propagated_source.visit([](auto const& source) -> DOM::Document const& { return source->document(); }) };
    auto const* layout_node = propagated_source.visit([&read](auto const& source) -> Layout::NodeWithStyle const* { return source->unsafe_layout_node(read); });
    if (!layout_node)
        return measure(context, &boxes);

    boxes.host = layout_node->document_host();
    boxes.read = read;
    boxes.scroller = Layout::Node::slot_id(layout_node);
    if (auto scroll_node = Painting::async_scroll_node_stable_id(*layout_node); scroll_node.has_value()) {
        boxes.has_scroll_node = true;
        boxes.scroll_node = { .node_id = scroll_node->node_id.value(), .kind = to_underlying(scroll_node->kind), .pseudo_element_type = scroll_node->pseudo_element_type };
    }
    boxes.scroller_style_node = propagated_source.visit(
        [](GC::Ptr<DOM::Element const> const& element) { return element->style_node_id().value(); },
        [](GC::Ptr<DOM::Document> const&) -> u32 { return 0; });
    // NB: A view timeline follows the box of its subject besides its scroller's.
    if (is_view) {
        auto subject = anonymous_target(*source);
        boxes.subject = Layout::Node::slot_id(subject.unsafe_layout_node(read));
        // NB: A clock tick finds the subject by its style node, which a pseudo-element does not name.
        if (!subject.pseudo_element().has_value())
            boxes.subject_style_node = subject.element().style_node_id().value();
    }
    measure(context, &boxes);
}

DOM::Element* web_animation_scroll_timeline_source(RustFFI::FfiScrollTimelineSource const* source, bool is_view)
{
    if (is_view) {
        // The values of subject, source, and currentTime are all computed when any of them is requested or updated.
        auto subject = anonymous_target(*source);
        subject.document().update_layout(DOM::UpdateLayoutReason::ViewTimelineSource);

        // If the source or subject of a ViewTimeline is an element whose principal box does not exist, [...] then the
        // ViewTimeline is inactive.
        // INTEROP: Its source is then null, as WebKit and Blink have it.
        for (GC::Ptr<DOM::Element const> element = &subject.element(); element; element = element->flat_tree_parent_element()) {
            if (!element->has_style() || element->installed_style().display_is_none())
                return nullptr;
        }
    } else if (source->is_anonymous && static_cast<CSS::Scroller>(source->scroller) == CSS::Scroller::Nearest) {
        // The nearest scroll container is the nearest one by used overflow, which layout decides.
        anonymous_target(*source).document().update_layout(DOM::UpdateLayoutReason::ScrollTimelineSource);
    }
    return const_cast<DOM::Element*>(scroll_timeline_source(*source).ptr());
}

DOM::Element* web_animation_document_scrolling_element(DOM::Document* document)
{
    return const_cast<DOM::Element*>(document->scrolling_element().ptr());
}

// NB: Absolutizes a style value that script gave, as the document's values are.
CSS::StyleValueFFI::StyleValueData const* web_animation_absolutize_style_value(DOM::Document* document, CSS::StyleValueFFI::StyleValueData const* value)
{
    auto style_value = CSS::StyleValue::adopt_rust_style_value_data(CSS::StyleValueFFI::rust_style_value_retain(value));
    auto computed = style_value->absolutized(CSS::ComputationContext { .length_resolution_context = CSS::Length::ResolutionContext::for_document(*document) });
    return CSS::StyleValueFFI::rust_style_value_retain(computed->rust_style_value_data());
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
                && RustFFI::animations_animation_composite_order(&other_animation.record(), animation_cell, compare_elements_in_tree_order_for_composite_order) < 0)
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
        using Commit = decltype(commit);
        RustFFI::animations_document_animations_at_times_observed_ahead(&document.document_animations(), &commit, [](void* context) { (*static_cast<Commit*>(context))(); });
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

// NB: What the timeline records ask of the document a timeline is associated with.
extern "C" WEB_API void web_animation_note_rendering_trace_event(u8 const* event, size_t length);

extern "C" WEB_API void web_animation_note_rendering_trace_event(u8 const* event, size_t length)
{
    HTML::note_rendering_trace_event(StringView { reinterpret_cast<char const*>(event), length });
}

}

extern "C" WEB_API bool ladybird_animations_document_is_active(void* document);
extern "C" WEB_API void* ladybird_animations_element_node_document(void* element);

extern "C" WEB_API bool ladybird_animations_document_is_active(void* document)
{
    return static_cast<Web::DOM::Document const*>(document)->is_active();
}

extern "C" WEB_API void* ladybird_animations_element_node_document(void* element)
{
    return &static_cast<Web::DOM::Element*>(element)->document();
}
