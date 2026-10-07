/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Font/Typeface.h>

#include <CoreText/CoreText.h>

namespace Gfx {

// A system UI font, which CoreText gives without its font data. HarfBuzz reads its tables through CoreText.
class TypefaceCoreText final : public Typeface {
    AK_MAKE_NONCOPYABLE(TypefaceCoreText);
    AK_MAKE_NONMOVABLE(TypefaceCoreText);

public:
    // The system UI font of a design, which answers with the style that it was asked for. A style always gives the
    // same typeface.
    static RefPtr<TypefaceCoreText> system_ui(SystemUIFontStyle);

    virtual ~TypefaceCoreText() override;

    CTFontRef core_text_font() const { return m_core_text_font; }

private:
    TypefaceCoreText(CTFontRef, CGFontRef, SystemUIFontStyle);

    virtual ReadonlyBytes buffer() const override { return {}; }
    virtual u32 ttc_index() const override { return 0; }
    virtual hb_face_t* create_harfbuzz_face() const override;
    virtual void encode_font_data_for_ipc(IPC::Encoder&) const override;
    virtual Optional<FaceStyle> fixed_style() const override;
    virtual bool is_core_text() const override { return true; }

    CTFontRef m_core_text_font { nullptr };
    CGFontRef m_graphics_font { nullptr };
    SystemUIFontStyle m_style;
};

template<>
inline bool Typeface::fast_is<TypefaceCoreText>() const { return is_core_text(); }

}
