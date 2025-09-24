/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <AK/QuickSort.h>
#include <LibGfx/Font/FontData.h>
#include <LibGfx/Forward.h>

#define POINTS_PER_INCH 72.0f
#define DEFAULT_DPI 96

class SkTypeface;
struct hb_blob_t;
struct hb_face_t;

namespace Gfx {

using FourByteTag = uint32_t;

// Pack four chars into a FourByteTag (big-endian like Skia)
constexpr FourByteTag MakeFourByteTag(char a, char b, char c, char d)
{
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
}

class Font;

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

struct FontCacheKey {
    float point_size;
    Vector<std::pair<FourByteTag, float>> axes;

    FontCacheKey(float point_size, Vector<std::pair<FourByteTag, float>> const& axes)
        : point_size(point_size)
        , axes(axes)
    {
        normalize();
    }

    void normalize()
    {
        quick_sort(axes, [](auto const& a, auto const& b) {
            return a.first < b.first;
        });
    }

    bool operator==(FontCacheKey const& other) const
    {
        if (point_size != other.point_size)
            return false;
        for (size_t i = 0; i < axes.size(); ++i)
            if (axes[i] != other.axes[i])
                return false;
        return true;
    }

    unsigned hash() const
    {
        auto h = pair_int_hash(int_hash(bit_cast<u32>(point_size)), axes.size());
        for (auto const& axis : axes)
            h = pair_int_hash(h, pair_int_hash(axis.first, int_hash(bit_cast<u32>(axis.second))));
        return h;
    }
};

class Typeface : public RefCounted<Typeface> {
public:
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_resource(Core::Resource const&, int ttc_index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_font_data(NonnullOwnPtr<Gfx::FontData>, int ttc_index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_temporary_memory(ReadonlyBytes bytes, int ttc_index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_externally_owned_memory(ReadonlyBytes bytes, int ttc_index = 0);

    virtual ~Typeface();

    virtual u32 glyph_count() const = 0;
    virtual u16 units_per_em() const = 0;
    virtual u32 glyph_id_for_code_point(u32 code_point) const = 0;
    virtual FlyString const& family() const = 0;
    virtual u16 weight() const = 0;
    virtual u16 width() const = 0;
    virtual u8 slope() const = 0;

    [[nodiscard]] NonnullRefPtr<Font> font(float point_size, Vector<std::pair<FourByteTag, float>> const& axes = {}) const;

    hb_face_t* harfbuzz_typeface() const;

    template<typename T>
    bool fast_is() const = delete;

    virtual bool is_skia() const { return false; }

protected:
    Typeface();

    virtual ReadonlyBytes buffer() const = 0;
    virtual unsigned ttc_index() const = 0;

private:
    OwnPtr<FontData> m_font_data;

    mutable HashMap<FontCacheKey, NonnullRefPtr<Font>> m_fonts;
    mutable hb_blob_t* m_harfbuzz_blob { nullptr };
    mutable hb_face_t* m_harfbuzz_face { nullptr };
};

}

template<>
struct AK::Traits<Gfx::FontCacheKey> : public AK::DefaultTraits<Gfx::FontCacheKey> {
    static unsigned hash(Gfx::FontCacheKey const& key)
    {
        return key.hash();
    }
};
