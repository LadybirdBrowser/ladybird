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

// The CoreText font of a face in font data. The upper 16 bits of the index select a named instance, from 1, whose
// coordinates the font gets as a variation. CoreText keeps the backing while it uses the data. Null if CoreText does
// not load the face.
CTFontRef create_core_text_font_from_data(NonnullRefPtr<Typeface::FontDataBacking>, ReadonlyBytes, u32 ttc_index);

// Whether CoreText loads a face of font data the way the compositor does to draw it.
bool core_text_accepts_font_data(NonnullRefPtr<Typeface::FontDataBacking>, u32 ttc_index);

// A copy of a CoreText font at a size, with these coordinates on top of the variation of the font. Without
// coordinates, the copy keeps the variation of the font. Null if CoreText does not make the copy.
CTFontRef create_core_text_font_with_variations(CTFontRef, CGFloat size, ReadonlySpan<FontVariationAxis>);

// The first named instance of a face, counted from 0, whose coordinates are the variation of a CoreText font.
// CoreText leaves the axes at their default values out of the variation.
Optional<unsigned> named_instance_with_variation(hb_face_t*, CFDictionaryRef variation);

}
