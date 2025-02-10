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

ListItemMarkerBox::ListItemMarkerBox(DOM::Document& document, CSS::ListStyleType style_type, CSS::ListStylePosition style_position, size_t index, GC::Ref<CSS::ComputedProperties> style)
    : Box(document, nullptr, move(style))
    , m_list_style_type(style_type)
    , m_list_style_position(style_position)
    , m_index(index)
{
    m_list_style_type.visit(
        [this](CSS::CounterStyleNameKeyword keyword) {
            switch (keyword) {
            case CSS::CounterStyleNameKeyword::Square:
            case CSS::CounterStyleNameKeyword::Circle:
            case CSS::CounterStyleNameKeyword::Disc:
            case CSS::CounterStyleNameKeyword::DisclosureClosed:
            case CSS::CounterStyleNameKeyword::DisclosureOpen:
                break;
            case CSS::CounterStyleNameKeyword::Decimal:
                m_text = MUST(String::formatted("{}.", m_index));
                break;
            case CSS::CounterStyleNameKeyword::DecimalLeadingZero:
                // This is weird, but in accordance to spec.
                m_text = m_index < 10 ? MUST(String::formatted("0{}.", m_index)) : MUST(String::formatted("{}.", m_index));
                break;
            case CSS::CounterStyleNameKeyword::LowerAlpha:
            case CSS::CounterStyleNameKeyword::LowerLatin:
                m_text = String::bijective_base_from(m_index - 1, String::Case::Lower);
                break;
            case CSS::CounterStyleNameKeyword::UpperAlpha:
            case CSS::CounterStyleNameKeyword::UpperLatin:
                m_text = String::bijective_base_from(m_index - 1, String::Case::Upper);
                break;
            case CSS::CounterStyleNameKeyword::LowerRoman:
                m_text = String::roman_number_from(m_index, String::Case::Lower);
                break;
            case CSS::CounterStyleNameKeyword::UpperRoman:
                m_text = String::roman_number_from(m_index, String::Case::Upper);
                break;
            case CSS::CounterStyleNameKeyword::None:
                break;
            }
        },
        [this](String const& string) {
            m_text = string;
        });
}

ListItemMarkerBox::~ListItemMarkerBox() = default;

GC::Ptr<Painting::Paintable> ListItemMarkerBox::create_paintable() const
{
    return Painting::MarkerPaintable::create(*this);
}

}
