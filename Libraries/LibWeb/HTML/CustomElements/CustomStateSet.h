/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/CustomStateSet.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/custom-elements.html#customstateset
class CustomStateSet final : public Bindings::Wrappable {
    WEB_WRAPPABLE(CustomStateSet, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CustomStateSet);

public:
    [[nodiscard]] static GC::Ref<CustomStateSet> create(GC::Ref<DOM::Element>);
    virtual ~CustomStateSet() override = default;

    size_t set_size() const { return m_states.size(); }
    GC::Ref<JS::Set> set_entries(JS::Realm&, Bindings::WrapperWorld const&) const;
    bool set_has(JS::Value) const;
    void set_add(JS::Value);
    bool set_remove(JS::Value);
    void set_clear();
    bool has_state(FlyString const&) const;

    void on_set_modified_from_js(Badge<Bindings::CustomStateSetPrototype>);

private:
    CustomStateSet(GC::Ref<DOM::Element>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    OrderedHashTable<FlyString> m_states;
    mutable Bindings::WrapperWorldWeakValueCache<JS::Set> m_set_entries;
    GC::Ref<DOM::Element> m_element;
};

}
