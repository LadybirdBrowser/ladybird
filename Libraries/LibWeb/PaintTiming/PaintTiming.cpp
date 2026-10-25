/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/PaintTiming/PaintTiming.h>
#include <LibWeb/PaintTiming/PerformancePaintTiming.h>
#include <LibWeb/Painting/BoxViews.h>

namespace Web::PaintTiming {

static Utf16String const& first_paint_name()
{
    static NeverDestroyed<Utf16String> name { "first-paint"_utf16 };
    return *name;
}

static Utf16String const& first_contentful_paint_name()
{
    static NeverDestroyed<Utf16String> name { "first-contentful-paint"_utf16 };
    return *name;
}

// https://w3c.github.io/paint-timing/#non-empty
static bool is_non_empty(Utf16View const& string)
{
    // A DOMString is non-empty if it contains at least one character excluding document white space characters.
    for (auto code_unit : string) {
        if (code_unit != ' ' && code_unit != '\t' && code_unit != '\n' && code_unit != '\r' && code_unit != '\f')
            return true;
    }
    return false;
}

static RefPtr<CSS::StyleValue const> style_value_of(CSS::ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    if (!handle.pointer)
        return nullptr;
    return CSS::StyleValue::adopt_rust_style_value_data(CSS::StyleValueFFI::rust_style_value_retain(static_cast<CSS::StyleValueFFI::StyleValueData const*>(handle.pointer)));
}

static bool is_zero_length_or_percentage(CSS::StyleValue const& value)
{
    if (value.is_length())
        return value.as_length().raw_value() == 0;
    if (value.is_percentage())
        return value.as_percentage().raw_value() == 0;
    return false;
}

// Whether the used background-size of the given background layer has a zero width or height.
static bool background_size_is_empty(Layout::NodeWithStyle const& node, size_t layer_index)
{
    auto background_size = style_value_of(node.style_group<CSS::ComputedValues::BackgroundValues>().background_size);
    if (!background_size)
        return false;
    if (background_size->is_value_list()) {
        auto const& list = background_size->as_value_list();
        if (list.size() == 0)
            return false;
        background_size = list.value_at(layer_index, true);
    }
    if (!background_size->is_background_size())
        return false;
    auto const& size = background_size->as_background_size();
    return is_zero_length_or_percentage(size.size_x()) || is_zero_length_or_percentage(size.size_y());
}

// https://w3c.github.io/paint-timing/#contentful-image
static bool is_contentful_image(Layout::NodeWithStyle::ImageObserver const* observer)
{
    // A CSS image img is a contentful image when all of the following apply:
    // - img is url valued.
    // - img is available.
    // NB: Only url-valued images have an observer, which holds the decoded image data once it is available.
    return observer && observer->decoded_image_data();
}

static bool svg_has_rendered_descendants(Layout::Node const& svg_box)
{
    bool has_rendered_descendants = false;
    svg_box.for_each_in_inclusive_subtree([&](Layout::Node const& node) {
        if (&node == &svg_box)
            return TraversalDecision::Continue;
        switch (node.kind()) {
        case Layout::RustFFI::NodeKind::SVGGeometryBox:
        case Layout::RustFFI::NodeKind::SVGImageBox:
        case Layout::RustFFI::NodeKind::SVGTextBox:
        case Layout::RustFFI::NodeKind::SVGTextPathBox:
        case Layout::RustFFI::NodeKind::SVGForeignObjectBox:
            has_rendered_descendants = true;
            return TraversalDecision::Break;
        default:
            return TraversalDecision::Continue;
        }
    });
    return has_rendered_descendants;
}

// https://w3c.github.io/paint-timing/#contentful
// NB: This answers the question for the box of an element, or for a text node's box, which stands for the element
//     the text node is a child of. Generated content has boxes of its own, so a pseudo-element's content is found
//     the same way as an element's.
static bool is_contentful(Layout::Node const& node)
{
    // An element target is contentful when one or more of the following apply:

    // - target has a text node child, representing non-empty text, and the node's used opacity is greater than zero.
    // NB: The opacity of the text node's ancestors is checked as part of whether it is paintable.
    if (auto const* text_node = as_if<Layout::TextNode>(node)) {
        if (auto const* dom_text = text_node->dom_text())
            return is_non_empty(dom_text->data().utf16_view());
        // NB: Generated text has no DOM node, so its rendered text is looked at instead.
        auto rendered_text = text_node->rendered_text_for_dom();
        return is_non_empty(rendered_text.utf16_view());
    }

    auto const* node_with_style = as_if<Layout::NodeWithStyle>(node);
    if (!node_with_style)
        return false;

    auto const* dom_node = node.dom_node();

    switch (node.kind()) {
    // - target is a replaced element representing an available image.
    case Layout::RustFFI::NodeKind::ImageBox:
    case Layout::RustFFI::NodeKind::SVGImageBox: {
        auto const* image_provider = as<Layout::Box>(node).image_provider_if_any();
        if (image_provider && image_provider->is_image_available())
            return true;
        break;
    }
    // - target is a canvas with its context mode set to any value other than none.
    case Layout::RustFFI::NodeKind::CanvasBox:
        if (auto const* canvas = as_if<HTML::HTMLCanvasElement>(dom_node); canvas && !canvas->context().has<Empty>())
            return true;
        break;
    // - target is a video element that represents its poster frame or the first video frame and the frame is
    //   available.
    case Layout::RustFFI::NodeKind::VideoBox:
        if (auto const* video = as_if<HTML::HTMLVideoElement>(dom_node)) {
            auto representation = video->current_representation();
            if (representation == HTML::HTMLVideoElement::Representation::PosterFrame && video->poster_frame().has_value())
                return true;
            if (representation == HTML::HTMLVideoElement::Representation::VideoFrame || representation == HTML::HTMLVideoElement::Representation::FirstVideoFrame)
                return true;
        }
        break;
    // - target is an svg element with rendered descendants.
    case Layout::RustFFI::NodeKind::SVGSVGBox:
        if (svg_has_rendered_descendants(node))
            return true;
        break;
    // - target is an input element with a non-empty value attribute.
    // NB: The value is what the control renders, which the value attribute only provides the default for.
    case Layout::RustFFI::NodeKind::TextInputBox:
        if (auto const* input = as_if<HTML::HTMLInputElement>(dom_node)) {
            auto value = input->value();
            if (is_non_empty(value.utf16_view()))
                return true;
        }
        break;
    default:
        break;
    }

    // - target has a background-image which is a contentful image, and its used background-size has non-zero width
    //   and height values.
    auto background_layer_count = node_with_style->background_images().size();
    for (size_t layer_index = 0; layer_index < background_layer_count; ++layer_index) {
        if (is_contentful_image(node_with_style->background_image_observer(layer_index)) && !background_size_is_empty(*node_with_style, layer_index))
            return true;
    }

    // - target has a border-image-source which is a contentful image, and its used border-image-width has non-zero
    //   value.
    // FIXME: Check the used border-image-width.
    if (is_contentful_image(node_with_style->border_image_source_observer()))
        return true;

    // - target is an originating element for a paintable pseudo-element that represents a contentful image or
    //   non-empty text.
    // NB: The boxes of a pseudo-element's generated content are checked like those of any element.

    return false;
}

// AD-HOC: Whether the box paints something other than the default background, which is what first paint is about.
static bool paints_non_default_content(Layout::Node const& node)
{
    if (is_contentful(node))
        return true;

    auto const* node_with_style = as_if<Layout::NodeWithStyle>(node);
    if (!node_with_style)
        return false;

    // NB: The enclosing box of a paintable iframe counts towards first paint.
    if (node.kind() == Layout::RustFFI::NodeKind::NavigableContainerViewport)
        return true;

    if (node_with_style->background_color().alpha() > 0)
        return true;

    for (auto const& background_image : node_with_style->background_images()) {
        if (background_image)
            return true;
    }

    auto border_is_painted = [](CSS::BorderData const& border) {
        return border.width > 0 && border.line_style != CSS::LineStyle::None && border.line_style != CSS::LineStyle::Hidden && border.color.alpha() > 0;
    };
    return border_is_painted(node_with_style->border_left()) || border_is_painted(node_with_style->border_top()) || border_is_painted(node_with_style->border_right()) || border_is_painted(node_with_style->border_bottom());
}

static CSSPixelRect client_rect_of_box(Layout::Node const& node, Layout::RustFFI::FfiRectToViewportTransform const& rect_to_viewport_transform)
{
    if (!node.is_text_node())
        return Painting::bounding_client_rect(node, rect_to_viewport_transform);

    // NB: The client rect of a text box is the union of the rects of its fragments.
    CSSPixelRect united_rect;
    Layout::RustFFI::layout_script_text_range_rects(
        node.document_host(), Layout::Node::slot_id(&node),
        to_underlying(Painting::SelectionState::Full), 0, 0, 0, NumericLimits<size_t>::max(),
        rect_to_viewport_transform, &united_rect, [](void* context, CSSPixelRect rect) {
            auto& united_rect = *static_cast<CSSPixelRect*>(context);
            if (rect.is_empty())
                return;
            united_rect = united_rect.is_empty() ? rect : united_rect.united(rect);
        });
    return united_rect;
}

// NB: This answers whether boxes of the document are paintable, from the state the document has after layout.
class PaintableChecker {
public:
    explicit PaintableChecker(DOM::Document& document)
        : m_read(document)
    {
        auto navigable = document.navigable();
        if (!navigable)
            return;
        m_viewport = document.layout_node(m_read);
        if (!m_viewport || !Painting::has_committed_box(*m_viewport)) {
            m_viewport = nullptr;
            return;
        }

        // NB: The scrolling area is in the coordinate space of the document, while client rects are relative to the
        //     viewport, so client rects are offset by the viewport's scroll offset before they are compared with it.
        m_scrolling_area = Painting::scrollable_overflow_rect(*m_viewport);
        m_scroll_offset = navigable->viewport_scroll_offset();

        document.update_paint_and_hit_testing_properties_if_needed();
        m_rect_to_viewport_transform = Painting::rect_to_viewport_transform(document, document.visual_context_tree());
    }

