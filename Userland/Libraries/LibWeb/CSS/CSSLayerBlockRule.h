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
    JS_DECLARE_ALLOCATOR(CSSLayerBlockRule);

public:
    [[nodiscard]] static JS::NonnullGCPtr<CSSLayerBlockRule> create(JS::Realm&, FlyString name, CSSRuleList&);

    static FlyString next_unique_anonymous_layer_name();

    virtual ~CSSLayerBlockRule() = default;

    virtual Type type() const override { return Type::LayerBlock; }

    FlyString const& name() const { return m_name; }
    FlyString const& internal_name() const { return m_name_internal; }
    FlyString internal_qualified_name(Badge<StyleComputer>) const;

private:
    CSSLayerBlockRule(JS::Realm&, FlyString name, CSSRuleList&);

    virtual void initialize(JS::Realm&) override;
    virtual String serialized() const override;

    FlyString m_name;
    FlyString m_name_internal;
};

}
