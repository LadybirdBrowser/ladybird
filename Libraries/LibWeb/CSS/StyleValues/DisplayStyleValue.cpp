/*
 * Copyright (c) 2023, Emil Militzer <emil.militzer@posteo.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DisplayStyleValue.h"
#include <LibWeb/CSS/CSSKeywordValue.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<DisplayStyleValue const> DisplayStyleValue::create(Display const& display)
{
    return adopt_ref(*new (nothrow) DisplayStyleValue(display));
}

GC::Ref<CSSStyleValue> DisplayStyleValue::reify(JS::Realm& realm, FlyString const& associated_property) const
{
    if (auto keyword = m_display.to_keyword(); keyword.has_value())
        return CSSKeywordValue::create(realm, FlyString::from_utf8_without_validation(string_from_keyword(keyword.value()).bytes()));

    return CSSStyleValue::create(realm, associated_property, *this);
}

}
