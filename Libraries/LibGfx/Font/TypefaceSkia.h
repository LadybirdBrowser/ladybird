/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Font/Typeface.h>

#ifdef AK_OS_MACOS
#    include <CoreText/CoreText.h>
#endif

template<typename T>
class sk_sp;

namespace Gfx {

enum class SystemUIFontKind : u8 {
    System,
    Serif,
    Monospace,
    Rounded,
};

struct SystemUIFontStyle {
    SystemUIFontKind kind;
    u16 weight;
    u16 width;
    u8 slope;
};

class TypefaceSkia : public Gfx::Typeface {
    AK_MAKE_NONCOPYABLE(TypefaceSkia);

public:
    static ErrorOr<NonnullRefPtr<TypefaceSkia>> load_from_buffer(ReadonlyBytes, u32 ttc_index, NonnullRefPtr<FontDataBacking>);
    static ErrorOr<RefPtr<TypefaceSkia>> match_system_ui(SystemUIFontKind, float point_size, u16 weight, u16 width, u8 slope);
    static ErrorOr<RefPtr<TypefaceSkia>> match_family_style(StringView family_name, u16 weight, u16 width, u8 slope);
    static ErrorOr<RefPtr<TypefaceSkia>> find_typeface_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji);
    static Optional<FlyString> resolve_generic_family(StringView family_name, u16 weight, u8 slope);

    virtual FlyString const& family() const override;
    virtual u16 weight() const override;
    virtual u16 width() const override;
    virtual u8 slope() const override;
    virtual FaceStyle style_for_variations(ReadonlySpan<FontVariationAxis>) const override;

    virtual ReadonlyBytes buffer() const LIFETIME_BOUND override { return m_buffer; }
    virtual u32 ttc_index() const override { return m_ttc_index; }

    SkTypeface const* sk_typeface() const;
    u32 platform_typeface_id() const;

protected:
    virtual void encode_font_data_for_ipc(IPC::Encoder&) const override;
    virtual hb_face_t* create_harfbuzz_face() const override;

private:
    struct Impl;
    Impl& impl() const { return *m_impl; }
    NonnullOwnPtr<Impl> m_impl;

    static ErrorOr<RefPtr<TypefaceSkia>> typeface_from_skia_typeface(sk_sp<SkTypeface>, Optional<SystemUIFontStyle> = {});
#ifdef AK_OS_MACOS
    static ErrorOr<RefPtr<TypefaceSkia>> typeface_from_core_text_typeface(sk_sp<SkTypeface>, CTFontRef, SystemUIFontStyle);
#endif

    TypefaceSkia(NonnullOwnPtr<Impl>, ReadonlyBytes, u32 ttc_index = 0);

    virtual bool is_skia() const override { return true; }

    ReadonlyBytes m_buffer;
    u32 m_ttc_index { 0 };
};

template<>
inline bool Typeface::fast_is<TypefaceSkia>() const { return is_skia(); }

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::SystemUIFontStyle const&);

template<>
ErrorOr<Gfx::SystemUIFontStyle> decode(Decoder&);

}
