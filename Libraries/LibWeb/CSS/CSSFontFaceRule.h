/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSFontFaceDescriptors.h>
#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

class FontFaceState;

class CSSFontFaceRule final : public CSSRule {
    WEB_WRAPPABLE(CSSFontFaceRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSFontFaceRule);

public:
    [[nodiscard]] static GC::Ref<CSSFontFaceRule> create(RustRule);

    virtual ~CSSFontFaceRule() override = default;

    bool is_valid() const;
    GC::Ref<CSSFontFaceDescriptors> descriptors() const;

    RefPtr<FontFaceState> css_connected_font_face() const;
    void handle_descriptor_change(Utf16FlyString const& property);
    void disconnect_font_face();

private:
    CSSFontFaceRule(RustRule);

    virtual size_t external_memory_size() const override;
    virtual Utf16String serialized() const override;
    virtual void visit_edges(Visitor&) override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    void handle_src_descriptor_change();

    RustDescriptorBlock m_descriptors;
    mutable GC::Ptr<CSSFontFaceDescriptors> m_style;
};

template<>
inline bool CSSRule::fast_is<CSSFontFaceRule>() const { return type() == CSSRule::Type::FontFace; }

}
