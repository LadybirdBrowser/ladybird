/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Painting/MarkerPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(ListItemMarkerBox);

ListItemMarkerBox::ListItemMarkerBox(DOM::Document& document, CSS::ListStyleType style_type, CSS::ListStylePosition style_position, GC::Ref<DOM::Element> list_item_element, GC::Ref<CSS::ComputedProperties> style)
    : Box(document, nullptr, move(style))
    , m_list_style_type(style_type)
    , m_list_style_position(style_position)
    , m_list_item_element(list_item_element)
{
}

ListItemMarkerBox::~ListItemMarkerBox() = default;

Optional<String> ListItemMarkerBox::text() const
{
    auto index = m_list_item_element->ordinal_value();

    return m_list_style_type.visit(
        [index](CSS::CounterStyleNameKeyword keyword) -> Optional<String> {
            switch (keyword) {
            case CSS::CounterStyleNameKeyword::Square:
            case CSS::CounterStyleNameKeyword::Circle:
            case CSS::CounterStyleNameKeyword::Disc:
            case CSS::CounterStyleNameKeyword::DisclosureClosed:
            case CSS::CounterStyleNameKeyword::DisclosureOpen:
                return {};
            case CSS::CounterStyleNameKeyword::Decimal:
                return MUST(String::formatted("{}.", index));
            case CSS::CounterStyleNameKeyword::DecimalLeadingZero:
                // This is weird, but in accordance to spec.
                return MUST(index < 10 ? String::formatted("0{}.", index) : String::formatted("{}.", index));
            case CSS::CounterStyleNameKeyword::LowerAlpha:
            case CSS::CounterStyleNameKeyword::LowerLatin:
                return String::bijective_base_from(index - 1, String::Case::Lower);
            case CSS::CounterStyleNameKeyword::UpperAlpha:
            case CSS::CounterStyleNameKeyword::UpperLatin:
                return String::bijective_base_from(index - 1, String::Case::Upper);
            case CSS::CounterStyleNameKeyword::LowerRoman:
                return String::roman_number_from(index, String::Case::Lower);
            case CSS::CounterStyleNameKeyword::UpperRoman:
                return String::roman_number_from(index, String::Case::Upper);
            case CSS::CounterStyleNameKeyword::None:
                return {};
            default:
                VERIFY_NOT_REACHED();
            }
        },
        [](String const& string) -> Optional<String> {
            return string;
        });
}

GC::Ptr<Painting::Paintable> ListItemMarkerBox::create_paintable() const
{
    return Painting::MarkerPaintable::create(*this);
}

void ListItemMarkerBox::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_list_item_element);
}
}
