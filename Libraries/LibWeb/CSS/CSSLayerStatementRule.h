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
    GC_DECLARE_ALLOCATOR(CSSLayerStatementRule);

public:
    [[nodiscard]] static GC::Ref<CSSLayerStatementRule> create(JS::Realm&, Vector<Utf16FlyString> name_list);

    virtual ~CSSLayerStatementRule() = default;

    // FIXME: Should be FrozenArray
    ReadonlySpan<Utf16FlyString> name_list() const { return m_name_list; }
    Vector<Utf16FlyString> internal_qualified_name_list(Badge<StyleScope>) const;

private:
    CSSLayerStatementRule(JS::Realm&, Vector<Utf16FlyString> name_list);

    virtual void initialize(JS::Realm&) override;
    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    Vector<Utf16FlyString> m_name_list;
};

}
