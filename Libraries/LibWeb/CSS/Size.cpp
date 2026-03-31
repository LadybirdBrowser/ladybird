/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Size.h>
#include <LibWeb/CSS/StyleValues/FitContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>

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

Size Size::from_style_value(NonnullRefPtr<StyleValue const> const& value)
{
    if (value->is_keyword()) {
        switch (value->to_keyword()) {
        case Keyword::Auto:
            return Size::make_auto();
        case Keyword::MinContent:
            return Size::make_min_content();
        case Keyword::MaxContent:
            return Size::make_max_content();
        case Keyword::None:
            return Size::make_none();
        default:
            VERIFY_NOT_REACHED();
        }
    }
    if (value->is_fit_content()) {
        auto const& fit_content = value->as_fit_content();
        if (auto length_percentage = fit_content.length_percentage(); length_percentage.has_value())
            return Size::make_fit_content(length_percentage.release_value());
        return Size::make_fit_content();
    }

    if (value->is_calculated())
        return Size::make_calculated(value->as_calculated());

    if (value->is_percentage())
        return Size::make_percentage(value->as_percentage().percentage());

    if (value->is_length())
        return Size::make_length(value->as_length().length());

    // FIXME: Support `anchor-size(..)`
    if (value->is_anchor_size())
        return Size::make_none();

    dbgln("FIXME: Unsupported size value: `{}`, treating as `auto`", value->to_string(SerializationMode::Normal));
    return Size::make_auto();
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
