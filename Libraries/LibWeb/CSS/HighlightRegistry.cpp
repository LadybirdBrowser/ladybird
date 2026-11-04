/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/HighlightRegistry.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StaticRange.h>
#include <LibWeb/DOM/Utils.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Painting/HitTestResult.h>

namespace Web::CSS {

extern "C" {

// Defined in Bindings/Rust/src/css/exports.rs.
void* web_highlight_registry_create(JSVM*, void* document);

// Declared in Bindings/Rust/src/css/highlight.rs and Bindings/Rust/src/css/highlight_registry.rs.
void* web_css_abstract_range_of_wrapper(void* wrapper);
void* web_css_shadow_root_of_wrapper(void* wrapper);
bool web_css_highlight_registry_point_is_in_viewport(void* document, float x, float y);
void* web_css_highlight_registry_shadow_root_at_point(void* document, float x, float y);
bool web_css_highlight_registry_range_is_hit(void* abstract_range, float x, float y);
}

GC::Ref<HighlightRegistry> HighlightRegistry::create(DOM::Document& document)
{
    auto* registry = web_highlight_registry_create(JS::HostABI::vm_to_abi(JS::VM::the()), static_cast<GC::Cell*>(&document));
    return GC::Ref { *static_cast<HighlightRegistry*>(registry) };
}

static DOM::Document& document_from_abi(void* document)
{
    return *static_cast<DOM::Document*>(static_cast<GC::Cell*>(document));
}

void* web_css_abstract_range_of_wrapper(void* wrapper)
{
    auto* range = Bindings::impl_from<DOM::AbstractRange>(static_cast<JS::Object*>(wrapper));
    VERIFY(range);
    return static_cast<GC::Cell*>(range);
}

void* web_css_shadow_root_of_wrapper(void* wrapper)
{
    auto* shadow_root = Bindings::impl_from<DOM::ShadowRoot>(static_cast<JS::Object*>(wrapper));
    VERIFY(shadow_root);
    return static_cast<GC::Cell*>(shadow_root);
}

bool web_css_highlight_registry_point_is_in_viewport(void* raw_document, float x, float y)
{
    // FIXME: This should account for the size of the scroll bar.
    auto viewport_rect = document_from_abi(raw_document).viewport_rect();
    CSSPixelPoint position { x, y };
    return !(x < 0 || y < 0 || position.x() > viewport_rect.width() || position.y() > viewport_rect.height());
}

// The shadow root of the tree of the element of the topmost box at the point, if it is in a shadow tree.
void* web_css_highlight_registry_shadow_root_at_point(void* raw_document, float x, float y)
{
    auto& document = document_from_abi(raw_document);
    document.update_layout(DOM::UpdateLayoutReason::HighlightRegistryHighlightsFromPoint);
    Layout::ForcedReadScope read { document };
    GC::Ptr<DOM::Element> hit_element;
    (void)document.hit_test_all(read, CSSPixelPoint { x, y }, [&](Painting::HitTestResult result) {
        auto* node = result.dom_node();
        if (!node)
            return TraversalDecision::Continue;
        hit_element = is<DOM::Element>(*node) ? static_cast<DOM::Element*>(node) : node->parent_element();
        return TraversalDecision::Break;
    });
    if (!hit_element)
        return nullptr;

    // AD-HOC: If the element is inside a UA internal shadow root, retarget to the host, as elementFromPoint()
    //         does. UA internal shadow roots can never be passed in options.shadowRoots.
    hit_element = DOM::retarget_from_ua_internal_shadow_root(*hit_element);
    return static_cast<GC::Cell*>(as_if<DOM::ShadowRoot>(hit_element->root()));
}

bool web_css_highlight_registry_range_is_hit(void* raw_abstract_range, float x, float y)
{
    auto& abstract_range = *static_cast<DOM::AbstractRange*>(static_cast<GC::Cell*>(raw_abstract_range));

    // 1. If abstractRange is an invalid StaticRange, then continue.
    if (auto* static_range = as_if<DOM::StaticRange>(abstract_range); static_range && !static_range->is_valid())
        return false;

    // 2. Let range be a new Range whose start node and end node are set to abstractRange’s start node and end
    //    node respectively, and start offset and end offset are set to abstractRange’s start offset and end
    //    offset respectively.
    // OPTIMIZATION: A live Range already has these boundary points, so we use it instead of creating a new live
    //               Range that every DOM mutation would have to update until it is collected.
    GC::Ptr<DOM::Range> range = as_if<DOM::Range>(abstract_range);
    if (!range)
        range = DOM::Range::create(abstract_range.start_container(), abstract_range.start_offset(), abstract_range.end_container(), abstract_range.end_offset());

    // 3. If the coordinates x,y fall inside at least one of the DOMRects returned by calling getClientRects()
    //    on range, then append abstractRange to result.ranges.
    // NB: A rect is treated as half-open, so that the empty rect of a collapsed range is never hit.
    return any_of(range->client_rects(), [&](Gfx::DoubleRect const& rect) {
        return x >= rect.left() && x < rect.right() && y >= rect.top() && y < rect.bottom();
    });
}

}
