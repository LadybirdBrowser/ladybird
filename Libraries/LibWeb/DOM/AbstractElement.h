/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// Either an Element or a PseudoElement
class AbstractElement {
public:
    AbstractElement(GC::Ref<Element>, Optional<CSS::PseudoElement> = {});

    Element& element() { return m_element; }
    Element const& element() const { return m_element; }
    Optional<CSS::PseudoElement> pseudo_element() const { return m_pseudo_element; }

    GC::Ptr<Element const> parent_element() const;
    GC::Ptr<CSS::ComputedProperties const> computed_properties() const;

    CSS::CountersSet& ensure_counters_set();
    void set_counters_set(OwnPtr<CSS::CountersSet>&&);

    void visit(GC::Cell::Visitor& visitor) const;

private:
    GC::Ref<Element> m_element;
    Optional<CSS::PseudoElement> m_pseudo_element;
};

}
