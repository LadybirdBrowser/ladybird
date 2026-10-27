/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Animations/ViewTimeline.h>
#include <LibWeb/CSS/CSSKeywordValue.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Painting/BoxViews.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(ViewTimeline);

ViewTimelineInset view_timeline_inset_from_ffi(CSS::ComputedValuesFFI::FfiViewTimelineInset const& inset)
{
    return {
        .start = { inset.start_is_auto, inset.start_px, inset.start_percentage, retain_calculated_offset(inset.start_calculated) },
        .end = { inset.end_is_auto, inset.end_px, inset.end_percentage, retain_calculated_offset(inset.end_calculated) },
    };
}

// The insets a computed 'view-timeline-inset' value describes.
static ViewTimelineInset view_timeline_inset_from_style_value(CSS::StyleValue const& value)
{
    return view_timeline_inset_from_ffi(CSS::ComputedValuesFFI::rust_view_timeline_inset(value.rust_style_value_data()));
}

GC::Ref<ViewTimeline> ViewTimeline::create(DOM::Document& document, DOM::AbstractElement subject, ScrollAxis axis, Inset inset)
{
    auto timeline = GC::Heap::the().allocate<ViewTimeline>(document, subject, axis, inset);

    // NB: The passed timestamp is ignored for ViewTimelines so we can just pass 0 here.
    timeline->update_current_time(0);

    return timeline;
}

// https://drafts.csswg.org/scroll-animations-1/#dom-viewtimeline-viewtimeline
WebIDL::ExceptionOr<GC::Ref<ViewTimeline>> ViewTimeline::create_for_constructor(JS::Object& relevant_global_object, Bindings::ViewTimelineOptions const& options)
{
    auto& document = HTML::relevant_window(relevant_global_object).associated_document();

    // 1. Let timeline be the new ViewTimeline object.
    // 2. Set the subject and axis properties of timeline to the corresponding values from options.
    // 3. Set the source of timeline to the subject’s nearest ancestor scroll container element that's scrollable in
    //    the specified axis.
    // NB: The source is found from the subject whenever it is asked for.

    // 4. If a DOMString value is provided as an inset, parse it as a <'view-timeline-inset'> value; if a sequence is
    //    provided, the first value represents the start inset and the second value represents the end inset. If the
    //    sequence has only one value, it is duplicated. If it has zero values or more than two values, or if it
    //    contains a CSSKeywordValue whose value is not "auto", throw a TypeError.
    // AD-HOC: Relative lengths are not allowed, see https://github.com/w3c/csswg-drafts/issues/13852
    auto parse_inset = [&](Utf16String const& string) -> WebIDL::ExceptionOr<Inset> {
        auto value = parse_css_value(CSS::Parser::ParsingParams { document }, string, CSS::PropertyID::ViewTimelineInset);
        // NB: Arbitrary substitution functions and CSS-wide keywords have nothing to substitute or inherit from here.
        if (!value || value->is_css_wide_keyword() || value->is_unresolved() || value->is_pending_substitution()
            || CSS::ComputedValuesFFI::rust_view_timeline_inset_has_relative_length(value->rust_style_value_data()))
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid view timeline inset"_utf16 };
        auto computed = value->absolutized(CSS::ComputationContext { .length_resolution_context = CSS::LengthResolutionContext::for_document(document) });
        return view_timeline_inset_from_style_value(*computed);
    };
    auto parse_auto = [&](Utf16View const& keyword) -> WebIDL::ExceptionOr<InsetEdge> {
        if (!keyword.equals_ignoring_ascii_case("auto"sv))
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "A view timeline inset keyword must be auto"_utf16 };
        return InsetEdge {};
    };
    auto inset = TRY(options.inset.visit(
        [&](Utf16String const& string) { return parse_inset(string); },
        [&](auto const& values) -> WebIDL::ExceptionOr<Inset> {
            if (values.is_empty() || values.size() > 2)
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "A view timeline inset takes one or two values"_utf16 };
            Vector<InsetEdge, 2> edges;
            for (auto const& value : values) {
                auto edge = TRY(value.visit(
                    [&](GC::Ref<CSS::CSSNumericValue> const& numeric) -> WebIDL::ExceptionOr<InsetEdge> {
                        return TRY(parse_inset(numeric->to_string({}))).start;
                    },
                    [&](GC::Ref<CSS::CSSKeywordValue> const& keyword) { return parse_auto(keyword->value()); },
                    // INTEROP: Blink takes "auto" as a string in the sequence too.
                    [&](Utf16String const& keyword) { return parse_auto(keyword); }));
                edges.append(edge);
            }
            return Inset { edges[0], edges.size() > 1 ? edges[1] : edges[0] };
        }));

    return create(document, DOM::AbstractElement { *options.subject }, options.axis, inset);
}

