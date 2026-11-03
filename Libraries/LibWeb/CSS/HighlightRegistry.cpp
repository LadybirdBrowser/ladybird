/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
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
bool web_highlight_registry_has_highlights(void* registry);
bool web_highlight_registry_has_ranges(void* registry);
bool web_highlight_registry_contains_range(void* registry, void* range);
void web_highlight_registry_for_each_highlight_in_stacking_order(JSVM*, void* registry, void* context, void (*callback)(void* context, FlatPtr name, void* const* ranges, size_t range_count));

// Declared in Bindings/Rust/src/css/highlight.rs and Bindings/Rust/src/css/highlight_registry.rs.
void* web_css_abstract_range_of_wrapper(void* wrapper);
void* web_css_shadow_root_of_wrapper(void* wrapper);
bool web_css_highlight_registry_point_is_in_viewport(void* document, float x, float y);
void* web_css_highlight_registry_shadow_root_at_point(void* document, float x, float y);
bool web_css_highlight_registry_range_is_hit(void* abstract_range, float x, float y);
void web_css_highlight_registry_invalidate_custom_highlights(void* document);
}

GC::Ref<HighlightRegistry> HighlightRegistry::create(DOM::Document& document)
{
    auto* registry = web_highlight_registry_create(JS::HostABI::vm_to_abi(JS::VM::the()), static_cast<GC::Cell*>(&document));
    return GC::Ref { *static_cast<HighlightRegistry*>(registry) };
}

bool HighlightRegistry::has_highlights()
{
    return web_highlight_registry_has_highlights(this);
}

bool HighlightRegistry::has_ranges()
{
    return web_highlight_registry_has_ranges(this);
}

bool HighlightRegistry::contains_range(DOM::AbstractRange& range)
{
    return web_highlight_registry_contains_range(this, static_cast<GC::Cell*>(&range));
}

void HighlightRegistry::for_each_highlight_in_stacking_order(Function<void(Utf16String const& name, ReadonlySpan<GC::Ref<DOM::AbstractRange>> ranges)> const& callback)
{
    auto trampoline = [](void* context, FlatPtr raw_name, void* const* raw_ranges, size_t range_count) {
        auto const& callback = *static_cast<Function<void(Utf16String const&, ReadonlySpan<GC::Ref<DOM::AbstractRange>>)> const*>(context);
        auto name = Utf16String::adopt_raw(raw_name);
        GC::RootVector<GC::Ref<DOM::AbstractRange>> ranges;
        ranges.ensure_capacity(range_count);
        for (size_t i = 0; i < range_count; ++i)
            ranges.unchecked_append(*static_cast<DOM::AbstractRange*>(static_cast<GC::Cell*>(raw_ranges[i])));
        callback(name, ranges.span());
    };
    web_highlight_registry_for_each_highlight_in_stacking_order(JS::HostABI::vm_to_abi(JS::VM::the()), this, const_cast<void*>(static_cast<void const*>(&callback)), trampoline);
}

bool HighlightRegistry::range_is_paintable(DOM::Document const& document, DOM::AbstractRange const& range)
{
    // https://drafts.csswg.org/css-highlight-api-1/#range-invalidation
    // When computing how to render a document, if start node or end node of any range in the highlight registry
    // associated with that document’s window refer to a Node whose shadow-including root is not that document, the user
    // agent must ignore that range. If any StaticRange in the highlight registry associated with that document’s window
    // is not valid, the user agent must ignore that range.
    if (&range.start_container()->shadow_including_root() != &document || &range.end_container()->shadow_including_root() != &document)
        return false;
    if (auto const* static_range = as_if<DOM::StaticRange>(range); static_range && !static_range->is_valid())
        return false;

    // https://drafts.csswg.org/css-highlight-api-1/#painting
    // Collapsed ranges are not rendered.
    return !range.collapsed();
}

static DOM::Document& document_from_abi(void* document)
{
    return *static_cast<DOM::Document*>(static_cast<GC::Cell*>(document));
}

void web_css_highlight_registry_invalidate_custom_highlights(void* raw_document)
{
    document_from_abi(raw_document).invalidate_custom_highlights();
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
