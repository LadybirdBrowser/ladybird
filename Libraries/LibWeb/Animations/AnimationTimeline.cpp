/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>.
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/RecordHandles.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/Scrolling.h>

namespace Web::Animations {

// Takes a reference to a calculated inset the style engine hands over, which may be null.
static CSS::RustStyleValueHandle retain_calculated_inset(void const* value)
{
    if (!value)
        return {};
    return CSS::RustStyleValueHandle::retained(static_cast<CSS::StyleValueFFI::StyleValueData const*>(value));
}

extern "C" {

// Defined in Bindings/Rust/src/animations/animation_timeline.rs.
void* web_animation_timeline_create(JSVM*, DOM::Document*, RustFFI::FfiTimelineDescription const*);
RustFFI::TimelineCell const* web_animation_timeline_record(void* timeline);

// Declared in Bindings/Rust/src/animations.
void web_animation_timeline_did_create(void* timeline);
RustFFI::OptionalTimeValue web_animation_timeline_current_time_for_observation(void* timeline);
DOM::Element* web_scroll_timeline_source(void* timeline);
DOM::Element* web_animation_document_scrolling_element(DOM::Document*);
void web_internal_animation_timeline_set_time(void* timeline, RustFFI::OptionalNumber, bool for_observation);
CSS::StyleValueFFI::StyleValueData const* web_animation_absolutize_style_value(DOM::Document*, CSS::StyleValueFFI::StyleValueData const*);
}

static RustFFI::FfiScrollTimelineSource source_to_ffi(AnimationTimeline::ScrollSource const& source)
{
    return source.visit(
        [](GC::Ptr<DOM::Element const> const& element) -> RustFFI::FfiScrollTimelineSource {
            return { .element = const_cast<DOM::Element*>(element.ptr()), .is_anonymous = false, .scroller = 0, .target = nullptr, .pseudo_element = 0, .has_pseudo_element = false };
        },
        [](AnimationTimeline::AnonymousScrollSource const& anonymous_source) -> RustFFI::FfiScrollTimelineSource {
            auto pseudo_element = anonymous_source.target.pseudo_element();
            return {
                .element = nullptr,
                .is_anonymous = true,
                .scroller = to_underlying(anonymous_source.scroller),
                .target = const_cast<DOM::Element*>(&anonymous_source.target.element()),
                .pseudo_element = pseudo_element.has_value() ? to_underlying(*pseudo_element) : u8 { 0 },
                .has_pseudo_element = pseudo_element.has_value(),
            };
        });
}

static RustFFI::FfiViewTimelineInset inset_to_ffi(ViewTimelineInset const& inset)
{
    auto edge = [](ViewTimelineInsetEdge const& edge) -> RustFFI::FfiViewTimelineInsetEdge {
        return { .is_auto = edge.is_auto, .px = edge.px, .percentage = edge.percentage, .calculated = edge.calculated.data() };
    };
    return { .start = edge(inset.start), .end = edge(inset.end) };
}

ViewTimelineInset view_timeline_inset_from_ffi(CSS::ComputedValuesFFI::FfiViewTimelineInset const& inset)
{
    return {
        .start = { inset.start_is_auto, inset.start_px, inset.start_percentage, retain_calculated_inset(inset.start_calculated) },
        .end = { inset.end_is_auto, inset.end_px, inset.end_percentage, retain_calculated_inset(inset.end_calculated) },
    };
}

GC::Ref<AnimationTimeline> AnimationTimeline::create(DOM::Document& document, RustFFI::FfiTimelineDescription const& description)
{
    return *static_cast<AnimationTimeline*>(web_animation_timeline_create(JS::HostABI::vm_to_abi(JS::VM::the()), &document, &description));
}

GC::Ref<AnimationTimeline> AnimationTimeline::create_document_timeline(DOM::Document& document, double origin_time)
{
    return create(document, { .kind = RustFFI::FfiTimelineKind::Document, .origin_time = origin_time, .source = {}, .axis = 0, .inset = {} });
}

GC::Ref<AnimationTimeline> AnimationTimeline::create_scroll_timeline(DOM::Document& document, ScrollSource const& source, ScrollAxis axis)
{
    return create(document, { .kind = RustFFI::FfiTimelineKind::Scroll, .origin_time = 0, .source = source_to_ffi(source), .axis = to_underlying(axis), .inset = {} });
}

GC::Ref<AnimationTimeline> AnimationTimeline::create_view_timeline(DOM::Document& document, DOM::AbstractElement subject, ScrollAxis axis, ViewTimelineInset const& inset)
{
    return create(document,
        {
            .kind = RustFFI::FfiTimelineKind::View,
            .origin_time = 0,
            .source = source_to_ffi(AnonymousScrollSource { .scroller = CSS::Scroller::Nearest, .target = subject }),
            .axis = to_underlying(axis),
            .inset = inset_to_ffi(inset),
        });
}

GC::Ref<AnimationTimeline> AnimationTimeline::create_internal_timeline(DOM::Document& document)
{
    return create(document, { .kind = RustFFI::FfiTimelineKind::Internal, .origin_time = 0, .source = {}, .axis = 0, .inset = {} });
}

RustFFI::TimelineCell const& AnimationTimeline::record() const
{
    return *web_animation_timeline_record(const_cast<AnimationTimeline*>(this));
}

GC::Ref<DOM::Document> AnimationTimeline::associated_document() const
{
    return *static_cast<DOM::Document*>(RustFFI::animations_timeline_document(&record()));
}

bool AnimationTimeline::is_scroll_timeline() const
{
    auto kind = read().kind;
    return kind == RustFFI::FfiTimelineKind::Scroll || kind == RustFFI::FfiTimelineKind::View;
}

AnimationTimeline::CurrentTimeOverrideScope::CurrentTimeOverrideScope(AnimationTimeline& timeline, Optional<TimeValue> current_time)
    : m_timeline(timeline)
{
    auto read = timeline.read();
    m_had_previous_current_time_override = read.has_style_sampling_override;
    m_previous_current_time_override = read.style_sampling_override;
    m_timeline->set_current_time_override_for_style_sampling(move(current_time));
}

AnimationTimeline::CurrentTimeOverrideScope::~CurrentTimeOverrideScope()
{
    RustFFI::animations_timeline_set_style_sampling_override(&m_timeline->record(), m_had_previous_current_time_override, m_previous_current_time_override);
}

// https://drafts.csswg.org/web-animations-1/#dom-animationtimeline-currenttime
Optional<TimeValue> AnimationTimeline::current_time() const
{
    // NB: The record returns the current time for this timeline or null if this timeline is inactive.
    return from_ffi(read().sample.current_time);
}

u64 AnimationTimeline::task_generation() const
{
    return associated_document()->relevant_settings_object().responsible_event_loop().task_generation();
}

Optional<TimeValue> AnimationTimeline::current_time_for_observation()
{
    auto read = this->read();
    if (!read.sample.is_monotonically_increasing || !read.can_sample_current_time_at_timestamp)
        return from_ffi(read.sample.current_time);

    bool has_animation_without_per_frame_tick = false;
    for (auto const& animation : associated_animations()) {
        auto effect = animation.effect();
        if (!effect.has_value())
            continue;
        if (effect->per_frame_animation_tick_was_skipped() || effect->can_skip_per_frame_animation_tick()) {
            has_animation_without_per_frame_tick = true;
            break;
        }
    }
    if (!has_animation_without_per_frame_tick)
        return from_ffi(read.sample.current_time);

    auto& settings = associated_document()->relevant_settings_object();
    auto task_generation = settings.responsible_event_loop().task_generation();
    if (read.has_observation_task_generation && read.observation_task_generation == task_generation)
        return from_ffi(read.observed_current_time);

    // OPTIMIZATION: An animation skipped by the per-frame sampler does not keep the rendering update loop alive.
    //               Sample document timelines only when script observes them, and at most once per event-loop task
    //               so all reads within a stable state agree.
    auto timestamp = HighResolutionTime::relative_high_resolution_time(
        HighResolutionTime::unsafe_shared_current_time(), settings.global_object());
    auto observed_current_time = RustFFI::animations_timeline_current_time_at_timestamp(&record(), timestamp, true, task_generation);
    HTML::note_rendering_trace_event("animation timeline synchronized"sv);
    return from_ffi(observed_current_time);
}

bool AnimationTimeline::was_observed_ahead_in_current_task() const
{
    auto read = this->read();
    return read.was_observed_ahead && read.observed_ahead_task_generation == task_generation();
}

Optional<TimeValue> AnimationTimeline::current_time_at_timestamp(double timestamp) const
{
    return from_ffi(RustFFI::animations_timeline_current_time_at_timestamp(&record(), timestamp, false, 0));
}

void AnimationTimeline::update_current_time(double timestamp)
{
    // https://drafts.csswg.org/scroll-animations-1/#ref-for-dom-animationtimeline-currenttime
    // NB: The record calculates the scroll progress from the layout of the scroller. The passed timestamp is ignored.
    if (is_scroll_timeline()) {
        // NB: Called during animation timeline update, which runs before layout is up to date.
        update_current_time_from_layout(false);
        return;
    }
    RustFFI::animations_timeline_update_current_time(&record(), timestamp, nullptr, task_generation(), false, update_associated_animations_of);
}

// https://drafts.csswg.org/scroll-animations-1/#stale-timelines
bool AnimationTimeline::update_current_time_if_stale()
{
    return update_current_time_from_layout(true);
}

void AnimationTimeline::update_associated_animations_of(void* timeline)
{
    static_cast<AnimationTimeline*>(timeline)->update_associated_animations();
}

void AnimationTimeline::update_associated_animations()
{
    // https://drafts.csswg.org/web-animations-1/#animation-frame-loop
    // Note: Due to the hierarchical nature of the timing model, updating the current time of a timeline also involves:
    // - Updating the current time of any animations associated with the timeline.
    // - Running the update an animation's finished state procedure for any animations whose current time has been
    //   updated.
    // - Queueing animation events for any such animations.
    // NB: Since we dispatch events for all animations regardless of whether they have a timeline we handle them all together in Document::update_animations_and_send_events()
    HTML::note_rendering_trace_event("animation timeline updated its animations"sv);
    // NB: Updating an animation can run script that changes which animations are associated with this timeline. Skip
    //     the ones it disassociates. The animations stay alive through it.
    GC::RootVector<AnimationRef> animations;
    auto associated_animations = this->associated_animations();
    for (auto const& animation : associated_animations)
        animations.append(animation);
    for (auto const& animation : animations) {
        if (associated_animations.contains(animation.record()))
            animation.update();
    }
}

// https://www.w3.org/TR/web-animations-1/#timeline-time-to-origin-relative-time
Optional<double> AnimationTimeline::convert_a_timeline_time_to_an_origin_relative_time(Optional<TimeValue> timeline_time)
{
    return from_ffi(RustFFI::animations_timeline_convert_to_origin_relative_time(&record(), to_ffi(timeline_time)));
}

// https://drafts.csswg.org/web-animations-2/#timeline-duration
Optional<TimeValue> AnimationTimeline::duration() const
{
    return from_ffi(read().duration);
}

AnimationTimeline::ScrollSource AnimationTimeline::scroll_source() const
{
    auto source = read().source;
    if (!source.is_anonymous)
        return GC::Ptr<DOM::Element const> { static_cast<DOM::Element const*>(source.element) };
    Optional<CSS::PseudoElement> pseudo_element;
    if (source.has_pseudo_element)
        pseudo_element = static_cast<CSS::PseudoElement>(source.pseudo_element);
    return AnonymousScrollSource {
        .scroller = static_cast<CSS::Scroller>(source.scroller),
        .target = DOM::AbstractElement { *static_cast<DOM::Element*>(source.target), pseudo_element },
    };
}

GC::Ptr<DOM::Element const> AnimationTimeline::source() const
{
    return scroll_source().visit(
        [](GC::Ptr<DOM::Element const> const& source) -> GC::Ptr<DOM::Element const> {
            return source;
        },
        [](AnonymousScrollSource const& anonymous_source) -> GC::Ptr<DOM::Element const> {
            switch (anonymous_source.scroller) {
            case CSS::Scroller::Root:
                return anonymous_source.target.document().document_element();
            case CSS::Scroller::Nearest: {
                GC::Ptr<DOM::Element const> ancestor = anonymous_source.target.parent_element();

                // Whether a box is a scroll container depends on its used overflow, which is visible for a body whose
                // overflow is propagated to the viewport. The document element stands for the viewport.
                Layout::ForcedReadScope read { anonymous_source.target.document() };
                while (ancestor && !ancestor->is_document_element()) {
                    auto const* layout_node = ancestor->unsafe_layout_node(read);
                    if (layout_node && layout_node->is_scroll_container())
                        break;
                    ancestor = ancestor->parent_element();
                }

                return ancestor;
            }
            case CSS::Scroller::Self:
                return anonymous_source.target.element();
            }
            VERIFY_NOT_REACHED();
        });
}

void AnimationTimeline::set_axis(ScrollAxis axis)
{
    if (RustFFI::animations_scroll_timeline_set_axis_and_inset(&record(), to_underlying(axis), nullptr))
        update_current_time(0);
}

DOM::AbstractElement AnimationTimeline::view_subject() const
{
    return scroll_source().get<AnonymousScrollSource>().target;
}

ViewTimelineInset AnimationTimeline::view_inset() const
{
    auto inset = read().inset;
    auto edge = [](RustFFI::FfiViewTimelineInsetEdge const& edge) -> ViewTimelineInsetEdge {
        return { edge.is_auto, edge.px, edge.percentage, retain_calculated_inset(edge.calculated) };
    };
    return { edge(inset.start), edge(inset.end) };
}

void AnimationTimeline::set_axis_and_inset(ScrollAxis axis, ViewTimelineInset const& inset)
{
    auto ffi_inset = inset_to_ffi(inset);
    if (RustFFI::animations_scroll_timeline_set_axis_and_inset(&record(), to_underlying(axis), &ffi_inset))
        update_current_time(0);
}

void AnimationTimeline::set_internal_time(Optional<double> time, bool for_observation)
{
    RustFFI::animations_internal_timeline_set_time(&record(), to_ffi(time), for_observation, task_generation(), update_associated_animations_of);
}

bool AnimationTimeline::update_current_time_from_layout(bool only_if_stale)
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
    auto update = [&] {
        return RustFFI::animations_timeline_update_current_time(&record(), 0, &boxes, task_generation(), only_if_stale, update_associated_animations_of);
    };

