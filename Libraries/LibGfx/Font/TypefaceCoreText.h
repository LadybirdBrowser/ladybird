/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <LibGfx/Font/Typeface.h>

#include <CoreText/CoreText.h>

namespace Gfx {

// A typeface that CoreText gives without its font data: a system UI font, or an installed font whose data CoreText
// cannot load back. HarfBuzz reads its tables through CoreText.
class TypefaceCoreText final : public Typeface {
    AK_MAKE_NONCOPYABLE(TypefaceCoreText);
    AK_MAKE_NONMOVABLE(TypefaceCoreText);

public:
    // The system UI font of a design, which answers with the style that it was asked for. A style always gives the
    // same typeface.
    static RefPtr<TypefaceCoreText> system_ui(SystemUIFontStyle);

    // The installed font that has this PostScript name.
    static ErrorOr<NonnullRefPtr<TypefaceCoreText>> try_load_postscript_name(String const&);

    virtual ~TypefaceCoreText() override;

    CTFontRef core_text_font() const { return m_core_text_font; }

    // HarfBuzz cannot draw the outlines of some fonts that CoreText gives, such as the hvgl outlines of PingFang.
    bool has_outlines_that_only_core_text_draws() const;

private:
    using Identity = Variant<SystemUIFontStyle, String>;

    TypefaceCoreText(CTFontRef, CGFontRef, Identity);

    virtual ReadonlyBytes buffer() const override { return {}; }
    virtual u32 ttc_index() const override { return 0; }
    virtual hb_face_t* create_harfbuzz_face() const override;
    virtual void encode_font_data_for_ipc(IPC::Encoder&) const override;
    virtual Optional<FaceStyle> fixed_style() const override;
    virtual bool is_core_text() const override { return true; }

    CTFontRef m_core_text_font { nullptr };
    CGFontRef m_graphics_font { nullptr };
    Identity m_identity;
    mutable OnceFlag m_outline_format_once;
    mutable bool m_has_outlines_that_only_core_text_draws { false };
};

template<>
inline bool Typeface::fast_is<TypefaceCoreText>() const { return is_core_text(); }

// A copy of a CoreText font at a size, with these coordinates on top of the variation of the font. Without
// coordinates, the copy keeps the variation of the font. Null if CoreText does not make the copy.
CTFontRef create_core_text_font_with_variations(CTFontRef, CGFloat size, ReadonlySpan<FontVariationAxis>);

}
