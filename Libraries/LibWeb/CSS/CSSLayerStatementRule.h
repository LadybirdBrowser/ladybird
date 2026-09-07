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
    WEB_WRAPPABLE(CSSLayerStatementRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSLayerStatementRule);

public:
    [[nodiscard]] static GC::Ref<CSSLayerStatementRule> create(RustRule);

    virtual ~CSSLayerStatementRule() = default;

    // FIXME: Should be FrozenArray
    Vector<Utf16String> name_list() const;

private:
    CSSLayerStatementRule(RustRule);

    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;
    virtual size_t external_memory_size() const override;

    Utf16View name_at(size_t) const;
    Parser::ValueParserFFI::LayerNames const& m_names;
};

}
