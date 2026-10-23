/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-anchor-position-1/#om-position-try
class CSSPositionTryRule final : public CSSRule {
    WEB_WRAPPABLE(CSSPositionTryRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSPositionTryRule);

public:
    [[nodiscard]] static GC::Ref<CSSPositionTryRule> create(RustRule);

    virtual ~CSSPositionTryRule() override = default;

    Utf16String name() const;
    GC::Ref<CSSPositionTryDescriptors> style() const;

private:
    explicit CSSPositionTryRule(RustRule);

    virtual size_t external_memory_size() const override;
    virtual Utf16String serialized() const override;
    virtual void visit_edges(Visitor&) override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    Utf16FlyString m_name;
    RustDeclarationBlock m_declarations;
    mutable GC::Ptr<CSSPositionTryDescriptors> m_style;
};

}
