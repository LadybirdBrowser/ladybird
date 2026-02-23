/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSFontFaceDescriptors.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/ParsedFontFace.h>

namespace Web::CSS {

class FontFace;

class CSSFontFaceRule final
    : public CSSRule
    , public CSSStyleSheet::Subresource {
    WEB_PLATFORM_OBJECT(CSSFontFaceRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSFontFaceRule);

public:
    [[nodiscard]] static GC::Ref<CSSFontFaceRule> create(JS::Realm&, GC::Ref<CSSFontFaceDescriptors>);

    virtual ~CSSFontFaceRule() override = default;

    bool is_valid() const;
    ParsedFontFace font_face() const;
    GC::Ref<CSSStyleDeclaration> style() { return m_style; }
    GC::Ref<CSSFontFaceDescriptors> descriptors() { return m_style; }
    GC::Ref<CSSFontFaceDescriptors const> descriptors() const { return m_style; }

    GC::Ptr<FontFace> css_connected_font_face() const { return m_css_connected_font_face; }
    void set_css_connected_font_face(GC::Ptr<FontFace> font_face) { m_css_connected_font_face = font_face; }
    void handle_descriptor_change(FlyString const& property);
    void disconnect_font_face();

private:
    CSSFontFaceRule(JS::Realm&, GC::Ref<CSSFontFaceDescriptors>);

    virtual void initialize(JS::Realm&) override;
    virtual String serialized() const override;
    virtual void visit_edges(Visitor&) override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    void handle_src_descriptor_change();

    virtual void set_parent_style_sheet(CSSStyleSheet*) override;

    GC::Ptr<CSSStyleSheet> parent_style_sheet_for_subresource() override { return parent_style_sheet(); }

    GC::Ref<CSSFontFaceDescriptors> m_style;
    GC::Ptr<FontFace> m_css_connected_font_face;
};

template<>
inline bool CSSRule::fast_is<CSSFontFaceRule>() const { return type() == CSSRule::Type::FontFace; }

}
