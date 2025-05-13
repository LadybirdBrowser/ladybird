/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSPageRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSPageRule.h>
#include <LibWeb/CSS/DescriptorID.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSPageRule);

GC::Ref<CSSPageRule> CSSPageRule::create(JS::Realm& realm, SelectorList&& selectors, GC::Ref<CSSPageDescriptors> style, CSSRuleList& rules)
{
    return realm.create<CSSPageRule>(realm, move(selectors), style, rules);
}

CSSPageRule::CSSPageRule(JS::Realm& realm, SelectorList&& selectors, GC::Ref<CSSPageDescriptors> style, CSSRuleList& rules)
    : CSSGroupingRule(realm, rules, Type::Page)
    , m_selectors(move(selectors))
    , m_style(style)
{
    m_style->set_parent_rule(*this);
}

void CSSPageRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSPageRule);
    Base::initialize(realm);
}

// https://drafts.csswg.org/cssom/#dom-csspagerule-selectortext
String CSSPageRule::selector_text() const
{
    // The selectorText attribute, on getting, must return the result of serializing the associated selector list.
    return serialize_a_group_of_selectors(m_selectors);
}

// https://drafts.csswg.org/cssom/#dom-csspagerule-selectortext
void CSSPageRule::set_selector_text(StringView)
{
    // FIXME: On setting the selectorText attribute these steps must be run:
    //  1. Run the parse a list of CSS page selectors algorithm on the given value.
    //  2. If the algorithm returns a non-null value replace the associated selector list with the returned value.
    //  3. Otherwise, if the algorithm returns a null value, do nothing.
}

// https://drafts.csswg.org/cssom/#ref-for-csspagerule
String CSSPageRule::serialized() const
{
    auto& descriptors = *m_style;

    StringBuilder builder;

    // AD-HOC: There's no spec for this yet.
    builder.append("@page "sv);
    if (auto selector = selector_text(); !selector.is_empty())
        builder.appendff("{} ", selector);
    builder.append("{ "sv);
    if (descriptors.length() > 0) {
        builder.append(descriptors.serialized());
        builder.append(' ');
    }
    builder.append("}"sv);

    return builder.to_string_without_validation();
}

void CSSPageRule::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style);
}

}
