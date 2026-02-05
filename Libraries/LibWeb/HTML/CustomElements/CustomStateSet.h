/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/SetIterator.h>
#include <LibWeb/Bindings/CustomStateSetPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/custom-elements.html#customstateset
class CustomStateSet final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CustomStateSet, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CustomStateSet);

public:
    [[nodiscard]] static GC::Ref<CustomStateSet> create(JS::Realm&, GC::Ref<DOM::Element>);
    virtual ~CustomStateSet() override = default;

    GC::Ref<JS::Set> set_entries() const { return m_set_entries; }
    bool has_state(FlyString const&) const;

    void on_set_modified_from_js(Badge<Bindings::CustomStateSetPrototype>);

private:
    CustomStateSet(JS::Realm&, GC::Ref<DOM::Element>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<JS::Set> m_set_entries;
    GC::Ref<DOM::Element> m_element;
};

}