    // https://drafts.csswg.org/scroll-animations-1/#scroll-notation
    // References to the root element propagate to the document viewport (which functions as its scroll container).
    auto const source = this->source();
    Variant<GC::Ptr<DOM::Element const>, GC::Ptr<DOM::Document>> propagated_source = source;
    if (source && source.ptr() == source->document().document_element())
        propagated_source = source->owner_document();
    if (propagated_source.visit([](auto const& source) { return source == nullptr; }))
        return update();

    Layout::ForcedReadScope read { propagated_source.visit([](auto const& source) -> DOM::Document const& { return source->document(); }) };
    auto const* layout_node = propagated_source.visit([&read](auto const& source) -> Layout::NodeWithStyle const* { return source->unsafe_layout_node(read); });
    if (!layout_node)
        return update();

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
    if (is_view_timeline()) {
        auto subject = view_subject();
        boxes.subject = Layout::Node::slot_id(subject.unsafe_layout_node(read));
        // NB: A clock tick finds the subject by its style node, which a pseudo-element does not name.
        if (!subject.pseudo_element().has_value())
            boxes.subject_style_node = subject.element().style_node_id().value();
    }
    return update();
}

ScrollAxis scroll_axis_from_css_axis(CSS::Axis axis)
{
    switch (axis) {
    case CSS::Axis::Block:
        return ScrollAxis::Block;
    case CSS::Axis::Inline:
        return ScrollAxis::Inline;
    case CSS::Axis::X:
        return ScrollAxis::X;
    case CSS::Axis::Y:
        return ScrollAxis::Y;
    }

    VERIFY_NOT_REACHED();
}