    Layout::Viewport const* viewport() const { return m_scrolling_area.has_value() ? m_viewport : nullptr; }

    // https://w3c.github.io/paint-timing/#paintable
    bool is_paintable(Layout::Node const& node) const
    {
        if (!viewport())
            return false;

        // An element el is paintable when all of the following apply:
        // - el is being rendered.
        // NB: Only boxes are looked at.

        // - el's used visibility is visible.
        auto const* node_with_style = as_if<Layout::NodeWithStyle>(node);
        if (!node_with_style)
            node_with_style = as_if<Layout::NodeWithStyle>(node.parent_ptr());
        if (!node_with_style || node_with_style->visibility() != CSS::Visibility::Visible)
            return false;

        // - el and all of its ancestors' used opacity is greater than zero.
        // NB: The subtrees of boxes with an opacity of zero are not looked at.

        // - el's paintable bounding rect intersects with the scrolling area of the document.
        // https://w3c.github.io/paint-timing/#paintable-bounding-rect
        // 1. Let boundingRect be the result of running the getBoundingClientRect() on target.
        auto bounding_rect = client_rect_of_box(node, m_rect_to_viewport_transform).translated(m_scroll_offset);

        // 2. Clip boundingRect with the document's scrolling area.
        // 3. Return boundingRect.
        // NB: The clipped rect is not empty only if the rect intersects with the scrolling area.
        return bounding_rect.intersects(*m_scrolling_area);
    }

private:
    Layout::ForcedReadScope m_read;
    Layout::Viewport const* m_viewport { nullptr };
    Optional<CSSPixelRect> m_scrolling_area;
    CSSPixelPoint m_scroll_offset;
    Layout::RustFFI::FfiRectToViewportTransform m_rect_to_viewport_transform { Painting::identity_rect_to_viewport_transform() };
};

struct PaintedContent {
    bool has_non_default_content { false };
    bool has_contentful_content { false };
};

// NB: This finds whether the document has an element that is both paintable and contentful, and whether it paints
//     anything other than the default background, from the boxes it has after layout. Only boxes are looked at, so
//     elements that are not being rendered are skipped, and a box with an opacity of zero hides its whole subtree.
static PaintedContent find_painted_content(PaintableChecker const& checker, PaintedContent const& already_found)
{
    PaintedContent result = already_found;

    auto const* viewport = checker.viewport();
    if (!viewport)
        return result;

    viewport->for_each_in_inclusive_subtree([&](Layout::Node const& node) {
        if (auto const* node_with_style = as_if<Layout::NodeWithStyle>(node); node_with_style && node_with_style->opacity() <= 0)
            return TraversalDecision::SkipChildrenAndContinue;

        if (!result.has_non_default_content && paints_non_default_content(node) && checker.is_paintable(node))
            result.has_non_default_content = true;

        if (is_contentful(node) && checker.is_paintable(node)) {
            result.has_non_default_content = true;
            result.has_contentful_content = true;
            return TraversalDecision::Break;
        }

        // NB: The rendered descendants of an svg element make the svg element contentful, so they are not looked
        //     at by themselves.
        if (node.kind() == Layout::RustFFI::NodeKind::SVGSVGBox)
            return TraversalDecision::SkipChildrenAndContinue;

        return TraversalDecision::Continue;
    });

    return result;
}

// https://w3c.github.io/paint-timing/#mark-paint-timing
void mark_paint_timing(DOM::Document& document)
{
    // 1. If the document's browsing context is not paint-timing eligible, return.
    // NB: We configure every nested browsing context to report paint timing, so every browsing context is eligible.
    if (!document.browsing_context())
        return;

    auto& reported_paints = document.paint_timing_state().set_of_previously_reported_paints;

    // OPTIMIZATION: Nothing below reports anything once first paint and first contentful paint have been reported.
    if (reported_paints.contains(first_paint_name()) && reported_paints.contains(first_contentful_paint_name()))
        return;

    // AD-HOC: Nothing is painted for a document whose initial paint is deferred until its fonts load.
    if (document.font_computer().should_defer_initial_paint())
        return;

    PaintableChecker checker { document };

    // 2. Let paintTimingInfo be a new paint timing info, whose rendering update end time is the current high
    //    resolution time given document's relevant global object.
    PaintTimingInfo paint_timing_info {
        .rendering_update_end_time = HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(document)),
        .implementation_defined_presentation_time = {},
    };

