/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/QuickSort.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Variant.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Resource.h>
#include <LibGfx/Font/FontVariationSettings.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ShapeFeature.h>
#include <LibIPC/Forward.h>

#define POINTS_PER_INCH 72.0f
#define DEFAULT_DPI 96

class SkTypeface;
struct hb_blob_t;
struct hb_face_t;

namespace Gfx {

class Font;

struct FontCacheKey {
    float point_size;
    Vector<FontVariationAxis> axes;
    Gfx::ShapeFeatures shape_features;

    bool operator==(FontCacheKey const& other) const
    {
        return point_size == other.point_size && axes == other.axes && shape_features == other.shape_features;
    }

    unsigned hash() const
    {
        auto h = pair_int_hash(bit_cast<u32>(point_size), axes.size());
        for (auto const& axis : axes)
            h = pair_int_hash(h, pair_int_hash(axis.tag.to_u32(), bit_cast<u32>(axis.value)));
        h = pair_int_hash(h, Traits<Gfx::ShapeFeatures>::hash(shape_features));
        return h;
    }
};

class Typeface : public RefCounted<Typeface> {
public:
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_resource(Core::Resource const&, u32 ttc_index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_anonymous_buffer(Core::AnonymousBuffer, u32 ttc_index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_temporary_memory(ReadonlyBytes bytes, u32 ttc_index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_externally_owned_memory(ReadonlyBytes bytes, u32 ttc_index = 0);

    virtual ~Typeface();

    virtual u32 glyph_count() const = 0;
    virtual u16 units_per_em() const = 0;
    virtual u32 glyph_id_for_code_point(u32 code_point) const = 0;
    virtual FlyString const& family() const = 0;
    virtual u16 weight() const = 0;
    virtual u16 width() const = 0;
    virtual u8 slope() const = 0;

    [[nodiscard]] NonnullRefPtr<Font> font(float point_size, FontVariationSettings const& variations = {}, Gfx::ShapeFeatures const& shape_features = {}) const;

    hb_face_t* harfbuzz_typeface() const;

    template<typename T>
    bool fast_is() const = delete;

    virtual bool is_skia() const { return false; }

protected:
    enum class FontDataFormat : u8 {
        RawFontData,
        ResourceFontData,
        SystemFont,
    };

    Typeface();

    virtual ReadonlyBytes buffer() const = 0;
    virtual u32 ttc_index() const = 0;
    virtual void encode_font_data_for_ipc(IPC::Encoder&) const;
    virtual hb_face_t* create_harfbuzz_face() const;

    void set_anonymous_font_data(Core::AnonymousBuffer);
    void set_resource_font_data(Core::Resource const&);
    void copy_font_data_from(Typeface const&);
    bool has_font_data_backing() const { return m_font_data.has_value(); }

private:
    template<typename T>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, T const&);

    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);

    using FontDataBacking = Variant<Core::AnonymousBuffer, NonnullRefPtr<Core::Resource const>>;
    Optional<FontDataBacking> m_font_data;

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

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::Typeface const&);

template<>
ErrorOr<NonnullRefPtr<Gfx::Typeface const>> decode(Decoder&);

}
