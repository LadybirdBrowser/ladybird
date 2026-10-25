/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/LargestContentfulPaint/LargestContentfulPaint.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/PaintTiming/PaintTiming.h>
#include <LibWeb/Painting/BoxModelMetrics.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>

namespace Web::LargestContentfulPaint {

GC_DEFINE_ALLOCATOR(LargestContentfulPaint);

GC::Ref<LargestContentfulPaint> LargestContentfulPaint::create(PaintTiming::PaintTimingInfo paint_timing_info, WebIDL::UnsignedLong size, Utf16String url, Utf16String id, HighResolutionTime::DOMHighResTimeStamp load_time, GC::Ref<DOM::Element> element)
{
    return GC::Heap::the().allocate<LargestContentfulPaint>(paint_timing_info, size, move(url), move(id), load_time, element);
}

// https://w3c.github.io/largest-contentful-paint/#sec-largest-contentful-paint-interface
LargestContentfulPaint::LargestContentfulPaint(PaintTiming::PaintTimingInfo paint_timing_info, WebIDL::UnsignedLong size, Utf16String url, Utf16String id, HighResolutionTime::DOMHighResTimeStamp load_time, GC::Ref<DOM::Element> element)
    // The name attribute's getter must return the empty string.
    // The startTime attribute's getter must return the value of this's renderTime.
    // The duration attribute's getter must return 0.
    : PerformanceTimeline::PerformanceEntry(Utf16String {}, PaintTiming::default_paint_timestamp(paint_timing_info), 0)
    , PaintTiming::PaintTimingMixin(paint_timing_info)
    , m_size(size)
    , m_load_time(load_time)
    , m_id(move(id))
    , m_url(move(url))
    , m_element(element)
{
}

LargestContentfulPaint::~LargestContentfulPaint() = default;

Utf16FlyString const& LargestContentfulPaint::entry_type() const
{
    // The entryType attribute's getter must return the DOMString "largest-contentful-paint".
    return PerformanceTimeline::EntryTypes::largest_contentful_paint;
}

// https://w3c.github.io/largest-contentful-paint/#dom-largestcontentfulpaint-rendertime
HighResolutionTime::DOMHighResTimeStamp LargestContentfulPaint::render_time() const
{
    // The renderTime attribute must return the default paint timestamp given this's paint timing info.
    return PaintTiming::default_paint_timestamp(paint_timing_info());
}

// https://w3c.github.io/largest-contentful-paint/#dom-largestcontentfulpaint-element
GC::Ptr<DOM::Element> LargestContentfulPaint::element() const
{
    // 1. If this's element is not exposed for paint timing given null, return null.
    if (!m_element || !PaintTiming::is_exposed_for_paint_timing(*m_element, nullptr))
        return nullptr;

    // 2. Return this's element.
    return m_element;
}

void LargestContentfulPaint::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

// https://w3c.github.io/largest-contentful-paint/#effective-visual-size-result
struct EffectiveVisualSizeResult {
    double size { 0 };
    double width { 0 };
    double height { 0 };
};

static CSSPixelRect content_box_of(DOM::Element const& element, CSSPixelRect border_box)
{
    Layout::ForcedReadScope read { element.document() };
    auto const* layout_node = element.layout_node(read);
    if (!layout_node || !Painting::has_committed_box(*layout_node))
        return border_box;
    auto box_model = Painting::box_model(*layout_node);
    auto left = box_model.border.left + box_model.padding.left;
    auto top = box_model.border.top + box_model.padding.top;
    auto right = box_model.border.right + box_model.padding.right;
    auto bottom = box_model.border.bottom + box_model.padding.bottom;
    return CSSPixelRect {
        border_box.x() + left,
        border_box.y() + top,
        max(CSSPixels(0), border_box.width() - left - right),
        max(CSSPixels(0), border_box.height() - top - bottom),
    };
}

// https://w3c.github.io/largest-contentful-paint/#effective-visual-size
static Optional<EffectiveVisualSizeResult> effective_visual_size(CSSPixelRect const& intersection_rect, Optional<PaintTiming::ImageRequestInfo> const& image_request, DOM::Element const& element, DOM::Document const& document, CSSPixelRect const& bounding_client_rect)
{
    // 1. Let width be intersectionRect's width, rounded up to the nearest integer.
    auto width = ceil(intersection_rect.width().to_double());

    // 2. Let height be intersectionRect's height, rounded up to the nearest integer.
    auto height = ceil(intersection_rect.height().to_double());

    // 3. Let size be width * height.
    auto size = width * height;

    // 4. Let root be document's browsing context's top-level browsing context's active document.
    // NB: The top-level browsing context may be in another process, so this uses the active document of the local
    //     root of document's navigable.
    auto navigable = document.navigable();
    if (!navigable)
        return {};
    auto root = navigable->local_root()->active_document();
    if (!root || !root->navigable())
        return {};

    // 5. Let rootWidth be root's visual viewport's width, excluding any scrollbars.
    // 6. Let rootHeight be root's visual viewport's height, excluding any scrollbars.
    auto root_size = root->navigable()->viewport_rect().size();
    auto root_width = root_size.width().to_double();
    auto root_height = root_size.height().to_double();

    // 7. If size is equal to rootWidth times rootHeight, return null.
    if (size == root_width * root_height)
        return {};

    // 8. If imageRequest is not null, run the following steps to adjust for image position and upscaling:
    if (image_request.has_value()) {
        // FIXME: 1. If imageRequest's response's content length in bytes is less than size * 0.004, then return null.

        // 2. Let concreteDimensions be imageRequest's concrete object size within element.
        // 3. Let visibleDimensions be concreteDimensions, adjusted for positioning by object-position or
        //    background-position and element's content box.
        // 4. Let clientContentRect be the smallest DOMRectReadOnly containing visibleDimensions with element's
        //    transforms applied.
        // FIXME: Find the concrete object size and position of the image within the element. This takes the whole
        //        content box of the element instead.
        auto client_content_rect = content_box_of(element, bounding_client_rect);

        // 5. Let intersectingClientContentRect be the intersection of clientContentRect with intersectionRect.
        auto intersecting_client_content_rect = client_content_rect.intersected(intersection_rect);

        // 6. Set width to intersectingClientContentRect's width, rounded up to the nearest integer.
        width = ceil(intersecting_client_content_rect.width().to_double());

        // 7. Set height to intersectingClientContentRect's height, rounded up to the nearest integer.
        height = ceil(intersecting_client_content_rect.height().to_double());

        // 8. Set size to width * height.
        size = width * height;

        // 9. Let naturalArea be imageRequest's natural width * imageRequest's natural height.
        double natural_area = 0;
        if (auto const& natural_size = image_request->natural_size; natural_size.has_value())
            natural_area = natural_size->width().to_double() * natural_size->height().to_double();

        // 10. If naturalArea is 0, then return null.
        if (natural_area == 0)
            return {};

        // 11. Let boundingClientArea be clientContentRect's width * clientContentRect's height.
        auto bounding_client_area = client_content_rect.width().to_double() * client_content_rect.height().to_double();

        // 12. Let scaleFactor be boundingClientArea / naturalArea.
        auto scale_factor = bounding_client_area / natural_area;

        // 13. If scaleFactor is greater than 1, then divide size by scaleFactor.
        if (scale_factor > 1)
            size /= scale_factor;
    }

    // 9. Return an effective visual size result with size set to size, width set to width, and height set to height.
    return EffectiveVisualSizeResult { .size = size, .width = width, .height = height };
}

// https://w3c.github.io/largest-contentful-paint/#compute-a-new-largest-contentful-paint-candidate
static Optional<PaintTiming::LargestContentfulPaintCandidate> compute_a_new_largest_contentful_paint_candidate(DOM::Document& document, Vector<PaintTiming::PendingImageRecord> const& painted_images, PaintTiming::PaintedTextNodes const& painted_text_nodes, Optional<PaintTiming::LargestContentfulPaintCandidate> const& current_candidate)
{
    // 1. Let currentSize be currentCandidate's size if currentCandidate is not null or 0 otherwise.
    auto current_size = current_candidate.has_value() ? current_candidate->size : 0;

    // 2. Let largestSize be currentSize.
    auto largest_size = current_size;

    // 3. Let newCandidate be null.
    Optional<PaintTiming::LargestContentfulPaintCandidate> new_candidate;

    auto navigable = document.navigable();
    if (!navigable)
        return {};
    CSSPixelRect viewport_rect { {}, navigable->viewport_rect().size() };

    // 4. For each record of paintedImages:
    for (auto const& record : painted_images) {
        // 1. Let imageElement be record's element.
        auto image_element = record.element;

        // 2. If imageElement is not exposed for paint timing, given document, continue.
        if (!PaintTiming::is_exposed_for_paint_timing(*image_element, document))
            continue;

        // 3. Let intersectionRect be the value returned by the intersection rect algorithm using imageElement as the
        //    target and viewport as the root.
        auto bounding_client_rect = image_element->bounding_client_rect_assuming_layout_clean();
        auto intersection_rect = bounding_client_rect.intersected(viewport_rect);

        // 4. Let result be the effective visual size of imageElement given intersectionRect and record's request.
        auto result = effective_visual_size(intersection_rect, record.request, *image_element, document, bounding_client_rect);

        // 5. If result is null, continue.
        if (!result.has_value())
            continue;

        // 6. If result's size is less than or equal to largestSize, continue.
        if (result->size <= largest_size)
            continue;

        // 7. Set largestSize to result's size.
        largest_size = result->size;

        // 8. Set newCandidate to be a new largest contentful paint candidate with the following values:
        new_candidate = PaintTiming::LargestContentfulPaintCandidate {
            // 1. element set to imageElement.
            .element = image_element,
            // 2. size set to result's size.
            .size = result->size,
            // 3. width set to result's width.
            .width = result->width,
            // 4. height set to result's height.
            .height = result->height,
            // 5. request set to record's request.
            .request = record.request,
            // 6. loadTime set to record's loadTime.
            .load_time = record.load_time,
        };
    }

    // 5. For each textNode of paintedTextNodes,
    Layout::ForcedReadScope read { document };
    auto rect_to_viewport_transform = Painting::rect_to_viewport_transform(document, document.visual_context_tree());
    for (auto const& [text_node, owned_text_nodes] : painted_text_nodes) {
        // 1. If textNode is not exposed for paint timing, given document, continue.
        if (!PaintTiming::is_exposed_for_paint_timing(*text_node, document))
            continue;

        // 2. If textNode has alpha channel value <=0 or opacity value <=0:
        auto const* layout_node = text_node->layout_node(read);
        if (!layout_node)
            continue;
        if (layout_node->color().alpha() == 0 || layout_node->opacity() <= 0) {
            // 1. If textNode's text-shadow value is none, textNode's stroke-color value is transparent and textNode's
            //    stroke-image value is none, continue.
            // FIXME: Check stroke-color and stroke-image once we support text strokes.
            if (layout_node->text_shadow().is_empty())
                continue;
        }

        // 3. Let intersectionRect be the union of the border boxes of all Text nodes in textNode's set of owned text
        //    nodes, intersected with the visual viewport.
        CSSPixelRect text_rect;
        for (auto const& text : owned_text_nodes) {
            auto const* text_layout_node = text->layout_node(read);
            if (!text_layout_node)
                continue;
            auto rect = PaintTiming::client_rect_of_box(*text_layout_node, rect_to_viewport_transform);
            if (rect.is_empty())
                continue;
            text_rect = text_rect.is_empty() ? rect : text_rect.united(rect);
        }
        auto intersection_rect = text_rect.intersected(viewport_rect);

        // 4. Let result be the effective visual size of textNode given intersectionRect and null.
        auto result = effective_visual_size(intersection_rect, {}, *text_node, document, {});

        // 5. If result is null, continue.
        if (!result.has_value())
            continue;

        // 6. If result's size is less than or equal to largestSize, continue.
        if (result->size <= largest_size)
            continue;

        // 7. Set largestSize to result's size.
        largest_size = result->size;

        // 8. Set newCandidate to be a new largest contentful paint candidate with the following values:
        new_candidate = PaintTiming::LargestContentfulPaintCandidate {
            // 1. element set to textNode.
            .element = text_node,
            // 2. size set to result's size.
            .size = result->size,
            // 3. width set to result's width.
            .width = result->width,
            // 4. height set to result's height.
            .height = result->height,
            // 5. request set to null.
            .request = {},
            // 6. loadTime set to 0.
            .load_time = 0,
        };
    }

    // 6. If newCandidate is not null and currentSize is greater than 0:
    if (new_candidate.has_value() && current_size > 0) {
        // 1. If newCandidate's width minus currentCandidate's width is less than or equal to 3, and newCandidate's
        //    height minus currentCandidate's height is less than or equal to 3, return null.
        if (new_candidate->width - current_candidate->width <= 3 && new_candidate->height - current_candidate->height <= 3)
            return {};
    }

    // 7. Return newCandidate.
    return new_candidate;
}

// https://w3c.github.io/largest-contentful-paint/#create-a-largestcontentfulpaint-entry
static GC::Ref<LargestContentfulPaint> create_a_largest_contentful_paint_entry(PaintTiming::LargestContentfulPaintCandidate const& candidate, PaintTiming::PaintTimingInfo const& paint_timing_info)
{
    // 1. Let url be the empty string.
    Utf16String url;

    // 2. If candidate's request is not null, set url to be candidate's request's request URL.
    if (candidate.request.has_value())
        url = candidate.request->url;

    // 3. Let entry be a new LargestContentfulPaint entry with document's relevant realm, whose paint timing info is
    //    paintTimingInfo, with its
    //    - size set to candidate's size,
    //    - url set to url,
    //    - id set to candidate's element's element id,
    //    - loadTime set to candidate's loadTime,
    //    - and element set to candidate's element.
    Utf16String id;
    if (auto const& element_id = candidate.element->id(); element_id.has_value())
        id = element_id->to_utf16_string();
    auto size = static_cast<WebIDL::UnsignedLong>(clamp(candidate.size, 0.0, static_cast<double>(NumericLimits<WebIDL::UnsignedLong>::max())));

    // 4. Return entry.
    return LargestContentfulPaint::create(paint_timing_info, size, move(url), move(id), candidate.load_time, candidate.element);
}

// https://w3c.github.io/largest-contentful-paint/#report-largest-contentful-paint
void report_largest_contentful_paint(DOM::Document& document, PaintTiming::PaintTimingInfo const& paint_timing_info, Vector<PaintTiming::PendingImageRecord> const& painted_images, PaintTiming::PaintedTextNodes const& painted_text_nodes)
{
    // NOTE: Each pending image record in paintedImages and text element in paintedTextNodes will only be reported
    //       exactly once, from mark paint timing, for the first paint where the element is considered paintable (i.e.
    //       has opacity and visibility) and contentful (i.e. image resource or blocking fonts are sufficiently loaded).

    // 1. Let window be document’s relevant global object.
    auto& window = HTML::relevant_window(document);

    // 2. If either of window's has dispatched scroll event or has dispatched input event is true, return.
    if (window.has_dispatched_scroll_event() || window.event_timing_state().has_dispatched_input_event)
        return;

    // 3. Let newCandidate be the result of computing a new largest contentful paint candidate given document,
    //    paintedImages, paintedTextNodes, and document's current largest contentful paint candidate.
    auto& state = document.paint_timing_state();
    auto new_candidate = compute_a_new_largest_contentful_paint_candidate(document, painted_images, painted_text_nodes, state.current_largest_contentful_paint_candidate);

    // 4. If newCandidate is null, return.
    if (!new_candidate.has_value())
        return;

    // 5. Set document's current largest contentful paint candidate to newCandidate.
    state.current_largest_contentful_paint_candidate = new_candidate;

    // 6. Let entry be the result of creating a LargestContentfulPaint entry with newCandidate, paintTimingInfo, and
    //    document.
    auto entry = create_a_largest_contentful_paint_entry(*new_candidate, paint_timing_info);

    // 7. Queue the PerformanceEntry entry.
    window.queue_performance_entry(entry);
    window.add_performance_entry(entry, HTML::WindowOrWorkerGlobalScopeMixin::CheckIfPerformanceBufferIsFull::Yes);
}

}
