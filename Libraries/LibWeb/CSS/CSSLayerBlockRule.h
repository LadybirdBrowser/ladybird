/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSGroupingRule.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-cascade-5/#the-csslayerblockrule-interface
class CSSLayerBlockRule final : public CSSGroupingRule {
    WEB_PLATFORM_OBJECT(CSSLayerBlockRule, CSSGroupingRule);
    GC_DECLARE_ALLOCATOR(CSSLayerBlockRule);

public:
    [[nodiscard]] static GC::Ref<CSSLayerBlockRule> create(JS::Realm&, Utf16FlyString name, CSSRuleList&);

    static Utf16FlyString next_unique_anonymous_layer_name();

    virtual ~CSSLayerBlockRule() = default;

    Utf16FlyString const& name() const { return m_name; }
    Utf16FlyString const& internal_name() const { return m_name_internal; }
    Utf16FlyString internal_qualified_name(Badge<StyleScope>) const;

private:
    CSSLayerBlockRule(JS::Realm&, Utf16FlyString name, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    Utf16FlyString m_name;
    Utf16FlyString m_name_internal;
};

}
