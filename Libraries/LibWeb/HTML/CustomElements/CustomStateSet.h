/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/custom-elements.html#customstateset
class CustomStateSet final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(CustomStateSet, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(CustomStateSet);

public:
    [[nodiscard]] static GC::Ref<CustomStateSet> create(GC::Ref<DOM::Element>);
    virtual ~CustomStateSet() override = default;

    size_t set_size() const { return m_states.size(); }
    OrderedHashTable<Utf16FlyString> const& states() const { return m_states; }
    bool has_state(Utf16FlyString const&) const;
    void add_state(Utf16FlyString const&);
    bool remove_state(Utf16FlyString const&);
    void clear_states();
    DOM::Element& element() { return *m_element; }

private:
    CustomStateSet(GC::Ref<DOM::Element>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    OrderedHashTable<Utf16FlyString> m_states;
    GC::Ref<DOM::Element> m_element;
};

}
