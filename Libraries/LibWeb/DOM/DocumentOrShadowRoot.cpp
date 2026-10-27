/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/DocumentOrShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Painting/HitTestResult.h>

namespace Web::DOM {

static Element* associated_hit_element(Painting::HitTestResult const& result, Node& context)
{
    auto* node = result.dom_node();
    if (!node)
        return nullptr;
    auto* element = as_if<Element>(node);
    // NB: Text fragments use their DOM parent element, or the host when directly under a shadow root.
    if (!element && is<Text>(*node))
        element = node->parent_or_shadow_host_element();
    if (!element)
        return nullptr;
    // INTEROP: Chrome and Safari retarget hit elements against the document or shadow root making the query.
    // https://dom.spec.whatwg.org/#retarget
    return as<Element>(retarget(element, &context));
}

// https://drafts.csswg.org/cssom-view/#dom-document-elementfrompoint
Element const* calculate_element_from_point(Node& context, double x, double y)
{
    auto& document = context.document();
    Layout::ForcedReadScope read { document };
    // 1. If either argument is negative, x is greater than the viewport width excluding the size of a rendered scroll
    //    bar (if any), or y is greater than the viewport height excluding the size of a rendered scroll bar (if any), or
    //    there is no viewport associated with the document, return null and terminate these steps.
    auto viewport_rect = document.viewport_rect();
    CSSPixelPoint position { x, y };
    // FIXME: This should account for the size of the scroll bar.
    if (x < 0 || y < 0 || position.x() > viewport_rect.width() || position.y() > viewport_rect.height())
        return nullptr;

    // NB: Ensure the layout tree exists prior to hit testing.
    document.update_layout(UpdateLayoutReason::DocumentElementFromPoint);

    // 2. If there is a box in the viewport that would be a target for hit testing at coordinates x,y, when applying the transforms
    //    that apply to the descendants of the viewport, return the associated element and terminate these steps.
    GC::Ptr<Element> hit_element;
    (void)document.hit_test_all(read, position, [&](Painting::HitTestResult result) {
        if (auto* element = associated_hit_element(result, context)) {
            hit_element = element;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    if (hit_element)
        return hit_element.ptr();

    // 3. If the document has a root element, return the root element and terminate these steps.
    if (auto const* root_element = document.document_element())
        return root_element;

    // 4. Return null.
    return nullptr;
}

// https://drafts.csswg.org/cssom-view/#dom-document-elementsfrompoint
GC::RootVector<GC::Ref<Element>> calculate_elements_from_point(Node& context, double x, double y)
{
    auto& document = context.document();
    Layout::ForcedReadScope read { document };
    // 1. Let sequence be a new empty sequence.
    GC::RootVector<GC::Ref<Element>> sequence;

    // 2. If either argument is negative, x is greater than the viewport width excluding the size of a rendered scroll bar (if any),
    //    or y is greater than the viewport height excluding the size of a rendered scroll bar (if any),
    //    or there is no viewport associated with the document, return sequence and terminate these steps.
    auto viewport_rect = document.viewport_rect();
    CSSPixelPoint position { x, y };
    // FIXME: This should account for the size of the scroll bar.
    if (x < 0 || y < 0 || position.x() > viewport_rect.width() || position.y() > viewport_rect.height())
        return sequence;

    // NB: Ensure the layout tree exists prior to hit testing.
    document.update_layout(UpdateLayoutReason::DocumentElementsFromPoint);

    // 3. For each box in the viewport, in paint order, starting with the topmost box, that would be a target for
    //    hit testing at coordinates x,y even if nothing would be overlapping it, when applying the transforms that
    //    apply to the descendants of the viewport, append the associated element to sequence.
    (void)document.hit_test_all(read, position, [&](Painting::HitTestResult result) {
        if (auto* element = associated_hit_element(result, context)) {
            // NB: Avoid duplicates when multiple boxes resolve or retarget to the same element.
            if (!sequence.contains_slow(GC::Ref { *element }))
                sequence.append(*element);
        }
        return TraversalDecision::Continue;
    });

    // 4. If the document has a root element, and the last item in sequence is not the root element,
    //    append the root element to sequence.
    if (auto* root_element = document.document_element(); root_element && (sequence.is_empty() || (sequence.last().ptr() != root_element)))
        sequence.append(*root_element);

    // 5. Return sequence.
    return sequence;
}

}
