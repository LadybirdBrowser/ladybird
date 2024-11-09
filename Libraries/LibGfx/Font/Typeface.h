/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/RefCounted.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontData.h>
#include <LibGfx/Forward.h>

#define POINTS_PER_INCH 72.0f
#define DEFAULT_DPI 96

class SkTypeface;
struct hb_blob_t;
struct hb_face_t;

namespace Gfx {

class ScaledFont;

struct ScaledFontMetrics {
    float ascender { 0 };
    float descender { 0 };
    float line_gap { 0 };
    float x_height { 0 };

    float height() const
    {
        return ascender + descender;
    }
};

class Typeface : public RefCounted<Typeface> {
public:
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_resource(Core::Resource const&, int ttc_index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_font_data(NonnullOwnPtr<Gfx::FontData>, int ttc_index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_externally_owned_memory(ReadonlyBytes bytes, int ttc_index = 0);

    virtual ~Typeface();

    virtual u32 glyph_count() const = 0;
    virtual u16 units_per_em() const = 0;
    virtual u32 glyph_id_for_code_point(u32 code_point) const = 0;
    virtual FlyString family() const = 0;
    virtual u16 weight() const = 0;
    virtual u16 width() const = 0;
    virtual u8 slope() const = 0;

    [[nodiscard]] NonnullRefPtr<ScaledFont> scaled_font(float point_size) const;

    hb_face_t* harfbuzz_typeface() const;

protected:
    Typeface();

    virtual ReadonlyBytes buffer() const = 0;
    virtual unsigned ttc_index() const = 0;

private:
    OwnPtr<Gfx::FontData> m_font_data;

    mutable HashMap<float, NonnullRefPtr<ScaledFont>> m_scaled_fonts;
    mutable hb_blob_t* m_harfbuzz_blob { nullptr };
    mutable hb_face_t* m_harfbuzz_face { nullptr };
};

}
