/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGLengthList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLengthList);

GC::Ref<SVGLengthList> SVGLengthList::create(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, SVGLength::Directionality directionality, ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGLengthList>(element, move(reflected_attribute), directionality, read_only);
}

SVGLengthList::SVGLengthList(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, SVGLength::Directionality directionality, ReadOnlyList read_only)
    : SVGList(read_only)
    , m_element(element)
    , m_reflected_attribute(move(reflected_attribute))
    , m_directionality(directionality)
{
    synchronize();
}

void SVGLengthList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
    visitor.visit(m_element);
}

void SVGLengthList::attach(GC::Ref<SVGLength> const& length)
{
    length->attach(*this, m_element, m_directionality, read_only() == ReadOnlyList::Yes ? SVGLength::ReadOnly::Yes : SVGLength::ReadOnly::No);
}

void SVGLengthList::detach(GC::Ref<SVGLength> const& length)
{
    length->detach();
}

GC::Ref<SVGLength> SVGLengthList::copy_if_not_detached(GC::Ref<SVGLength> length)
{
    if (length->is_detached())
        return length;
    return length->create_detached_copy();
}

// Parses a list of lengths, percentages and numbers that are separated by commas and/or whitespace.
static Vector<SVGLengthValue> parse_length_list(Utf16View input)
{
    Vector<SVGLengthValue> values;
    auto is_separator = [](char16_t code_unit) { return code_unit == ',' || code_unit == ' ' || code_unit == '\t' || code_unit == '\n' || code_unit == '\r' || code_unit == '\f'; };

    size_t position = 0;
    auto length = input.length_in_code_units();
    while (true) {
        // Skip whitespace, and at most one comma.
        bool seen_comma = false;
        while (position < length && is_separator(input.code_unit_at(position))) {
            if (input.code_unit_at(position) == ',') {
                // A comma must separate two values.
                if (seen_comma || values.is_empty())
                    return {};
                seen_comma = true;
            }
            ++position;
        }
        if (position >= length) {
            // A trailing comma is invalid.
            if (seen_comma)
                return {};
            break;
        }

        auto start = position;
        while (position < length && !is_separator(input.code_unit_at(position)))
            ++position;
        auto value = SVGLength::parse_scalar_value(input.substring_view(start, position - start));
        if (!value.has_value())
            return {};
        values.append(*value);
    }
    return values;
}

// https://svgwg.org/svg2-draft/types.html#TermSynchronizeList
void SVGLengthList::synchronize()
{
    // NB: The list does not need to be synchronized with a value that it has just reserialized itself.
    if (m_is_reserializing)
        return;

    // 1. Let value be the base value of the reflected content attribute (using the attribute's initial value if it is
    //    not present or invalid).
    // NB: The initial value of every attribute reflected by an SVGLengthList is the empty list.
    auto value = parse_length_list(m_element->get_attribute_value(m_reflected_attribute));

    auto& realm = m_element->document().relevant_settings_object().realm();
    synchronize_items(value, [&] { return SVGLength::create_detached(realm, SVGLengthValue::number(0), SVGLength::ReadOnly::No); }, [](GC::Ref<SVGLength> const& item, SVGLengthValue const& length) { item->set_value_from_reflected_attribute(length); });
}

// https://svgwg.org/svg2-draft/types.html#TermReserialize
void SVGLengthList::reserialize_reflected_attribute()
{
    // 1. Let elements be the list of values in value.
    // 2. Let serialized elements be a list of strings, where each string is formed based on the corresponding value in
    //    elements and its type:
    //    an SVGLength object: The string is the value that would be returned from getting the value's valueAsString
    //    member.
    // 3. Set the content attribute to a string consisting of the strings in serialized elements joined and separated by
    //    single U+0020 SPACE characters.
    Utf16StringBuilder builder;
    for (size_t index = 0; index < items().size(); ++index) {
        if (index > 0)
            builder.append_code_unit(' ');
        builder.append(items()[index]->value_as_string());
    }

    TemporaryChange change { m_is_reserializing, true };
    m_element->set_attribute_value(m_reflected_attribute, builder.to_string());
}

}
