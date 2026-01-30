/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSCounterStyleRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSCounterStyleRule.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSCounterStyleRule);

GC::Ref<CSSCounterStyleRule> CSSCounterStyleRule::create(JS::Realm& realm, FlyString name)
{
    return realm.create<CSSCounterStyleRule>(realm, name);
}

CSSCounterStyleRule::CSSCounterStyleRule(JS::Realm& realm, FlyString name)
    : CSSRule(realm, Type::CounterStyle)
    , m_name(move(name))

{
}

String CSSCounterStyleRule::serialized() const
{
    StringBuilder builder;
    builder.appendff("@counter-style {} {{", serialize_an_identifier(m_name));
    builder.append(" }"sv);
    return MUST(builder.to_string());
}

// https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-name
void CSSCounterStyleRule::set_name(FlyString name)
{
    // On setting the name attribute, run the following steps:

    // 1. If the value is an ASCII case-insensitive match for "none" or one of the non-overridable counter-style names, do nothing and return.
    if (name.equals_ignoring_ascii_case("none"sv) || matches_non_overridable_counter_style_name(name))
        return;

    // 2. If the value is an ASCII case-insensitive match for any of the predefined counter styles, lowercase it.
    if (auto keyword = keyword_from_string(name); keyword.has_value() && keyword_to_counter_style_name_keyword(keyword.release_value()).has_value())
        name = name.to_ascii_lowercase();

    // 3. Replace the associated rule’s name with an identifier equal to the value.
    m_name = move(name);
}

void CSSCounterStyleRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSCounterStyleRule);
    Base::initialize(realm);
}

}
