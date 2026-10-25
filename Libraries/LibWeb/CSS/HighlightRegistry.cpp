/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Bindings/Highlight.h>
#include <LibWeb/Bindings/HighlightRegistry.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/BindingsGlue.h>
#include <LibWeb/CSS/Highlight.h>
#include <LibWeb/CSS/HighlightRegistry.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StaticRange.h>
#include <LibWeb/DOM/Utils.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Painting/HitTestResult.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(HighlightRegistry);

GC::Ref<HighlightRegistry> HighlightRegistry::create(DOM::Document& document)
{
    return GC::Heap::the().allocate<HighlightRegistry>(document);
}

HighlightRegistry::HighlightRegistry(DOM::Document& document)
    : m_document(document)
{
}

void HighlightRegistry::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    for (auto& entry : m_highlights)
        visitor.visit(entry.value);
}

GC::Ptr<Highlight> HighlightRegistry::get_highlight(Utf16View name) const
{
    auto highlight = m_highlights.get(Utf16String::from_utf16(name));
    if (!highlight.has_value())
        return {};
    return *highlight;
}

bool HighlightRegistry::has_highlight(Utf16View name) const
{
    return m_highlights.contains(Utf16String::from_utf16(name));
}

void HighlightRegistry::set_highlight(Utf16String name, GC::Ref<Highlight> highlight)
{
    // FIXME: https://drafts.csswg.org/css-highlight-api-1/#repaint
    //        The addition or removal of a custom highlight in the highlight registry must cause the user agent to
    //        reevaluate the rendering, and to repaint if appropriate. We do not paint custom highlights yet.
    m_highlights.set(move(name), highlight);
}

bool HighlightRegistry::remove_highlight(Utf16View name)
{
    return m_highlights.remove(Utf16String::from_utf16(name));
}

void HighlightRegistry::clear_highlights()
{
    m_highlights.clear();
}

static bool shadow_root_is_contained_by_options(DOM::ShadowRoot const& shadow_root, Bindings::HighlightsFromPointOptions const& options)
{
    for (auto const& allowed_shadow_root : options.shadow_roots) {
        if (allowed_shadow_root.ptr() == &shadow_root)
            return true;
    }
    return false;
}

// https://drafts.csswg.org/css-highlight-api-1/#dom-highlightregistry-highlightsfrompoint
GC::ConservativeVector<Bindings::HighlightHitResult> HighlightRegistry::highlights_from_point(float x, float y, Bindings::HighlightsFromPointOptions const& options)
{
    auto& document = *m_document;

    // 1. If any of the following are true, return the empty sequence:
    //    * x is negative
    //    * y is negative
    //    * x is greater than the viewport width excluding the size of a rendered scroll bar (if any)
    //    * y is greater than the viewport height excluding the size of a rendered scroll bar (if any)
    // FIXME: This should account for the size of the scroll bar.
    auto viewport_rect = document.viewport_rect();
    CSSPixelPoint position { x, y };
    if (x < 0 || y < 0 || position.x() > viewport_rect.width() || position.y() > viewport_rect.height())
        return {};

    //    * The topmost box in the viewport in paint order that would be a target for hit testing at coordinates x,y
    //      when applying the transforms that apply to the descendants of the viewport, has an element associated to
    //      it that's in a shadow tree whose shadow root is not contained by options.shadowRoots.
    {
        document.update_layout(DOM::UpdateLayoutReason::HighlightRegistryHighlightsFromPoint);
        Layout::ForcedReadScope read { document };
        GC::Ptr<DOM::Element> hit_element;
        (void)document.hit_test_all(read, position, [&](Painting::HitTestResult result) {
            auto* node = result.dom_node();
            if (!node)
                return TraversalDecision::Continue;
            hit_element = is<DOM::Element>(*node) ? static_cast<DOM::Element*>(node) : node->parent_element();
            return TraversalDecision::Break;
        });
        if (hit_element) {
            // AD-HOC: If the element is inside a UA internal shadow root, retarget to the host, as elementFromPoint()
            //         does. UA internal shadow roots can never be passed in options.shadowRoots.
            hit_element = DOM::retarget_from_ua_internal_shadow_root(*hit_element);
            if (auto* shadow_root = as_if<DOM::ShadowRoot>(hit_element->root()); shadow_root && !shadow_root_is_contained_by_options(*shadow_root, options))
                return {};
        }
    }

    // 2. Otherwise, let results be an empty sequence.
    struct ResultWithRegistrationIndex {
        Bindings::HighlightHitResult result;
        WebIDL::Long priority;
        size_t registration_index;
    };
    GC::ConservativeVector<ResultWithRegistrationIndex> results;

    // 3. For each Highlight highlight in this HighlightRegistry:
    size_t registration_index = 0;
    for (auto const& entry : m_highlights) {
        auto highlight = entry.value;

        // 1. Let result be a new HighlightHitResult with highlight set to highlight.
        Bindings::HighlightHitResult result;
        result.highlight = highlight;
        GC::RootVector<GC::Ref<DOM::AbstractRange>> ranges;

        // 2. For each AbstractRange abstractRange in highlight:
        for (auto abstract_range : highlight->ranges()) {
            // 1. If abstractRange is an invalid StaticRange, then continue.
            if (auto* static_range = as_if<DOM::StaticRange>(*abstract_range); static_range && !static_range->is_valid())
                continue;

            // 2. Let range be a new Range whose start node and end node are set to abstractRange’s start node and end
            //    node respectively, and start offset and end offset are set to abstractRange’s start offset and end
            //    offset respectively.
            // OPTIMIZATION: A live Range already has these boundary points, so we use it instead of creating a new live
            //               Range that every DOM mutation would have to update until it is collected.
            GC::Ptr<DOM::Range> range = as_if<DOM::Range>(*abstract_range);
            if (!range)
                range = DOM::Range::create(abstract_range->start_container(), abstract_range->start_offset(), abstract_range->end_container(), abstract_range->end_offset());

            // 3. If the coordinates x,y fall inside at least one of the DOMRects returned by calling getClientRects()
            //    on range, then append abstractRange to result.ranges.
            // NB: A rect is treated as half-open, so that the empty rect of a collapsed range is never hit.
            auto client_rects = range->client_rects();
            auto is_hit = any_of(client_rects, [&](Gfx::DoubleRect const& rect) {
                return x >= rect.left() && x < rect.right() && y >= rect.top() && y < rect.bottom();
            });
            if (is_hit)
                ranges.append(abstract_range);
        }

        // 3. If result.ranges is not empty, append result to results.
        if (!ranges.is_empty()) {
            result.ranges = move(ranges);
            results.append({ move(result), highlight->priority(), registration_index });
        }
        ++registration_index;
    }

    // 4. Sort results by descending order of priority of its HighlightHitResults' highlight attributes.
    // NB: When two or more custom highlights have the same numerical priority, the one most recently registered has
    //     the higher effective priority.
    quick_sort(results, [](auto const& a, auto const& b) {
        if (a.priority != b.priority)
            return a.priority > b.priority;
        return a.registration_index > b.registration_index;
    });

    // 5. Return results.
    GC::ConservativeVector<Bindings::HighlightHitResult> sorted_results;
    sorted_results.ensure_capacity(results.size());
    for (auto& result : results)
        sorted_results.unchecked_append(move(result.result));
    return sorted_results;
}

}

