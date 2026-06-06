/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibGC/Root.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Set.h>
#include <LibWeb/CSS/Invalidation/CustomElementInvalidator.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CustomStateSet);

GC::Ref<CustomStateSet> CustomStateSet::create(JS::Realm& realm, GC::Ref<DOM::Element> element)
{
    return realm.create<CustomStateSet>(realm, element);
}

CustomStateSet::CustomStateSet(JS::Realm& realm, GC::Ref<DOM::Element> element)
    : Bindings::Wrappable(realm)
    , m_element(element)
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

static void prune_live_set_entries(Vector<GC::Weak<JS::Set>>& live_set_entries)
{
    live_set_entries.remove_all_matching([](auto const& set_entries) { return !set_entries; });
}

template<typename Callback>
static void for_each_live_set_entries(GC::Weak<JS::Set> const& relevant_realm_set_entries, Vector<GC::Weak<JS::Set>>& live_set_entries, Callback callback)
{
    if (relevant_realm_set_entries) {
        auto set_entries = GC::make_root(*relevant_realm_set_entries);
        callback(*set_entries);
    }

    prune_live_set_entries(live_set_entries);
    for (auto const& weak_set_entries : live_set_entries) {
        auto set_entries = GC::make_root(*weak_set_entries);
        callback(*set_entries);
    }
}

static void add_state_to_set(JS::Set& set_entries, FlyString const& state)
{
    auto& realm = set_entries.shape().realm();
    auto value = GC::make_root(JS::PrimitiveString::create(realm.vm(), state));
    set_entries.set_add(JS::Value { value.ptr() });
}

static void remove_state_from_set(JS::Set& set_entries, FlyString const& state)
{
    auto& realm = set_entries.shape().realm();
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

GC::Ref<JS::Set> CustomStateSet::set_entries_for_realm(JS::Realm& realm) const
{
    if (&realm == &this->realm()) {
        if (m_relevant_realm_set_entries)
            return GC::Ref { *m_relevant_realm_set_entries };

        auto set_entries = create_set_entries(realm, m_states);
        m_relevant_realm_set_entries = GC::Ref { *set_entries };
        return GC::Ref { *set_entries };
    }

    prune_live_set_entries(m_live_set_entries);
    for (auto const& weak_set_entries : m_live_set_entries) {
        auto set_entries = GC::make_root(*weak_set_entries);
        if (&set_entries->shape().realm() == &realm)
            return GC::Ref { *set_entries };
    }

    auto set_entries = create_set_entries(realm, m_states);
    m_live_set_entries.append(GC::Ref { *set_entries });
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
    for_each_live_set_entries(m_relevant_realm_set_entries, m_live_set_entries, [&](auto& set_entries) {
        add_state_to_set(set_entries, state);
    });
}

bool CustomStateSet::set_remove(JS::Value value)
{
    auto state = state_from_set_value(value);
    auto removed = m_states.remove(state);
    if (removed) {
        for_each_live_set_entries(m_relevant_realm_set_entries, m_live_set_entries, [&](auto& set_entries) {
            remove_state_from_set(set_entries, state);
        });
    }
    return removed;
}

void CustomStateSet::set_clear()
{
    m_states.clear();
    for_each_live_set_entries(m_relevant_realm_set_entries, m_live_set_entries, [&](auto& set_entries) {
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
