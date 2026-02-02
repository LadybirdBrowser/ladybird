/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Size.h>

namespace Web::CSS {

Size::Size(Type type, Optional<LengthPercentage> length_percentage)
    : m_type(type)
    , m_length_percentage(move(length_percentage))
{
}

CSSPixels Size::to_px(Layout::Node const& node, CSSPixels reference_value) const
{
    if (!m_length_percentage.has_value())
        return 0;
    return m_length_percentage->resolved(node, reference_value).to_px(node);
}

Size Size::make_auto()
{
    return Size { Type::Auto };
}

Size Size::make_px(CSSPixels px)
{
    return make_length(Length::make_px(px));
}

Size Size::make_length(Length length)
{
    return Size { Type::Length, move(length) };
}

Size Size::make_percentage(Percentage percentage)
{
    return Size { Type::Percentage, move(percentage) };
}

Size Size::make_calculated(NonnullRefPtr<CalculatedStyleValue const> calculated)
{
    return Size { Type::Calculated, move(calculated) };
}

Size Size::make_length_percentage(LengthPercentage const& length_percentage)
{
    if (length_percentage.is_length())
        return make_length(length_percentage.length());
    if (length_percentage.is_percentage())
        return make_percentage(length_percentage.percentage());
    VERIFY(length_percentage.is_calculated());
    return make_calculated(length_percentage.calculated());
}

Size Size::make_min_content()
{
    return Size { Type::MinContent };
}

Size Size::make_max_content()
{
    return Size { Type::MaxContent };
}

Size Size::make_fit_content(LengthPercentage available_space)
{
    return Size { Type::FitContent, move(available_space) };
}

Size Size::make_fit_content()
{
    return Size { Type::FitContent };
}

Size Size::make_none()
{
    return Size { Type::None };
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
        return m_length_percentage.has_value() && m_length_percentage->contains_percentage();
    default:
        return m_length_percentage->contains_percentage();
    }
}

void Size::serialize(StringBuilder& builder, SerializationMode mode) const
{
    switch (m_type) {
    case Type::Auto:
        builder.append("auto"sv);
        break;
    case Type::Calculated:
    case Type::Length:
    case Type::Percentage:
        m_length_percentage->serialize(builder, mode);
        break;
    case Type::MinContent:
        builder.append("min-content"sv);
        break;
    case Type::MaxContent:
        builder.append("max-content"sv);
        break;
    case Type::FitContent:
        if (!m_length_percentage.has_value()) {
            builder.append("fit-content"sv);
        } else {
            builder.append("fit-content("sv);
            m_length_percentage->serialize(builder, mode);
            builder.append(")"sv);
        }
        break;
    case Type::None:
        builder.append("none"sv);
        break;
    }
}

String Size::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    serialize(builder, mode);
    return MUST(builder.to_string());
}

}
