/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSDescriptors.h>
#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

class FontFaceState;

class CSSFontFaceRule final : public CSSRule {
    WEB_RUST_INTERFACE(CSSFontFaceRule, css_font_face_rule)

public:
    bool is_valid() const;
    GC::Ref<CSSDescriptors> descriptors() const;

    RefPtr<FontFaceState> css_connected_font_face() const;
    void handle_descriptor_change(Utf16FlyString const& property);
    void disconnect_font_face();

private:
    void handle_src_descriptor_change();
};

template<>
inline bool CSSRule::fast_is<CSSFontFaceRule>() const { return type() == CSSRule::Type::FontFace; }

}