namespace Web::Bindings {

static WrapperWorldWeakValueCacheMap<CSS::HighlightRegistry, JS::Map>& highlight_registry_caches()
{
    static NeverDestroyed<WrapperWorldWeakValueCacheMap<CSS::HighlightRegistry, JS::Map>> caches;
    return *caches;
}

static WrapperWorldWeakValueCache<JS::Map>& entries_cache_for(CSS::HighlightRegistry& registry)
{
    return highlight_registry_caches().cache_for(registry);
}

static void set_map_entry(JS::Map& map_entries, Utf16View name, CSS::Highlight& highlight)
{
    auto& map_realm = HTML::relevant_realm(map_entries);
    auto key = GC::make_root(JS::PrimitiveString::create(map_realm.vm(), name));
    JS::Value value = Bindings::wrap(Bindings::host_defined_wrapper_world(map_realm), map_realm, GC::Ref { highlight });
    map_entries.map_set(JS::Value { key.ptr() }, value);
}

static void remove_map_entry(JS::Map& map_entries, Utf16View name)
{
    auto& map_realm = HTML::relevant_realm(map_entries);
    auto key = GC::make_root(JS::PrimitiveString::create(map_realm.vm(), name));
    map_entries.map_remove(JS::Value { key.ptr() });
}

GC::Ref<JS::Map> map_entries(JS::Realm& realm, CSS::HighlightRegistry& registry)
{
    auto& wrapper_world = host_defined_wrapper_world(realm);
    auto& cache = entries_cache_for(registry);
    if (auto map_entries = cache.get(wrapper_world))
        return *map_entries;

    auto map_entries = GC::make_root(JS::Map::create(realm));
    for (auto const& entry : registry.highlights())
        set_map_entry(*map_entries, entry.key, *entry.value);
    cache.set(wrapper_world, map_entries.ptr());
    return GC::Ref { *map_entries };
}

Optional<JS::Value> map_get(JS::Realm& realm, CSS::HighlightRegistry& registry, Utf16View key)
{
    auto highlight = registry.get_highlight(key);
    if (!highlight)
        return {};
    return JS::Value { Bindings::wrap(host_defined_wrapper_world(realm), realm, GC::Ref { *highlight }) };
}

bool map_has(CSS::HighlightRegistry& registry, Utf16View key)
{
    return registry.has_highlight(key);
}

bool map_remove(CSS::HighlightRegistry& registry, Utf16View key)
{
    auto removed = registry.remove_highlight(key);
    if (removed) {
        entries_cache_for(registry).for_each([&](auto& map_entries) {
            remove_map_entry(map_entries, key);
        });
    }
    return removed;
}

void map_clear(CSS::HighlightRegistry& registry)
{
    registry.clear_highlights();
    entries_cache_for(registry).for_each([&](auto& map_entries) {
        map_entries.map_clear();
    });
}

WebIDL::ExceptionOr<void> set(JS::Realm&, CSS::HighlightRegistry& registry, Utf16String const& key, JS::Value value)
{
    auto* highlight = Bindings::impl_from<CSS::Highlight>(&value.as_object());
    VERIFY(highlight);
    registry.set_highlight(key, *highlight);
    entries_cache_for(registry).for_each([&](auto& map_entries) {
        set_map_entry(map_entries, key, *highlight);
    });
    return {};
}

}
