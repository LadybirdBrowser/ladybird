/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGNumber.h>
#include <LibWeb/SVG/SVGNumberList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGNumberList);

GC::Ref<SVGNumberList> SVGNumberList::create(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGNumberList>(element, move(reflected_attribute), read_only);
}

SVGNumberList::SVGNumberList(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, ReadOnlyList read_only)
    : SVGList(read_only)
    , m_element(element)
    , m_reflected_attribute(move(reflected_attribute))
{
    synchronize();
}

void SVGNumberList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
    visitor.visit(m_element);
}

void SVGNumberList::attach(GC::Ref<SVGNumber> const& number)
{
    number->attach(*this, read_only() == ReadOnlyList::Yes ? SVGNumber::ReadOnly::Yes : SVGNumber::ReadOnly::No);
}

void SVGNumberList::detach(GC::Ref<SVGNumber> const& number)
{
    number->detach();
}

GC::Ref<SVGNumber> SVGNumberList::copy_if_not_detached(GC::Ref<SVGNumber> number)
{
    if (number->is_detached())
        return number;
    return SVGNumber::create(number->value(), SVGNumber::ReadOnly::No);
}

// https://svgwg.org/svg2-draft/types.html#TermSynchronizeList
void SVGNumberList::synchronize()
{
    // NB: The list does not need to be synchronized with a value that it has just reserialized itself.
    if (m_is_reserializing)
        return;

    // 1. Let value be the base value of the reflected content attribute (using the attribute's initial value if it is
    //    not present or invalid).
    // NB: The initial value of every attribute reflected by an SVGNumberList is the empty list, which is also what an
    //     invalid list of numbers parses to.
    auto value = parse_table_values(m_element->get_attribute_value(m_reflected_attribute));

    synchronize_items(value, [] { return SVGNumber::create(0, SVGNumber::ReadOnly::No); }, [](GC::Ref<SVGNumber> const& item, float number) { item->set_value_from_reflected_attribute(number); });
}

// https://svgwg.org/svg2-draft/types.html#TermReserialize
void SVGNumberList::reserialize_reflected_attribute()
{
    // 1. Let elements be the list of values in value.
    // 2. Let serialized elements be a list of strings, where each string is formed based on the corresponding value in
    //    elements and its type:
    //    an SVGNumber object: The string is an implementation specific string that, if parsed as a <number> using CSS
    //    syntax, would return the number value closest to the SVGNumber object's value member, given the
    //    implementation's supported real number precision.
    // 3. Set the content attribute to a string consisting of the strings in serialized elements joined and separated by
    //    single U+0020 SPACE characters.
    Utf16StringBuilder builder;
    for (size_t index = 0; index < items().size(); ++index) {
        if (index > 0)
            builder.append_code_unit(' ');
        CSS::serialize_a_number(builder, items()[index]->value());
    }

    TemporaryChange change { m_is_reserializing, true };
    m_element->set_attribute_value(m_reflected_attribute, builder.to_string());
}

}
