/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/Once.h>
#include <AK/Optional.h>
#include <AK/QuickSort.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/Font/FaceDescription.h>
#include <LibGfx/Font/FontVariationSettings.h>
#include <LibGfx/Font/RasterizerData.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ShapeFeature.h>
#include <LibIPC/Forward.h>

#define POINTS_PER_INCH 72.0f
#define DEFAULT_DPI 96

struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;

namespace Gfx {

class Font;

struct SystemFontIdentifier {
    u64 generation { 0 };
    u64 face_id { 0 };

    bool operator==(SystemFontIdentifier const&) const = default;
};

// The designs of the system UI font that CSS can name.
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

struct FontCacheKey {
    float point_size;
    Vector<FontVariationAxis> axes;
    Gfx::ShapeFeatures shape_features;

    // Sizes and values compare by their bits, as the hash reads them, so that a key with NaN finds itself.
    bool operator==(FontCacheKey const& other) const
    {
        if (bit_cast<u32>(point_size) != bit_cast<u32>(other.point_size) || axes.size() != other.axes.size() || shape_features != other.shape_features)
            return false;
        for (size_t index = 0; index < axes.size(); ++index) {
            if (axes[index].tag != other.axes[index].tag || bit_cast<u32>(axes[index].value) != bit_cast<u32>(other.axes[index].value))
                return false;
        }
        return true;
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

class Typeface : public AtomicRefCounted<Typeface> {
public:
    struct FontDataBacking final : AtomicRefCounted<FontDataBacking> {
        using Storage = Variant<Core::AnonymousBuffer, NonnullOwnPtr<Core::MappedFile>>;

        explicit FontDataBacking(Storage storage)
            : storage(move(storage))
        {
        }

        ReadonlyBytes bytes() const;

        Storage storage;
    };

    // The face of font data that FreeType loads as an SFNT font and that HarfBuzz reads, so that both shaping and every
    // rasterizer have the face. The upper 16 bits of the index select a named instance of a variable face, from 1.
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_font_data(NonnullRefPtr<FontDataBacking>, u32 ttc_index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_mapped_file(NonnullOwnPtr<Core::MappedFile>, u32 ttc_index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_anonymous_buffer(Core::AnonymousBuffer, u32 ttc_index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_temporary_memory(ReadonlyBytes bytes, u32 ttc_index = 0);

    virtual ~Typeface();

    u32 glyph_count() const;
    u16 units_per_em() const;
    u32 glyph_id_for_code_point(u32 code_point) const;
    FlyString const& family() const;
    u16 weight() const;
    u16 width() const;
    u8 slope() const;

    ReadonlyBytes font_data() const LIFETIME_BOUND { return m_font_data ? m_font_data->bytes() : ReadonlyBytes {}; }
    // What keeps the font data alive. Null for a typeface without font data of its own.
    RefPtr<FontDataBacking> font_data_backing() const { return m_font_data; }
    u32 collection_index() const { return m_ttc_index; }

    [[nodiscard]] NonnullRefPtr<Font> font(float point_size, FontVariationSettings const& variations = {}, Gfx::ShapeFeatures const& shape_features = {}) const;
    // A font that measures and shapes text like font() but draws nothing. Each call makes a new font.
    [[nodiscard]] NonnullRefPtr<Font> invisible_font(float point_size, FontVariationSettings const& variations = {}, Gfx::ShapeFeatures const& shape_features = {}) const;

    void set_system_font_identifier(SystemFontIdentifier identifier) { m_system_font_identifier = identifier; }
    Optional<SystemFontIdentifier> system_font_identifier() const { return m_system_font_identifier; }

    void set_file_path(String file_path) { m_file_path = move(file_path); }

    hb_face_t* harfbuzz_typeface() const;
    FaceDescription const& description() const;

    // The style of a font of this typeface with these variations.
    FaceStyle style_for_variations(ReadonlySpan<FontVariationAxis>) const;
    ErrorOr<Vector<String>> local_font_names() const;
    Optional<String> postscript_name() const;

    // What a rasterizer keeps with this typeface. The first call makes it.
    template<typename T, typename Callback>
    T& rasterizer_data(Callback make) const { return m_rasterizer_data.get<T>(make); }

    // Union of all glyph bounding boxes as recorded in the `head` table, in font units with y pointing up.
    // is_empty() when the face has no usable `head` table (e.g. bitmap-only fonts).
    struct BoundingBoxInFontUnits {
        i16 x_min { 0 };
        i16 y_min { 0 };
        i16 x_max { 0 };
        i16 y_max { 0 };
        u16 units_per_em { 0 };

        bool is_empty() const { return x_min >= x_max || y_min >= y_max || units_per_em == 0; }
    };
    BoundingBoxInFontUnits bounding_box_in_font_units() const;

    template<typename T>
    bool fast_is() const = delete;

    virtual bool is_core_text() const { return false; }

    // How many glyph pages the calling thread has filled in, for tests of its glyph page caches.
    static u64 glyph_pages_populated_on_this_thread();

protected:
    enum class FontDataFormat : u8 {
        RawFontData,
        MappedFile,
        SystemUIFont,
        SystemFontId,
        PlatformFontName,
    };

    // A typeface without font data of its own.
    Typeface();

    virtual void encode_font_data_for_ipc(IPC::Encoder&) const;
    virtual hb_face_t* create_harfbuzz_face() const;
    // A typeface that the platform picked for a style answers with that style, whatever its tables say.
    virtual Optional<FaceStyle> fixed_style() const { return {}; }

private:
    Typeface(NonnullRefPtr<FontDataBacking>, u32 ttc_index);

    friend class Font;

    template<typename T>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, T const&);

    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);

    RefPtr<FontDataBacking> m_font_data;
    u32 m_ttc_index { 0 };
    Optional<SystemFontIdentifier> m_system_font_identifier;
    Optional<String> m_file_path;

    // A font that is being destroyed leaves the cache of its typeface.
    void forget_font(Font const&) const;

    // This cache stores information per code point.
    // It's segmented into pages with data about 256 code points each.
    struct GlyphPage {
        AK_ALLOC_WITH_KMALLOC;

        static constexpr size_t glyphs_per_page = 256;
        u16 glyph_ids[glyphs_per_page];
    };

    [[nodiscard]] GlyphPage const& glyph_page(size_t page_index) const;
    void populate_glyph_page(GlyphPage&, size_t page_index) const;
    hb_font_t* cmap_font() const;

    // Addresses can be reused after destruction, so per-thread caches use a monotonic identity.
    u64 m_glyph_cache_id { 0 };

    mutable Mutex m_fonts_mutex;
    // The fonts that are alive. A font does not keep itself here, so it holds its typeface without a reference cycle.
    mutable HashMap<FontCacheKey, Font*> m_fonts;
    mutable OnceFlag m_harfbuzz_face_once;
    mutable hb_blob_t* m_harfbuzz_blob { nullptr };
    mutable hb_face_t* m_harfbuzz_face { nullptr };
    mutable OnceFlag m_cmap_font_once;
    mutable hb_font_t* m_cmap_font { nullptr };
    mutable OnceFlag m_description_once;
    mutable Optional<FaceDescription> m_description;
    mutable OnceFlag m_bounding_box_once;
    mutable BoundingBoxInFontUnits m_bounding_box_in_font_units;
    RasterizerDataSlot m_rasterizer_data;
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

template<>
ErrorOr<void> encode(Encoder&, Gfx::SystemUIFontStyle const&);

template<>
ErrorOr<Gfx::SystemUIFontStyle> decode(Decoder&);

}
