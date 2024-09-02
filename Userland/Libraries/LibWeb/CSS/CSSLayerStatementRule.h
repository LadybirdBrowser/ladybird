/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-cascade-5/#the-csslayerstatementrule-interface
class CSSLayerStatementRule final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSLayerStatementRule, CSSRule);
    JS_DECLARE_ALLOCATOR(CSSLayerStatementRule);

public:
    [[nodiscard]] static JS::NonnullGCPtr<CSSLayerStatementRule> create(JS::Realm&, Vector<FlyString> name_list);

    virtual ~CSSLayerStatementRule() = default;

    virtual Type type() const override { return Type::LayerStatement; }

    // FIXME: Should be FrozenArray
    ReadonlySpan<FlyString> name_list() const { return m_name_list; }
    Vector<FlyString> internal_qualified_name_list(Badge<StyleComputer>) const;

private:
    CSSLayerStatementRule(JS::Realm&, Vector<FlyString> name_list);

    virtual void initialize(JS::Realm&) override;
    virtual String serialized() const override;

    Vector<FlyString> m_name_list;
};

}
