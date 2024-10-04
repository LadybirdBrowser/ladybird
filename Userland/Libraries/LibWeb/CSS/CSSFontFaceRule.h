/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/ParsedFontFace.h>

namespace Web::CSS {

class CSSFontFaceRule final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSFontFaceRule, CSSRule);
    JS_DECLARE_ALLOCATOR(CSSFontFaceRule);

public:
    [[nodiscard]] static JS::NonnullGCPtr<CSSFontFaceRule> create(JS::Realm&, ParsedFontFace&&);

    virtual ~CSSFontFaceRule() override = default;

    virtual Type type() const override { return Type::FontFace; }

    ParsedFontFace const& font_face() const { return m_font_face; }
    CSSStyleDeclaration* style();

private:
    CSSFontFaceRule(JS::Realm&, ParsedFontFace&&);

    virtual void initialize(JS::Realm&) override;
    virtual String serialized() const override;

    ParsedFontFace m_font_face;
};

template<>
inline bool CSSRule::fast_is<CSSFontFaceRule>() const { return type() == CSSRule::Type::FontFace; }

}
