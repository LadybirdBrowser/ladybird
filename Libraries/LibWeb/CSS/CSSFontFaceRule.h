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

class FontFace;

class CSSFontFaceRule final : public CSSRule {
    WEB_WRAPPABLE(CSSFontFaceRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSFontFaceRule);

public:
    [[nodiscard]] static GC::Ref<CSSFontFaceRule> create(RustDescriptorBlock);

    virtual ~CSSFontFaceRule() override = default;

    bool is_valid() const;
    ParsedFontFace font_face() const;
    GC::Ref<CSSFontFaceDescriptors> descriptors() const;
    RustDescriptorBlock const& descriptor_block() const { return m_descriptors; }

    GC::Ptr<FontFace> css_connected_font_face() const { return m_css_connected_font_face; }
    void set_css_connected_font_face(GC::Ptr<FontFace> font_face) { m_css_connected_font_face = font_face; }
    void handle_descriptor_change(Utf16FlyString const& property);
    void disconnect_font_face();

private:
    CSSFontFaceRule(RustDescriptorBlock);

    virtual size_t external_memory_size() const override;
    virtual Utf16String serialized() const override;
    virtual void visit_edges(Visitor&) override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    void handle_src_descriptor_change();

    RustDescriptorBlock m_descriptors;
    mutable GC::Ptr<CSSFontFaceDescriptors> m_style;
    GC::Ptr<FontFace> m_css_connected_font_face;
};

template<>
inline bool CSSRule::fast_is<CSSFontFaceRule>() const { return type() == CSSRule::Type::FontFace; }

}