ViewTimeline::ViewTimeline(DOM::Document& document, DOM::AbstractElement subject, ScrollAxis axis, Inset inset)
    : ScrollTimeline(document, AnonymousSource { .scroller = CSS::Scroller::Nearest, .target = subject }, axis)
    , m_inset(inset)
{
}

DOM::AbstractElement ViewTimeline::subject_internal() const
{
    return source_internal().get<AnonymousSource>().target;
}

GC::Ref<DOM::Element const> ViewTimeline::subject() const
{
    return subject_internal().element();
}

GC::Ptr<DOM::Element const> ViewTimeline::source_for_bindings() const
{
    // The values of subject, source, and currentTime are all computed when any of them is requested or updated.
    auto subject = subject_internal();
    subject.document().update_style_for_element(subject, DOM::Document::StyleUpdateMode::OnlyIfNeeded);

    // If the source or subject of a ViewTimeline is an element whose principal box does not exist, [...] then the
    // ViewTimeline is inactive.
    // INTEROP: Its source is then null, as WebKit and Blink have it.
    for (GC::Ptr<DOM::Element const> element = &subject.element(); element; element = element->flat_tree_parent_element()) {
        if (!element->has_style() || element->installed_style().display_is_none())
            return nullptr;
    }
    return source();
}

void ViewTimeline::set_axis_and_inset(ScrollAxis axis, Inset inset)
{
    if (this->axis() == axis && m_inset == inset)
        return;
    m_inset = inset;
    if (this->axis() != axis)
        set_axis(axis);
    else
        update_current_time(0);
}

Optional<ViewTimeline::Metrics> ViewTimeline::compute_metrics() const
{
    // If the source or subject of a ViewTimeline is an element whose principal box does not exist, or if its nearest
    // ancestor scroll container has no scrollable overflow or is not scrollable in the specified axis (or if there is no
    // such ancestor, e.g. in print media), then the ViewTimeline is inactive.
    auto scroller = scroller_metrics();
    if (!scroller.has_value())
        return {};

    auto subject = subject_internal();
    Layout::ForcedReadScope read { subject.document() };
    auto const* subject_layout_node = subject.unsafe_layout_node(read);
    if (!subject_layout_node || !Painting::has_committed_box(*subject_layout_node))
        return {};

    // NB: The border box is laid out where the scroll container's scrolled content is at its scroll origin, with
    //     transforms ignored and relative positioning accounted for, as the view progress timeline ranges ask.
    auto subject_border_box = Painting::absolute_border_box_rect(*subject_layout_node);
    auto subject_start = (scroller->is_vertical ? subject_border_box.y() : subject_border_box.x()).to_double();
    auto subject_size = (scroller->is_vertical ? subject_border_box.height() : subject_border_box.width()).to_double();
    auto subject_offset = subject_start - scroller->scrollport_start;

    auto inset_of = [&](InsetEdge const& edge, double scroll_padding) {
        if (edge.is_auto)
            return scroll_padding;
        return edge.resolve(scroller->scrollport_size);
    };
    auto inset_start = inset_of(m_inset.start, scroller->scroll_padding_start);
    auto inset_end = inset_of(m_inset.end, scroller->scroll_padding_end);

    // https://drafts.csswg.org/scroll-animations-1/#valdef-animation-timeline-range-cover
    // 0% progress represents the latest position at which the start border edge of the element’s principal box
    // coincides with the end edge of its view progress visibility range.
    // 100% progress represents the earliest position at which the end border edge of the element’s principal box
    // coincides with the start edge of its view progress visibility range.
    return Metrics {
        .scroll_offset = scroller->scroll_offset,
        .max_scroll_offset = scroller->max_scroll_offset,
        .cover_start = subject_offset - (scroller->scrollport_size - inset_end),
        .cover_end = subject_offset + subject_size - inset_start,
        .subject_size = subject_size,
    };
}

bool ViewTimeline::is_stale() const
{
    // NB: See ScrollTimeline::is_stale(). A view progress timeline's named timeline ranges move with its subject too.
    return compute_metrics() != m_last_metrics;
}

void ViewTimeline::update_current_time(double)
{
    m_last_metrics = compute_metrics();

    if (!m_last_metrics.has_value()) {
        set_current_time({});
        return;
    }

    // https://drafts.csswg.org/scroll-animations-1/#view-timeline-progress
    // Progress (the current time) in a view progress timeline is calculated as: distance ÷ range where:
    // - distance is the current scroll offset minus the scroll offset corresponding to the start of the cover range
    // - range is the scroll offset corresponding to the end of the cover range minus the scroll offset corresponding
    //   to the start of the cover range
    auto distance = m_last_metrics->scroll_offset - m_last_metrics->cover_start;
    auto range = m_last_metrics->cover_end - m_last_metrics->cover_start;

    // If the 0% position and 100% position coincide (i.e. the denominator in the current time formula is zero), the
    // timeline is inactive.
    if (range == 0) {
        set_current_time({});
        return;
    }

    // FIXME: In paged media, view progress timelines that would otherwise reference the document viewport are also
    //        inactive.

    set_current_time(TimeValue { TimeValue::Type::Percentage, distance / range * 100 });
}

