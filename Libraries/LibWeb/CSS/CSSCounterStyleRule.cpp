/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSCounterStyleRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSCounterStyleRule.h>

namespace Web::CSS {

CSSCounterStyleRule::CSSCounterStyleRule(JS::Realm& realm, Type type)
    : CSSRule(realm, type)
{
}

void CSSCounterStyleRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSCounterStyleRule);
    Base::initialize(realm);
}

}
