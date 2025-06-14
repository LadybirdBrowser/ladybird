/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSMarginRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSMarginRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMarginRule);

GC::Ref<CSSMarginRule> CSSMarginRule::create(JS::Realm& realm, FlyString name, GC::Ref<CSSStyleProperties> style)
{
    return realm.create<CSSMarginRule>(realm, move(name), style);
}

CSSMarginRule::CSSMarginRule(JS::Realm& realm, FlyString name, GC::Ref<CSSStyleProperties> style)
    : CSSRule(realm, Type::Margin)
    , m_name(name.to_ascii_lowercase())
    , m_style(style)
{
    m_style->set_parent_rule(*this);
}

void CSSMarginRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSMarginRule);
    Base::initialize(realm);
}

String CSSMarginRule::serialized() const
{
    // AD-HOC: There is currently no spec for serializing CSSMarginRule.
    StringBuilder builder;
    builder.appendff("@{} {{ ", m_name);
    if (m_style->length() > 0) {
        builder.append(m_style->serialized());
        builder.append(' ');
    }
    builder.append('}');

    return builder.to_string_without_validation();
}

void CSSMarginRule::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style);
}

// https://drafts.csswg.org/css-page-3/#syntax-page-selector
bool is_margin_rule_name(StringView name)
{
    return name.equals_ignoring_ascii_case("top-left-corner"_sv)
        || name.equals_ignoring_ascii_case("top-left"_sv)
        || name.equals_ignoring_ascii_case("top-center"_sv)
        || name.equals_ignoring_ascii_case("top-right"_sv)
        || name.equals_ignoring_ascii_case("top-right-corner"_sv)
        || name.equals_ignoring_ascii_case("bottom-left-corner"_sv)
        || name.equals_ignoring_ascii_case("bottom-left"_sv)
        || name.equals_ignoring_ascii_case("bottom-center"_sv)
        || name.equals_ignoring_ascii_case("bottom-right"_sv)
        || name.equals_ignoring_ascii_case("bottom-right-corner"_sv)
        || name.equals_ignoring_ascii_case("left-top"_sv)
        || name.equals_ignoring_ascii_case("left-middle"_sv)
        || name.equals_ignoring_ascii_case("left-bottom"_sv)
        || name.equals_ignoring_ascii_case("right-top"_sv)
        || name.equals_ignoring_ascii_case("right-middle"_sv)
        || name.equals_ignoring_ascii_case("right-bottom"_sv);
}

}
