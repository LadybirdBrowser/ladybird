/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ScrollTimeline.h"
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(ScrollTimeline);

GC::Ref<ScrollTimeline> ScrollTimeline::create(JS::Realm& realm, DOM::Document& document, Source source, Bindings::ScrollAxis axis)
{
    auto timeline = realm.create<ScrollTimeline>(realm, document, source, axis);

    // NB: The passed timestamp is ignored for ScrollTimelines so we can just pass 0 here.
    timeline->update_current_time(0);

    return timeline;
}

// https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-scrolltimeline
GC::Ref<ScrollTimeline> ScrollTimeline::construct_impl(JS::Realm& realm, ScrollTimelineOptions options)
{
    auto& document = as<HTML::Window>(realm.global_object()).associated_document();

    // 1. Let timeline be the new ScrollTimeline object.
    // 2. Set the source of timeline to:
    auto source = [&]() -> GC::Ptr<DOM::Element const> {
        // If the source member of options is present,
        // The source member of options.
        if (options.source.has_value())
            return options.source.value();

        // Otherwise,
        // The scrollingElement of the Document associated with the Window that is the current global object.
        if (document.scrolling_element())
            return document.scrolling_element();

        return nullptr;
    }();

    // 3. Set the axis property of timeline to the corresponding value from options.
    return create(realm, document, source, options.axis);
}

GC::Ptr<DOM::Element const> ScrollTimeline::source() const
{
    return m_source.visit(
        [](GC::Ptr<DOM::Element const> const& source) -> GC::Ptr<DOM::Element const> {
            return source;
        },
        [](AnonymousSource const& anonymous_source) -> GC::Ptr<DOM::Element const> {
            switch (anonymous_source.scroller) {
            case CSS::Scroller::Root:
                return anonymous_source.target.document().document_element();
            case CSS::Scroller::Nearest: {
                GC::Ptr<DOM::Element const> ancestor = anonymous_source.target.parent_element();

                while (ancestor && !ancestor->is_scroll_container())
                    ancestor = ancestor->parent_element();

                return ancestor;
            }
            case CSS::Scroller::Self:
                return anonymous_source.target.element();
            }
            VERIFY_NOT_REACHED();
        });
}

struct ComputedScrollAxis {
    bool is_vertical;
    bool is_reversed;
};
static ComputedScrollAxis computed_scroll_axis(Bindings::ScrollAxis axis, CSS::WritingMode writing_mode, CSS::Direction direction)
{
    // NB: This is based on the table specified here: https://drafts.csswg.org/css-writing-modes-4/#logical-to-physical

    // FIXME: Note: The used direction depends on the computed writing-mode and text-orientation: in vertical writing
    //              modes, a text-orientation value of upright forces the used direction to ltr.
    auto used_direction = direction;

    switch (axis) {
    case Bindings::ScrollAxis::Block:
        switch (writing_mode) {
        case CSS::WritingMode::HorizontalTb:
            return { true, false };
        case CSS::WritingMode::VerticalRl:
        case CSS::WritingMode::SidewaysRl:
            return { false, true };
        case CSS::WritingMode::VerticalLr:
        case CSS::WritingMode::SidewaysLr:
            return { false, false };
        }
        VERIFY_NOT_REACHED();
    case Bindings::ScrollAxis::Inline:
        switch (writing_mode) {
        case CSS::WritingMode::HorizontalTb:
            return { false, used_direction == CSS::Direction::Rtl };
        case CSS::WritingMode::VerticalRl:
        case CSS::WritingMode::SidewaysRl:
        case CSS::WritingMode::VerticalLr:
            return { true, used_direction == CSS::Direction::Rtl };
        case CSS::WritingMode::SidewaysLr:
            return { true, used_direction == CSS::Direction::Ltr };
        }
        VERIFY_NOT_REACHED();
    case Bindings::ScrollAxis::X:
        return { false, false };
    case Bindings::ScrollAxis::Y:
        return { true, false };
    }

    VERIFY_NOT_REACHED();
}

