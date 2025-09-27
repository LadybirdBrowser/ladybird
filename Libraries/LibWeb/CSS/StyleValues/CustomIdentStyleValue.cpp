/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CustomIdentStyleValue.h"
#include <LibWeb/CSS/CSSKeywordValue.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>

namespace Web::CSS {

Vector<Parser::ComponentValue> CustomIdentStyleValue::tokenize() const
{
    return { Parser::Token::create_ident(m_custom_ident) };
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-ident
GC::Ref<CSSStyleValue> CustomIdentStyleValue::reify(JS::Realm& realm, FlyString const&) const
{
    // 1. Return a new CSSKeywordValue with its value internal slot set to the serialization of ident.
    return CSSKeywordValue::create(realm, m_custom_ident);
}

}
