/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSFontFaceDescriptors.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/ParsedFontFace.h>

namespace Web::CSS {

class CSSFontFaceRule final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSFontFaceRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSFontFaceRule);

public:
    [[nodiscard]] static GC::Ref<CSSFontFaceRule> create(JS::Realm&, GC::Ref<CSSFontFaceDescriptors>);

    virtual ~CSSFontFaceRule() override = default;

    bool is_valid() const;
    ParsedFontFace font_face() const;
    GC::Ref<CSSStyleDeclaration> style() { return m_style; }
    GC::Ref<CSSFontFaceDescriptors const> descriptors() const { return m_style; }

private:
    CSSFontFaceRule(JS::Realm&, GC::Ref<CSSFontFaceDescriptors>);

    virtual void initialize(JS::Realm&) override;
    virtual String serialized() const override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<CSSFontFaceDescriptors> m_style;
};

template<>
inline bool CSSRule::fast_is<CSSFontFaceRule>() const { return type() == CSSRule::Type::FontFace; }

}
