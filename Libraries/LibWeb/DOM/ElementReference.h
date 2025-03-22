/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/DOM/Element.h>

namespace Web::DOM {

class ElementReference {
public:
    ElementReference(GC::Ref<Element> element, Optional<CSS::PseudoElement> pseudo_element = {})
        : m_element(element)
        , m_pseudo_element(move(pseudo_element))
    {
    }

    Element& element() { return m_element; }
    Element const& element() const { return m_element; }
    Optional<CSS::PseudoElement> pseudo_element() const { return m_pseudo_element; }

    void visit(GC::Cell::Visitor& visitor) const
    {
        visitor.visit(m_element);
    }

private:
    GC::Ref<Element> m_element;
    Optional<CSS::PseudoElement> m_pseudo_element;
};

}
