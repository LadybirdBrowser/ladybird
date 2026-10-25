/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Set.h>
#include <LibWeb/Bindings/Highlight.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/BindingsGlue.h>
#include <LibWeb/CSS/Highlight.h>
#include <LibWeb/DOM/AbstractRange.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(Highlight);

// https://drafts.csswg.org/css-highlight-api-1/#dom-highlight-highlight
GC::Ref<Highlight> Highlight::construct_impl(GC::RootVector<GC::Ref<DOM::AbstractRange>> const& initial_ranges)
{
    // 1. Let highlight be the new Highlight object.
    auto highlight = GC::Heap::the().allocate<Highlight>();

    // 2. Set highlight’s priority to 0.
    highlight->m_priority = 0;

    // 3. Set highlight’s type to highlight.
    highlight->m_type = HighlightType::Highlight;

    // 4. For each range of initialRanges, let rangeArg be the result of converting range to an ECMAScript value,
    //    then run the steps for a built-in setlike add function, with highlight as the this value, and rangeArg as
    //    the argument.
    // NB: highlight has no wrapper yet, so there are no set entries in any world to update.
    for (auto range : initial_ranges)
        highlight->add_range(range);

    // 5. Return highlight.
    return highlight;
}

Highlight::Highlight() = default;

void Highlight::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto range : m_ranges)
        visitor.visit(range);
}

bool Highlight::has_range(DOM::AbstractRange& range) const
{
    return m_ranges.contains(GC::Ref { range });
}

void Highlight::add_range(GC::Ref<DOM::AbstractRange> range)
{
    m_ranges.set(range, AK::HashSetExistingEntryBehavior::Keep);
}

bool Highlight::remove_range(DOM::AbstractRange& range)
{
    return m_ranges.remove(GC::Ref { range });
}

void Highlight::clear_ranges()
{
    m_ranges.clear();
}

}

namespace Web::Bindings {

static WrapperWorldWeakValueCacheMap<CSS::Highlight const, JS::Set>& highlight_set_caches()
{
    static NeverDestroyed<WrapperWorldWeakValueCacheMap<CSS::Highlight const, JS::Set>> caches;
    return *caches;
}

static WrapperWorldWeakValueCache<JS::Set>& highlight_set_cache_for(CSS::Highlight const& highlight)
{
    return highlight_set_caches().cache_for(highlight);
}

static DOM::AbstractRange& range_from_set_value(JS::Value value)
{
    auto* range = Bindings::impl_from<DOM::AbstractRange>(&value.as_object());
    VERIFY(range);
    return *range;
}

static JS::Value wrap_range(JS::Realm& realm, DOM::AbstractRange& range)
{
    return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { range });
}

GC::Ref<JS::Set> setlike_entries(JS::Realm& realm, WrapperWorld const& wrapper_world, CSS::Highlight const& highlight)
{
    auto& cache = highlight_set_cache_for(highlight);
    if (auto set_entries = cache.get(wrapper_world))
        return *set_entries;

    auto set_entries = GC::make_root(JS::Set::create(realm));
    for (auto range : highlight.ranges())
        set_entries->set_add(wrap_range(realm, *range));
    cache.set(wrapper_world, set_entries.ptr());
    return GC::Ref { *set_entries };
}

bool setlike_has(CSS::Highlight const& highlight, JS::Value value)
{
    return highlight.has_range(range_from_set_value(value));
}

void setlike_add(CSS::Highlight& highlight, JS::Value value)
{
    auto& range = range_from_set_value(value);
    highlight.add_range(range);
    highlight_set_cache_for(highlight).for_each([&](auto& set_entries) {
        set_entries.set_add(wrap_range(HTML::relevant_realm(set_entries), range));
    });
}

bool setlike_remove(CSS::Highlight& highlight, JS::Value value)
{
    auto& range = range_from_set_value(value);
    auto removed = highlight.remove_range(range);
    if (removed) {
        highlight_set_cache_for(highlight).for_each([&](auto& set_entries) {
            set_entries.set_remove(wrap_range(HTML::relevant_realm(set_entries), range));
        });
    }
    return removed;
}

void setlike_clear(CSS::Highlight& highlight)
{
    highlight.clear_ranges();
    highlight_set_cache_for(highlight).for_each([&](auto& set_entries) {
        set_entries.set_clear();
    });
}

void setlike_on_set_modified_from_js(CSS::Highlight&)
{
    // FIXME: https://drafts.csswg.org/css-highlight-api-1/#repaint
    //        The addition or removal of a range in a registered custom highlight must cause the user agent to
    //        reevaluate the rendering, and to repaint if appropriate. We do not paint custom highlights yet.
}

}
