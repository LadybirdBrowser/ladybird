/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AbstractImageStyleValue.h"
#include <LibWeb/CSS/CSSImageValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#reify-stylevalue
GC::Ref<CSSStyleValue> AbstractImageStyleValue::reify(JS::Realm& realm, FlyString const&) const
{
    // AD-HOC: There's no spec description of how to reify as a CSSImageValue.
    return CSSImageValue::create(realm, to_string(SerializationMode::Normal));
}

}