    // FIXME: 3. Let paintedImages be a new ordered set
    // FIXME: 4. Let paintedTextNodes be a new ordered set
    // FIXME: 5. For each record in doc's images pending rendering list:
    //           1. If record's request is available and ready to be painted, then run the following steps:
    //              1. Append record to paintedImages.
    //              2. Remove record from doc's images pending rendering list.
    // FIXME: 6. For each Element element in doc's descendants:
    //           1. If element is contained in doc's set of elements with rendered text, continue.
    //           2. If element's set of owned text nodes is empty, continue.
    //           3. Append element to doc's set of elements with rendered text.
    //           4. Append element to paintedTextNodes.

    // 7. Let reportedPaints be the document's set of previously reported paints.
    // FIXME: 8. Let frameTimingInfo be document's current frame timing info.
    // FIXME: 9. Set document's current frame timing info to null.

    // 10. Let flushPaintTimings be the following steps:
    auto flush_paint_timings = [&] {
        // NB: The boxes are looked at once for both first paint and first contentful paint.
        PaintedContent already_found {
            .has_non_default_content = reported_paints.contains(first_paint_name()),
            .has_contentful_content = reported_paints.contains(first_contentful_paint_name()),
        };
        auto painted_content = find_painted_content(checker, already_found);

        // 1. If reportedPaints does not contain "first-paint", and the user agent is configured to mark first paint,
        //    then report paint timing given document, "first-paint", and paintTimingInfo.
        // NOTE: First paint excludes the default background paint, but includes non-default background paint.
        if (!reported_paints.contains(first_paint_name()) && painted_content.has_non_default_content)
            report_paint_timing(document, first_paint_name(), paint_timing_info);

        // 2. If document should report first contentful paint, then:
        // https://w3c.github.io/paint-timing/#should-report-first-contentful-paint
        //   1. If document's set of previously reported paints contains "first-contentful-paint", then return false.
        //   2. If document contains at least one element that is both paintable and contentful, then return true.
        //   3. Otherwise, return false.
        if (!reported_paints.contains(first_contentful_paint_name()) && painted_content.has_contentful_content) {
            // 1. Report paint timing given document, "first-contentful-paint", and paintTimingInfo.
            report_paint_timing(document, first_contentful_paint_name(), paint_timing_info);
        }

        // FIXME: 3. Report largest contentful paint given document, paintTimingInfo, paintedImages and
        //           paintedTextNodes.

        // FIXME: 4. Report element timing given document, paintTimingInfo, paintedImages and paintedTextNodes.
        // FIXME: 5. If frameTimingInfo is not null, then queue a long animation frame entry given document,
        //           frameTimingInfo, and paintTimingInfo.
    };