void ScrollTimeline::update_current_time(double)
{
    // https://drafts.csswg.org/scroll-animations-1/#ref-for-dom-animationtimeline-currenttime
    // currentTime represents the scroll progress of the scroll container as a percentage CSSUnitValue, with 0%
    // representing its startmost scroll position (in the writing mode of the scroll container). Null when the timeline
    // is inactive.

    // NB: We set the current time to null at the start of this so we can easily just return when the timeline should be
    //     inactive, only setting it to a resolved value if the timeline is active.
    set_current_time({});

    auto propagated_source = get_propagated_source();

    if (propagated_source.visit([](auto const& source) { return source == nullptr; }))
        return;

    // If the source of a ScrollTimeline is an element whose principal box does not exist or is not a scroll container,
    // or if there is no scrollable overflow, then the ScrollTimeline is inactive.
    // NB: Called during animation timeline update, which runs before layout is up to date.
    auto const& layout_node = propagated_source.visit([](auto const& source) -> Layout::NodeWithStyle const* { return source->unsafe_layout_node(); });

    if (!layout_node || !layout_node->is_scroll_container())
        return;

    auto const& paintable_box = propagated_source.visit([](auto const& source) -> Painting::PaintableBox const* { return source->unsafe_paintable_box(); });

    if (!paintable_box || !paintable_box->has_scrollable_overflow())
        return;

    auto const& scrollable_overflow_rect = paintable_box->scrollable_overflow_rect().value();
    auto const& computed_axis = computed_scroll_axis(m_axis, paintable_box->computed_values().writing_mode(), paintable_box->computed_values().direction());

    // https://drafts.csswg.org/scroll-animations-1/#scroll-timeline-progress
    // If the 0% position and 100% position coincide (i.e. the denominator in the current time formula is zero), the timeline is inactive.
    if ((computed_axis.is_vertical && scrollable_overflow_rect.height() == paintable_box->content_height()) || (!computed_axis.is_vertical && scrollable_overflow_rect.width() == paintable_box->content_width()))
        return;

    // FIXME: In paged media, scroll progress timelines that would otherwise reference the document viewport are also inactive.

    // https://drafts.csswg.org/scroll-animations-1/#scroll-timeline-progress
    // Progress (the current time) for a scroll progress timeline is calculated as:
    //     scroll offset ÷ (scrollable overflow size − scroll container size)
    // FIXME: Scroll offset is currently incorrect as it is always relative to the top left of the scrollable overflow
    //        rect when it should instead be relative to the scroll origin.
    auto progress = computed_axis.is_vertical
        ? paintable_box->scroll_offset().y().to_double() / (scrollable_overflow_rect.height().to_double() - paintable_box->content_height().to_double())
        : paintable_box->scroll_offset().x().to_double() / (scrollable_overflow_rect.width().to_double() - paintable_box->content_width().to_double());

    // FIXME: Support the case where the computed scroll axis is reversed

    set_current_time(TimeValue { TimeValue::Type::Percentage, progress * 100 });
}

ScrollTimeline::ScrollTimeline(JS::Realm& realm, DOM::Document& document, Source source, Bindings::ScrollAxis axis)
    : AnimationTimeline(realm)
    , m_source(source)
    , m_axis(axis)
{
    set_associated_document(document);
}

void ScrollTimeline::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_source.visit(
        [&](GC::Ptr<DOM::Element const>& source) { visitor.visit(source); },
        [&](AnonymousSource& anonymous_source) { anonymous_source.target.visit(visitor); });
}

void ScrollTimeline::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ScrollTimeline);
    Base::initialize(realm);
}

Variant<GC::Ptr<DOM::Element const>, GC::Ptr<DOM::Document>> ScrollTimeline::get_propagated_source() const
{
    auto const& source = this->source();

    // https://drafts.csswg.org/scroll-animations-1/#scroll-notation
    // References to the root element propagate to the document viewport (which functions as its scroll container).
    if (source && source == source->document().document_element())
        return source->owner_document();

    return source;
}

Bindings::ScrollAxis css_axis_to_bindings_scroll_axis(CSS::Axis axis)
{
    switch (axis) {
    case CSS::Axis::Block:
        return Bindings::ScrollAxis::Block;
    case CSS::Axis::Inline:
        return Bindings::ScrollAxis::Inline;
    case CSS::Axis::X:
        return Bindings::ScrollAxis::X;
    case CSS::Axis::Y:
        return Bindings::ScrollAxis::Y;
    }

    VERIFY_NOT_REACHED();
}

}
