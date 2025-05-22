/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/Vector.h>

namespace Web::CSS {

enum class PagePseudoClass : u8 {
    Left,
    Right,
    First,
    Blank,
};
Optional<PagePseudoClass> page_pseudo_class_from_string(StringView);
StringView to_string(PagePseudoClass);

class PageSelector {
public:
    PageSelector(Optional<FlyString> name, Vector<PagePseudoClass>);

    Optional<FlyString> name() const { return m_name; }
    Vector<PagePseudoClass> const& pseudo_classes() const { return m_pseudo_classes; }
    String serialize() const;

private:
    Optional<FlyString> m_name;
    Vector<PagePseudoClass> m_pseudo_classes;
};
using PageSelectorList = Vector<PageSelector>;

}
