/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/AvailableSpace.h>
#include <math.h>

namespace Web::Layout {

AvailableSize AvailableSize::make_definite(CSSPixels value)
{
    VERIFY(!value.might_be_saturated());
    return AvailableSize { Type::Definite, value };
}

AvailableSize AvailableSize::make_indefinite()
{
    return AvailableSize { Type::Indefinite, CSSPixels::max() };
}

AvailableSize AvailableSize::make_min_content()
{
    return AvailableSize { Type::MinContent, 0 };
}

AvailableSize AvailableSize::make_max_content()
{
    return AvailableSize { Type::MaxContent, CSSPixels::max() };
}

String AvailableSize::to_string() const
{
    switch (m_type) {
    case Type::Definite:
        return MUST(String::formatted("definite({})", m_value));
    case Type::Indefinite:
        return "indefinite"_string;
    case Type::MinContent:
        return "min-content"_string;
    case Type::MaxContent:
        return "max-content"_string;
    }
    VERIFY_NOT_REACHED();
}

String AvailableSpace::to_string() const
{
    return MUST(String::formatted("{} x {}", width, height));
}

AvailableSize::AvailableSize(Type type, CSSPixels value)
    : m_type(type)
    , m_value(value)
{
}

}
