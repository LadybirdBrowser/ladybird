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

Optional<PagePseudoClass> page_pseudo_class_from_string(StringView input)
{
    if (input.equals_ignoring_ascii_case("blank"sv))
        return PagePseudoClass::Blank;
    if (input.equals_ignoring_ascii_case("first"sv))
        return PagePseudoClass::First;
    if (input.equals_ignoring_ascii_case("left"sv))
        return PagePseudoClass::Left;
    if (input.equals_ignoring_ascii_case("right"sv))
        return PagePseudoClass::Right;
    return {};
}

StringView to_string(PagePseudoClass pseudo_class)
{
    switch (pseudo_class) {
    case PagePseudoClass::Blank:
        return "blank"sv;
    case PagePseudoClass::First:
        return "first"sv;
    case PagePseudoClass::Left:
        return "left"sv;
    case PagePseudoClass::Right:
        return "right"sv;
    }
    VERIFY_NOT_REACHED();
}

PageSelector::PageSelector(Optional<FlyString> name, Vector<PagePseudoClass> pseudo_classes)
    : m_name(move(name))
    , m_pseudo_classes(move(pseudo_classes))
{
}

String PageSelector::serialize() const
{
    StringBuilder builder;
    if (m_name.has_value())
        builder.append(m_name.value());
    for (auto pseudo_class : m_pseudo_classes)
        builder.appendff(":{}", to_string(pseudo_class));
    return builder.to_string_without_validation();
}

GC::Ref<CSSPageRule> CSSPageRule::create(JS::Realm& realm, PageSelectorList&& selectors, GC::Ref<CSSPageDescriptors> style, CSSRuleList& rules)
{
    return realm.create<CSSPageRule>(realm, move(selectors), style, rules);
}

CSSPageRule::CSSPageRule(JS::Realm& realm, PageSelectorList&& selectors, GC::Ref<CSSPageDescriptors> style, CSSRuleList& rules)
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

    // https://www.w3.org/TR/cssom/#serialize-a-group-of-selectors
    // To serialize a group of selectors serialize each selector in the group of selectors and then serialize a comma-separated list of these serializations.
    return MUST(String::join(", "sv, m_selectors));
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

    // AD-HOC: There's no spec for this yet, but Chrome puts declarations before margin rules.
    builder.append("@page "sv);
    if (auto selector = selector_text(); !selector.is_empty())
        builder.appendff("{} ", selector);
    builder.append("{ "sv);
    if (descriptors.length() > 0) {
        builder.append(descriptors.serialized());
        builder.append(' ');
    }
    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        auto result = rule->css_text();

        if (result.is_empty())
            continue;

        builder.appendff("{} ", result);
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
