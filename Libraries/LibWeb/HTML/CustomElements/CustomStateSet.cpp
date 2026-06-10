/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Set.h>
#include <LibWeb/Bindings/ImplementedInBindings.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/Invalidation/CustomElementInvalidator.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CustomStateSet);

GC::Ref<CustomStateSet> CustomStateSet::create(GC::Ref<DOM::Element> element)
{
    return GC::Heap::the().allocate<CustomStateSet>(element);
}

CustomStateSet::CustomStateSet(GC::Ref<DOM::Element> element)
    : m_element(element)
{
}

void CustomStateSet::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

bool CustomStateSet::has_state(FlyString const& state) const
{
    return m_states.contains(state);
}

void CustomStateSet::add_state(FlyString const& state)
{
    m_states.set(state, AK::HashSetExistingEntryBehavior::Keep);
}

bool CustomStateSet::remove_state(FlyString const& state)
{
    return m_states.remove(state);
}

void CustomStateSet::clear_states()
{
    m_states.clear();
}

}

namespace Web::Bindings {

static WrapperWorldWeakValueCacheMap<HTML::CustomStateSet const, JS::Set>& custom_state_set_caches()
{
    static NeverDestroyed<WrapperWorldWeakValueCacheMap<HTML::CustomStateSet const, JS::Set>> caches;
    return *caches;
}

static WrapperWorldWeakValueCache<JS::Set>& custom_state_set_cache_for(HTML::CustomStateSet const& custom_state_set)
{
    return custom_state_set_caches().cache_for(custom_state_set);
}

static FlyString state_from_set_value(JS::Value value)
{
    VERIFY(value.is_string());
    return FlyString { value.as_string().utf8_string() };
}

static void add_state_to_set(JS::Set& set_entries, FlyString const& state)
{
    auto& realm = HTML::relevant_realm(set_entries);
    auto value = GC::make_root(JS::PrimitiveString::create(realm.vm(), state));
    set_entries.set_add(JS::Value { value.ptr() });
}

static void remove_state_from_set(JS::Set& set_entries, FlyString const& state)
{
    auto& realm = HTML::relevant_realm(set_entries);
    auto value = GC::make_root(JS::PrimitiveString::create(realm.vm(), state));
    set_entries.set_remove(JS::Value { value.ptr() });
}

static GC::Root<JS::Set> create_set_entries(JS::Realm& realm, OrderedHashTable<FlyString> const& states)
{
    auto set_entries = GC::make_root(JS::Set::create(realm));
    for (auto const& state : states)
        add_state_to_set(*set_entries, state);
    return set_entries;
}

GC::Ref<JS::Set> setlike_entries(JS::Realm& realm, WrapperWorld const& wrapper_world, HTML::CustomStateSet const& custom_state_set)
{
    auto& cache = custom_state_set_cache_for(custom_state_set);
    if (auto set_entries = cache.get(wrapper_world))
        return *set_entries;

    auto set_entries = create_set_entries(realm, custom_state_set.states());
    cache.set(wrapper_world, set_entries.ptr());
    return GC::Ref { *set_entries };
}

bool setlike_has(HTML::CustomStateSet const& custom_state_set, JS::Value value)
{
    return custom_state_set.has_state(state_from_set_value(value));
}

void setlike_add(HTML::CustomStateSet& custom_state_set, JS::Value value)
{
    auto state = state_from_set_value(value);
    custom_state_set.add_state(state);
    custom_state_set_cache_for(custom_state_set).for_each([&](auto& set_entries) {
        add_state_to_set(set_entries, state);
    });
}

bool setlike_remove(HTML::CustomStateSet& custom_state_set, JS::Value value)
{
    auto state = state_from_set_value(value);
    auto removed = custom_state_set.remove_state(state);
    if (removed) {
        custom_state_set_cache_for(custom_state_set).for_each([&](auto& set_entries) {
            remove_state_from_set(set_entries, state);
        });
    }
    return removed;
}

void setlike_clear(HTML::CustomStateSet& custom_state_set)
{
    custom_state_set.clear_states();
    custom_state_set_cache_for(custom_state_set).for_each([&](auto& set_entries) {
        set_entries.set_clear();
    });
}

void setlike_on_set_modified_from_js(HTML::CustomStateSet& custom_state_set)
{
    CSS::Invalidation::invalidate_style_after_custom_state_set_change(custom_state_set.element());
}

}
