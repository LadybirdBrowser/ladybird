/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGStringList.h>
#include <LibWebCommon/Infra/CharacterTypes.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGStringList);

GC::Ref<SVGStringList> SVGStringList::create(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, Separator separator)
{
    return GC::Heap::the().allocate<SVGStringList>(element, move(reflected_attribute), separator);
}

SVGStringList::SVGStringList(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, Separator separator)
    : SVGList(ReadOnlyList::No)
    , m_element(element)
    , m_reflected_attribute(move(reflected_attribute))
    , m_separator(separator)
{
    synchronize();
}

void SVGStringList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

Vector<Utf16String> SVGStringList::parse(Utf16View input, Separator separator)
{
    Vector<Utf16String> strings;
    if (separator == Separator::Whitespace) {
        // https://infra.spec.whatwg.org/#split-on-ascii-whitespace
        size_t position = 0;
        auto length = input.length_in_code_units();
        auto is_ascii_whitespace = [&](size_t index) { return Infra::ASCII_WHITESPACE.contains(static_cast<u32>(input.code_unit_at(index))); };
        while (position < length) {
            while (position < length && is_ascii_whitespace(position))
                ++position;
            auto start = position;
            while (position < length && !is_ascii_whitespace(position))
                ++position;
            if (position > start)
                strings.append(Utf16String::from_utf16(input.substring_view(start, position - start)));
        }
        return strings;
    }

    // https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#comma-separated-tokens
    // A set of comma-separated tokens is a string containing zero or more tokens each separated from the next by a
    // single U+002C COMMA character (,), where tokens consist of any string of zero or more characters, neither
    // beginning nor ending with ASCII whitespace, nor containing any U+002C COMMA characters (,), and optionally
    // surrounded by ASCII whitespace.
    // NB: This strictly splits the string on commas, and strips the ASCII whitespace around each token.
    size_t token_start = 0;
    for (size_t position = 0; position <= input.length_in_code_units(); ++position) {
        if (position < input.length_in_code_units() && input.code_unit_at(position) != ',')
            continue;
        strings.append(Utf16String::from_utf16(input.substring_view(token_start, position - token_start).trim(Infra::ASCII_WHITESPACE)));
        token_start = position + 1;
    }
    return strings;
}

// https://svgwg.org/svg2-draft/types.html#TermSynchronizeList
void SVGStringList::synchronize()
{
    // NB: The list does not need to be synchronized with a value that it has just reserialized itself.
    if (m_is_reserializing)
        return;

    // 1. Let value be the base value of the reflected content attribute (using the attribute's initial value if it is
    //    not present or invalid).
    // NB: The initial value of every attribute reflected by an SVGStringList is the empty list.
    auto value = m_element->attribute(m_reflected_attribute);

    // 5. Otherwise, the list element type is DOMString:
    //    1. Replace the list with a new list consisting of the values in value.
    mutable_items() = value.has_value() ? parse(*value, m_separator) : Vector<Utf16String> {};
}

// https://svgwg.org/svg2-draft/types.html#TermReserialize
void SVGStringList::reserialize_reflected_attribute()
{
    // 1. Let elements be the list of values in value.
    // 2. Let serialized elements be a list of strings, where each string is formed based on the corresponding value in
    //    elements and its type:
    //    a DOMString: The string is the DOMString's value itself.
    // 3. Set the content attribute to a string consisting of the strings in serialized elements joined and separated by
    //    single U+0020 SPACE characters.
    // AD-HOC: Strings of an attribute that is a set of comma-separated tokens are joined with commas instead, so that
    //         parsing the attribute again returns the same list.
    Utf16StringBuilder builder;
    bool first = true;
    for (auto const& item : items()) {
        if (!first)
            builder.append_ascii(m_separator == Separator::Comma ? ", "sv : " "sv);
        first = false;
        builder.append(item);
    }

    TemporaryChange change { m_is_reserializing, true };
    m_element->set_attribute_value(m_reflected_attribute, builder.to_string());
}

}