// NB: What the Rust timelines ask of the host.
void web_animation_timeline_did_create(void* cell)
{
    auto& timeline = *static_cast<AnimationTimeline*>(cell);
    auto document = timeline.associated_document();
    document->associate_with_timeline(timeline);
    Optional<double> current_time;
    if (timeline.is_document_timeline()) {
        current_time = document->last_animation_frame_timestamp();
        if (!current_time.has_value()) {
            // The document hasn't processed an animation frame yet, we use the navigation start time, which is either
            // the time that the previous document started to be unloaded or the creation time of the current document.
            current_time = HighResolutionTime::relative_high_resolution_time(document->load_timing_info().navigation_start_time, document->relevant_settings_object().global_object());
        }
    } else if (timeline.is_scroll_timeline()) {
        // NB: The passed timestamp is ignored for ScrollTimelines so we can just pass 0 here.
        current_time = 0;
    }
    if (current_time.has_value())
        timeline.update_current_time(*current_time);
}

RustFFI::OptionalTimeValue web_animation_timeline_current_time_for_observation(void* cell)
{
    return to_ffi(static_cast<AnimationTimeline*>(cell)->current_time_for_observation());
}

DOM::Element* web_scroll_timeline_source(void* cell)
{
    auto& timeline = *static_cast<AnimationTimeline*>(cell);
    if (timeline.is_view_timeline()) {
        // The values of subject, source, and currentTime are all computed when any of them is requested or updated.
        auto subject = timeline.view_subject();
        subject.document().update_layout(DOM::UpdateLayoutReason::ViewTimelineSource);

        // If the source or subject of a ViewTimeline is an element whose principal box does not exist, [...] then the
        // ViewTimeline is inactive.
        // INTEROP: Its source is then null, as WebKit and Blink have it.
        for (GC::Ptr<DOM::Element const> element = &subject.element(); element; element = element->flat_tree_parent_element()) {
            if (!element->has_style() || element->installed_style().display_is_none())
                return nullptr;
        }
    } else {
        // The nearest scroll container is the nearest one by used overflow, which layout decides.
        auto scroll_source = timeline.scroll_source();
        if (auto const* anonymous_source = scroll_source.get_pointer<AnimationTimeline::AnonymousScrollSource>(); anonymous_source && anonymous_source->scroller == CSS::Scroller::Nearest)
            anonymous_source->target.document().update_layout(DOM::UpdateLayoutReason::ScrollTimelineSource);
    }
    return const_cast<DOM::Element*>(timeline.source().ptr());
}

