/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGNumber.h>
#include <LibWeb/SVG/SVGNumberList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGNumber);

GC::Ref<SVGNumber> SVGNumber::create(float value, ReadOnly read_only)
{
    return GC::Heap::the().allocate<SVGNumber>(value, read_only);
}

SVGNumber::SVGNumber(float value, ReadOnly read_only)
    : m_value(value)
    , m_read_only(read_only)
{
}

void SVGNumber::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_list);
}

// https://svgwg.org/svg2-draft/types.html#TermAttach
void SVGNumber::attach(GC::Ref<SVGNumberList> list, ReadOnly read_only)
{
    // Associate the SVGNumber with the element that the list interface object is associated with. Additionally,
    // depending on which IDL attribute the list interface object is reflected through:
    // baseVal: Set the SVGNumber to reflect an element of the base value.
    // animVal: Set the SVGNumber to reflect an element of the base value.
    // NB: SVGNumber objects reflected through the animVal IDL attribute are always read only.
    m_list = list;
    m_read_only = read_only;
}

// https://svgwg.org/svg2-draft/types.html#TermDetach
void SVGNumber::detach()
{
    // Set the SVGNumber to no longer be associated with any element. If the SVGNumber is read only, set it to be no
    // longer read only.
    m_list = nullptr;
    m_read_only = ReadOnly::No;
}

// https://www.w3.org/TR/SVG2/types.html#__svg__SVGNumber__value
WebIDL::ExceptionOr<void> SVGNumber::set_value(float value)
{
    // 1. If the SVGNumber is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create("Cannot modify value of read-only SVGNumber"_utf16);

    // 2. Set the SVGNumber's value to the value being assigned to the value member.
    m_value = value;

    // 3. If the SVGNumber reflects an element of the base value of a reflected attribute, then reserialize the
    //    reflected attribute.
    if (m_list)
        m_list->reserialize_reflected_attribute();

    return {};
}

}
