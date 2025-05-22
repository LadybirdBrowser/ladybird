/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Resolution.h>

namespace Web::CSS {

Resolution::Resolution(double value, Type type)
    : m_type(type)
    , m_value(value)
{
}

Resolution Resolution::make_dots_per_pixel(double value)
{
    return { value, Type::Dppx };
}

String Resolution::to_string(SerializationMode serialization_mode) const
{
    if (serialization_mode == SerializationMode::ResolvedValue)
        return MUST(String::formatted("{}dppx", to_dots_per_pixel()));
    return MUST(String::formatted("{}{}", raw_value(), unit_name()));
}

double Resolution::to_dots_per_pixel() const
{
    switch (m_type) {
    case Type::Dpi:
        return m_value / 96; // 1in = 2.54cm = 96px
    case Type::Dpcm:
        return m_value / (96.0 / 2.54); // 1cm = 96px/2.54
    case Type::Dppx:
    case Type::X:
        return m_value;
    }
    VERIFY_NOT_REACHED();
}

StringView Resolution::unit_name() const
{
    switch (m_type) {
    case Type::Dpi:
        return "dpi"sv;
    case Type::Dpcm:
        return "dpcm"sv;
    case Type::Dppx:
        return "dppx"sv;
    case Type::X:
        return "x"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Resolution::Type> Resolution::unit_from_name(StringView name)
{
    if (name.equals_ignoring_ascii_case("dpi"sv))
        return Type::Dpi;
    if (name.equals_ignoring_ascii_case("dpcm"sv))
        return Type::Dpcm;
    if (name.equals_ignoring_ascii_case("dppx"sv))
        return Type::Dppx;
    if (name.equals_ignoring_ascii_case("x"sv))
        return Type::X;
    return {};
}

}