// https://drafts.csswg.org/scroll-animations-1/#view-timelines-ranges
ViewTimeline::ScrollOffsetInterval ViewTimeline::interval_for_range_name(Metrics const& metrics, TimelineRangeName name) const
{
    auto sorted = [](double start, double end) {
        if (end < start)
            swap(start, end);
        return ScrollOffsetInterval { start, end };
    };

    switch (name) {
    case TimelineRangeName::None:
    case TimelineRangeName::Cover:
        // Represents the full range of the view progress timeline.
        return { metrics.cover_start, metrics.cover_end };
    case TimelineRangeName::Contain:
        // Represents the range during which the principal box is either fully contained by, or fully covers, its view
        // progress visibility range within the scrollport.
        return sorted(metrics.cover_start + metrics.subject_size, metrics.cover_end - metrics.subject_size);
    case TimelineRangeName::Entry: {
        // 0% is equivalent to 0% of the cover range.
        // 100% is equivalent to 0% of the contain range.
        auto contain = interval_for_range_name(metrics, TimelineRangeName::Contain);
        return sorted(metrics.cover_start, contain.start);
    }
    case TimelineRangeName::Exit: {
        // 0% is equivalent to 100% of the contain range.
        // 100% is equivalent to 100% of the cover range.
        auto contain = interval_for_range_name(metrics, TimelineRangeName::Contain);
        return sorted(contain.end, metrics.cover_end);
    }
    case TimelineRangeName::EntryCrossing:
        // Represents the range during which the principal box crosses the end border edge.
        return sorted(metrics.cover_start, metrics.cover_start + metrics.subject_size);
    case TimelineRangeName::ExitCrossing:
        // Represents the range during which the principal box crosses the start border edge.
        return sorted(metrics.cover_end - metrics.subject_size, metrics.cover_end);
    case TimelineRangeName::Scroll:
        // Represents the full range of the scroll container on which the view progress timeline is defined.
        return { 0, metrics.max_scroll_offset };
    }
    VERIFY_NOT_REACHED();
}

AttachmentInterval ViewTimeline::interval_for_attachment_range(AnimationAttachmentRange const& range) const
{
    auto cover_length = m_last_metrics.has_value() ? m_last_metrics->cover_end - m_last_metrics->cover_start : 0;
    if (cover_length == 0)
        return ScrollTimeline::interval_for_attachment_range(range);

    // Each edge is measured from the start of its named timeline range, along that range, and is then placed as a
    // proportion of the cover range, which spans the entire timeline.
    auto time_of = [&](TimelineRangeEdge const& edge) {
        auto named_range = interval_for_range_name(*m_last_metrics, edge.name);
        auto position = named_range.start + edge.offset_along(named_range.end - named_range.start);
        return TimeValue { TimeValue::Type::Percentage, (position - m_last_metrics->cover_start) / cover_length * 100 };
    };
    return { time_of(range.start), time_of(range.end) };
}

// NB: The timeline times at the minimum and the maximum scroll offsets, which need not be 0% and 100%.
Optional<TimeValue> ViewTimeline::minimum_timeline_time() const
{
    if (!m_last_metrics.has_value() || m_last_metrics->cover_end == m_last_metrics->cover_start)
        return {};
    return TimeValue { TimeValue::Type::Percentage, (0 - m_last_metrics->cover_start) / (m_last_metrics->cover_end - m_last_metrics->cover_start) * 100 };
}

Optional<TimeValue> ViewTimeline::maximum_timeline_time() const
{
    if (!m_last_metrics.has_value() || m_last_metrics->cover_end == m_last_metrics->cover_start)
        return {};
    return TimeValue { TimeValue::Type::Percentage, (m_last_metrics->max_scroll_offset - m_last_metrics->cover_start) / (m_last_metrics->cover_end - m_last_metrics->cover_start) * 100 };
}

GC::Ref<CSS::CSSNumericValue> ViewTimeline::start_offset() const
{
    // Represents the starting (0% progress) scroll position of the view progress timeline as a length offset (in px)
    // from the scroll origin.
    return CSS::CSSUnitValue::create(m_last_metrics.has_value() ? m_last_metrics->cover_start : 0, "px"_utf16_fly_string);
}

GC::Ref<CSS::CSSNumericValue> ViewTimeline::end_offset() const
{
    // Represents the ending (100% progress) scroll position of the view progress timeline as a length offset (in px)
    // from the scroll origin.
    return CSS::CSSUnitValue::create(m_last_metrics.has_value() ? m_last_metrics->cover_end : 0, "px"_utf16_fly_string);
}

}
