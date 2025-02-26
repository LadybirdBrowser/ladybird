/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Size.h>

namespace Web::CSS {

Size::Size(Type type, LengthPercentage length_percentage)
    : m_type(type)
    , m_length_percentage(move(length_percentage))
{
}

CSSPixels Size::to_px(Layout::Node const& node, CSSPixels reference_value) const
{
    return m_length_percentage.resolved(node, reference_value).to_px(node);
}

Size Size::make_auto()
{
    return Size { Type::Auto, Length::make_auto() };
}

Size Size::make_px(CSSPixels px)
{
    return make_length(CSS::Length::make_px(px));
}

Size Size::make_length(Length length)
{
    return Size { Type::Length, move(length) };
}

Size Size::make_percentage(Percentage percentage)
{
    return Size { Type::Percentage, move(percentage) };
}

Size Size::make_calculated(NonnullRefPtr<Web::CSS::CalculatedStyleValue> calculated)
{
    return Size { Type::Calculated, move(calculated) };
}

Size Size::make_min_content()
{
    return Size { Type::MinContent, Length::make_auto() };
}

Size Size::make_max_content()
{
    return Size { Type::MaxContent, Length::make_auto() };
}

Size Size::make_fit_content(LengthPercentage available_space)
{
    return Size { Type::FitContent, move(available_space) };
}

Size Size::make_fit_content()
{
    // NOTE: We use "auto" as a stand-in for "stretch" here.
    return Size { Type::FitContent, Length::make_auto() };
}

Size Size::make_none()
{
    return Size { Type::None, Length::make_auto() };
}

bool Size::contains_percentage() const
{
    switch (m_type) {
    case Type::Auto:
    case Type::MinContent:
    case Type::MaxContent:
    case Type::None:
        return false;
    case Type::FitContent:
        // FIXME: This should return m_length_percentage.contains_percentage()
        //        but we have to update a lot of code to handle this.
        return false;
    default:
        return m_length_percentage.contains_percentage();
    }
}

String Size::to_string() const
{
    switch (m_type) {
    case Type::Auto:
        return "auto"_string;
    case Type::Calculated:
    case Type::Length:
    case Type::Percentage:
        return m_length_percentage.to_string();
    case Type::MinContent:
        return "min-content"_string;
    case Type::MaxContent:
        return "max-content"_string;
    case Type::FitContent:
        return MUST(String::formatted("fit-content({})", m_length_percentage.to_string()));
    case Type::None:
        return "none"_string;
    }
    VERIFY_NOT_REACHED();
}

}