    // 11. If the user-agent does not support implementation-defined presentation times, call flushPaintTimings and
    //     return.
    // NB: The frame is presented by another process, which does not report back when it was presented.
    flush_paint_timings();
}

// https://w3c.github.io/paint-timing/#report-paint-timing
void report_paint_timing(DOM::Document& document, Utf16String const& paint_type, PaintTimingInfo const& paint_timing_info)
{
    // 1. Create a new PerformancePaintTiming object newEntry with document's relevant realm and set its attributes as
    //    follows:
    //    1. Set newEntry's name attribute to paintType.
    //    2. Set newEntry's entryType attribute to "paint".
    //    3. Set newEntry's startTime attribute to the default paint timestamp given paintTimingInfo.
    //    4. Set newEntry's duration attribute to 0.
    // 2. Set newEntry's paint timing info to paintTimingInfo.
    auto new_entry = PerformancePaintTiming::create(paint_type, paint_timing_info);

    // 3. Queue newEntry in document's relevant realm.
    auto& window = HTML::relevant_window(document);
    window.queue_performance_entry(new_entry);
    window.add_performance_entry(new_entry);

    // 4. Append paintType to document's set of previously reported paints.
    document.paint_timing_state().set_of_previously_reported_paints.set(paint_type);
}

// https://w3c.github.io/paint-timing/#exposed-for-paint-timing
bool is_exposed_for_paint_timing(DOM::Element const& element, GC::Ptr<DOM::Document const> document)
{
    // 1. If element is not connected, return false.
    if (!element.is_connected())
        return false;

    // 2. If document is null, let document be element's relevant settings object's relevant global object's
    //    associated document.
    if (!document)
        document = HTML::relevant_window(element).associated_document();

    // 3. If document is not fully active, return false.
    if (!document->is_fully_active())
        return false;

    // 4. If element's root is not equal to document, return false.
    if (&element.root() != document.ptr())
        return false;

    // 5. Return true.
    return true;
}

}
