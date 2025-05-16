/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PageSelector.h>

namespace Web::CSS {

Optional<PagePseudoClass> page_pseudo_class_from_string(StringView input)
{
    if (input.equals_ignoring_ascii_case("blank"sv))
        return PagePseudoClass::Blank;
    if (input.equals_ignoring_ascii_case("first"sv))
        return PagePseudoClass::First;
    if (input.equals_ignoring_ascii_case("left"sv))
        return PagePseudoClass::Left;
    if (input.equals_ignoring_ascii_case("right"sv))
        return PagePseudoClass::Right;
    return {};
}

StringView to_string(PagePseudoClass pseudo_class)
{
    switch (pseudo_class) {
    case PagePseudoClass::Blank:
        return "blank"sv;
    case PagePseudoClass::First:
        return "first"sv;
    case PagePseudoClass::Left:
        return "left"sv;
    case PagePseudoClass::Right:
        return "right"sv;
    }
    VERIFY_NOT_REACHED();
}

PageSelector::PageSelector(Optional<FlyString> name, Vector<PagePseudoClass> pseudo_classes)
    : m_name(move(name))
    , m_pseudo_classes(move(pseudo_classes))
{
}

String PageSelector::serialize() const
{
    StringBuilder builder;
    if (m_name.has_value())
        builder.append(m_name.value());
    for (auto pseudo_class : m_pseudo_classes)
        builder.appendff(":{}", to_string(pseudo_class));
    return builder.to_string_without_validation();
}

}
