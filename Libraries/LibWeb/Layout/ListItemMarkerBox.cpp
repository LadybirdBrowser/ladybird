/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
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
        [](Empty const&) -> Optional<String> {
            return {};
        },
        [index](CSS::CounterStyleNameKeyword keyword) -> Optional<String> {
            String text;
            switch (keyword) {
            case CSS::CounterStyleNameKeyword::Square:
            case CSS::CounterStyleNameKeyword::Circle:
            case CSS::CounterStyleNameKeyword::Disc:
            case CSS::CounterStyleNameKeyword::DisclosureClosed:
            case CSS::CounterStyleNameKeyword::DisclosureOpen:
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
    VERIFY(!m_list_style_type.has<Empty>());

    auto font_size = first_available_font().pixel_size();
    auto marker_text = text();
    if (marker_text.has_value())
        return CSSPixels::nearest_value_for(font_size);

    // https://drafts.csswg.org/css-counter-styles-3/#simple-symbolic
    // NB: The spec allows us to render some predefined symbol counter styles using a UA-generated image instead of
    //     text, it instructs us to size these in order to attractively fit within a 1em x 1em square. We mimic Firefox
    //     and generally use a size of 0.35em, except for disclosure open/closed styles which use a size of 0.5em.
    static constexpr float marker_image_size_factor = 0.35f;
    static constexpr float disclosure_marker_image_size_factor = 0.5f;

    // Scale the marker box relative to the used font's pixel size.
    switch (m_list_style_type.get<CSS::CounterStyleNameKeyword>()) {
    case CSS::CounterStyleNameKeyword::DisclosureClosed:
    case CSS::CounterStyleNameKeyword::DisclosureOpen:
        return CSSPixels::nearest_value_for(ceilf(font_size * disclosure_marker_image_size_factor));
    default:
        return CSSPixels::nearest_value_for(ceilf(font_size * marker_image_size_factor));
    }
}

void ListItemMarkerBox::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_list_item_element);
}

}
