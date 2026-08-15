/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CustomIdentStyleValue.h"
#include <LibWeb/CSS/CSSKeywordValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#reify-ident
GC::Ref<CSSStyleValue> CustomIdentStyleValue::reify(Utf16FlyString const&) const
{
    // 1. Return a new CSSKeywordValue with its value internal slot set to the serialization of ident.
    return CSSKeywordValue::create(custom_ident());
}

}
