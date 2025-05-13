/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSPageDescriptors.h>

namespace Web::CSS {

enum class PagePseudoClass : u8 {
    Left,
    Right,
    First,
    Blank,
};
Optional<PagePseudoClass> page_pseudo_class_from_string(StringView);
StringView to_string(PagePseudoClass);

class PageSelector {
public:
    PageSelector(Optional<FlyString> name, Vector<PagePseudoClass>);

    Optional<FlyString> name() const { return m_name; }
    Vector<PagePseudoClass> const& pseudo_classes() const { return m_pseudo_classes; }
    String serialize() const;

private:
    Optional<FlyString> m_name;
    Vector<PagePseudoClass> m_pseudo_classes;
};
using PageSelectorList = Vector<PageSelector>;

// https://drafts.csswg.org/css-page-3/#at-ruledef-page
class CSSPageRule final : public CSSGroupingRule {
    WEB_PLATFORM_OBJECT(CSSPageRule, CSSGroupingRule);
    GC_DECLARE_ALLOCATOR(CSSPageRule);

public:
    [[nodiscard]] static GC::Ref<CSSPageRule> create(JS::Realm&, PageSelectorList&&, GC::Ref<CSSPageDescriptors>, CSSRuleList&);

    virtual ~CSSPageRule() override = default;

    String selector_text() const;
    void set_selector_text(StringView);

    GC::Ref<CSSPageDescriptors> style() { return m_style; }
    GC::Ref<CSSPageDescriptors const> descriptors() const { return m_style; }

private:
    CSSPageRule(JS::Realm&, PageSelectorList&&, GC::Ref<CSSPageDescriptors>, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
    virtual String serialized() const override;
    virtual void visit_edges(Visitor&) override;

    PageSelectorList m_selectors;
    GC::Ref<CSSPageDescriptors> m_style;
};

}

namespace AK {

template<>
struct Formatter<Web::CSS::PageSelector> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::PageSelector const& selector)
    {
        return Formatter<StringView>::format(builder, selector.serialize());
    }
};

}
