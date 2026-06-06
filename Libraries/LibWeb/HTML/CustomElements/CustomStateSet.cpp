/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Set.h>
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

GC::Ref<JS::Set> CustomStateSet::set_entries(JS::Realm& realm, Bindings::WrapperWorld const& wrapper_world) const
{
    if (auto set_entries = m_set_entries.get(wrapper_world))
        return *set_entries;

    auto set_entries = create_set_entries(realm, m_states);
    m_set_entries.set(wrapper_world, set_entries.ptr());
    return GC::Ref { *set_entries };
}

bool CustomStateSet::set_has(JS::Value value) const
{
    return m_states.contains(state_from_set_value(value));
}

void CustomStateSet::set_add(JS::Value value)
{
    auto state = state_from_set_value(value);
    m_states.set(state, AK::HashSetExistingEntryBehavior::Keep);
    m_set_entries.for_each([&](auto& set_entries) {
        add_state_to_set(set_entries, state);
    });
}

bool CustomStateSet::set_remove(JS::Value value)
{
    auto state = state_from_set_value(value);
    auto removed = m_states.remove(state);
    if (removed) {
        m_set_entries.for_each([&](auto& set_entries) {
            remove_state_from_set(set_entries, state);
        });
    }
    return removed;
}

void CustomStateSet::set_clear()
{
    m_states.clear();
    m_set_entries.for_each([&](auto& set_entries) {
        set_entries.set_clear();
    });
}

bool CustomStateSet::has_state(FlyString const& state) const
{
    return m_states.contains(state);
}

void CustomStateSet::on_set_modified_from_js(Badge<Bindings::CustomStateSetPrototype>)
{
    CSS::Invalidation::invalidate_style_after_custom_state_set_change(*m_element);
}

}
