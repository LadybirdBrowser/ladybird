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
    WEB_WRAPPABLE(CSSLayerBlockRule, CSSGroupingRule);
    GC_DECLARE_ALLOCATOR(CSSLayerBlockRule);

public:
    [[nodiscard]] static GC::Ref<CSSLayerBlockRule> create(RustRule, CSSRuleList&);

    virtual ~CSSLayerBlockRule() = default;

    Utf16View name() const;
    Utf16FlyString const& internal_name() const;

private:
    CSSLayerBlockRule(RustRule, CSSRuleList&);

    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;
    virtual size_t external_memory_size() const override;

    Parser::ValueParserFFI::LayerNames const& m_names;
    mutable Optional<Utf16FlyString> m_name_internal;
};

}
