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
            String text;
            switch (keyword) {
            case CSS::CounterStyleNameKeyword::Square:
            case CSS::CounterStyleNameKeyword::Circle:
            case CSS::CounterStyleNameKeyword::Disc:
            case CSS::CounterStyleNameKeyword::DisclosureClosed:
            case CSS::CounterStyleNameKeyword::DisclosureOpen:
            case CSS::CounterStyleNameKeyword::None:
                return {};
            case CSS::CounterStyleNameKeyword::Decimal:
                text = String::number(index);
                break;
            case CSS::CounterStyleNameKeyword::DecimalLeadingZero:
                // This is weird, but in accordance to spec.
                text = index < 10 ? MUST(String::formatted("0{}", index)) : String::number(index);
                break;
            case CSS::CounterStyleNameKeyword::LowerAlpha:
            case CSS::CounterStyleNameKeyword::LowerLatin:
                text = String::bijective_base_from(index - 1, String::Case::Lower);
                break;
            case CSS::CounterStyleNameKeyword::UpperAlpha:
            case CSS::CounterStyleNameKeyword::UpperLatin:
                text = String::bijective_base_from(index - 1, String::Case::Upper);
                break;
            case CSS::CounterStyleNameKeyword::LowerGreek:
                text = String::greek_letter_from(index);
                break;
            case CSS::CounterStyleNameKeyword::LowerRoman:
                text = String::roman_number_from(index, String::Case::Lower);
                break;
            case CSS::CounterStyleNameKeyword::UpperRoman:
                text = String::roman_number_from(index, String::Case::Upper);
                break;
            default:
                VERIFY_NOT_REACHED();
            }
            return MUST(String::formatted("{}. ", text));
        },
        [](String const& string) -> Optional<String> {
            return string;
        });
}

GC::Ptr<Painting::Paintable> ListItemMarkerBox::create_paintable() const
{
    return Painting::MarkerPaintable::create(*this);
}

CSSPixels ListItemMarkerBox::relative_size() const
{
    auto font_size = first_available_font().pixel_size();
    auto marker_text = text();
    if (marker_text.has_value())
        return CSSPixels::nearest_value_for(font_size);

    // Scale the marker box relative to the used font's pixel size.
    switch (m_list_style_type.get<CSS::CounterStyleNameKeyword>()) {
    case CSS::CounterStyleNameKeyword::DisclosureClosed:
    case CSS::CounterStyleNameKeyword::DisclosureOpen:
        return CSSPixels::nearest_value_for(ceilf(font_size * .5f));
    default:
        return CSSPixels::nearest_value_for(ceilf(font_size * .35f));
    }
}

void ListItemMarkerBox::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_list_item_element);
}

}
