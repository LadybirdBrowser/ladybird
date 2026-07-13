/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>

namespace Web::CSS {

enum class PagePseudoClass : u8 {
    Left,
    Right,
    First,
    Blank,
};
Optional<PagePseudoClass> page_pseudo_class_from_string(Utf16View);
StringView to_string(PagePseudoClass);

class PageSelector {
public:
    PageSelector(Optional<Utf16FlyString> name, Vector<PagePseudoClass>);

    Optional<Utf16FlyString> name() const { return m_name; }
    Vector<PagePseudoClass> const& pseudo_classes() const { return m_pseudo_classes; }
    String serialize() const;
    void serialize_to(AK::Utf16StringBuilder&) const;

private:
    Optional<Utf16FlyString> m_name;
    Vector<PagePseudoClass> m_pseudo_classes;
};
using PageSelectorList = Vector<PageSelector>;

}