DOM::Element* web_animation_document_scrolling_element(DOM::Document* document)
{
    return const_cast<DOM::Element*>(document->scrolling_element().ptr());
}

void web_internal_animation_timeline_set_time(void* cell, RustFFI::OptionalNumber time, bool for_observation)
{
    static_cast<AnimationTimeline*>(cell)->set_internal_time(from_ffi(time), for_observation);
}

// NB: Absolutizes a style value that script gave, as the document's values are.
CSS::StyleValueFFI::StyleValueData const* web_animation_absolutize_style_value(DOM::Document* document, CSS::StyleValueFFI::StyleValueData const* value)
{
    auto style_value = CSS::StyleValue::adopt_rust_style_value_data(CSS::StyleValueFFI::rust_style_value_retain(value));
    auto computed = style_value->absolutized(CSS::ComputationContext { .length_resolution_context = CSS::Length::ResolutionContext::for_document(*document) });
    return CSS::StyleValueFFI::rust_style_value_retain(computed->rust_style_value_data());
}

}

// NB: What the timeline records ask of the document a timeline is associated with.
extern "C" WEB_API bool ladybird_animations_document_is_active(void* document)
{
    return static_cast<Web::DOM::Document const*>(document)->is_active();
}

extern "C" WEB_API void* ladybird_animations_element_node_document(void* element)
{
    return &static_cast<Web::DOM::Element*>(element)->document();
}
