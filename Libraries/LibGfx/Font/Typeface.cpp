/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <harfbuzz/hb-ot.h>
#include <harfbuzz/hb.h>

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/Diagnostics.h>
#include <AK/NeverDestroyed.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/FontTable.h>
#include <LibGfx/Font/FontVariationSettings.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/TypefaceSkia.h>
#ifdef AK_OS_MACOS
#    include <LibGfx/Font/TypefaceCoreText.h>
#endif
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Gfx {

ErrorOr<Vector<String>> Typeface::local_font_names() const
{
    // https://drafts.csswg.org/css-fonts-4/#local-font-fallback
    // NB: local() identifies a face by its full name or PostScript name, never by its family.
    Vector<String> names;
    for (auto name_id : { HB_OT_NAME_ID_FULL_NAME, HB_OT_NAME_ID_POSTSCRIPT_NAME }) {
        if (auto name = face_name(harfbuzz_typeface(), name_id); name.has_value())
            TRY(names.try_append(name.release_value()));
    }
    return names;
}

FlyString const& Typeface::family() const
{
    return description().family();
}

u16 Typeface::weight() const
{
    return fixed_style().value_or(description().style()).weight;
}

u16 Typeface::width() const
{
    return fixed_style().value_or(description().style()).width;
}

u8 Typeface::slope() const
{
    return fixed_style().value_or(description().style()).slope;
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_mapped_file(NonnullOwnPtr<Core::MappedFile> mapped_file, u32 ttc_index)
{
    auto bytes = mapped_file->bytes();
    return TypefaceSkia::load_from_buffer(bytes, ttc_index, make_ref_counted<FontDataBacking>(move(mapped_file)));
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_anonymous_buffer(Core::AnonymousBuffer anonymous_buffer, u32 ttc_index)
{
    auto bytes = anonymous_buffer.bytes();
    return TypefaceSkia::load_from_buffer(bytes, ttc_index, make_ref_counted<FontDataBacking>(move(anonymous_buffer)));
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_temporary_memory(ReadonlyBytes bytes, u32 ttc_index)
{
    auto anonymous_buffer = TRY(Core::AnonymousBuffer::create_with_size(bytes.size()));
    if (!bytes.is_empty())
        memcpy(anonymous_buffer.data<void>(), bytes.data(), bytes.size());
    return try_load_from_anonymous_buffer(move(anonymous_buffer), ttc_index);
}

static Atomic<u64> s_next_glyph_cache_id { 1 };

Typeface::Typeface()
    : m_glyph_cache_id(s_next_glyph_cache_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
{
    VERIFY(m_glyph_cache_id != 0);
}

Typeface::~Typeface()
{
    if (m_cmap_font)
        hb_font_destroy(m_cmap_font);
    if (m_harfbuzz_face)
        hb_face_destroy(m_harfbuzz_face);
    if (m_harfbuzz_blob)
        hb_blob_destroy(m_harfbuzz_blob);
}

namespace {

// The fonts that were made last. Text asks for the same fonts again and again, so a font stays alive for a while after
// its last holder drops it, and keeps its id and the data of its rasterizer when it comes back.
struct RecentFonts {
    static constexpr size_t capacity = 256;

    Mutex mutex;
    Array<RefPtr<Font const>, capacity> fonts;
    size_t next_index { 0 };
};

void keep_recent_font(NonnullRefPtr<Font const> font)
{
    static NeverDestroyed<RecentFonts> recent_fonts;
    RefPtr<Font const> oldest_font;
    {
        MutexLocker locker { recent_fonts->mutex };
        oldest_font = exchange(recent_fonts->fonts[recent_fonts->next_index], move(font));
        recent_fonts->next_index = (recent_fonts->next_index + 1) % RecentFonts::capacity;
    }
    // NB: The oldest font is released after the lock, because releasing it can destroy the font and its typeface.
}

}

NonnullRefPtr<Font> Typeface::font(float point_size, FontVariationSettings const& variations, Gfx::ShapeFeatures const& shape_features) const
{
    RefPtr<Font> font;
    {
        MutexLocker locker { m_fonts_mutex };
        FontCacheKey key { point_size, variations.to_sorted_list(), shape_features };

        // A font that another thread is destroying cannot be referenced any more, and a new font replaces it.
        if (auto it = m_fonts.find(key); it != m_fonts.end() && it->value->try_ref())
            return adopt_ref(*it->value);

        font = adopt_ref(*new Font(*this, point_size, point_size, variations, shape_features));
        m_fonts.set(move(key), font.ptr());
    }
    keep_recent_font(*font);
    return font.release_nonnull();
}

NonnullRefPtr<Font> Typeface::invisible_font(float point_size, FontVariationSettings const& variations, Gfx::ShapeFeatures const& shape_features) const
{
    auto font = adopt_ref(*new Font(*this, point_size, point_size, variations, shape_features));
    font->m_is_invisible = true;
    return font;
}

void Typeface::forget_font(Font const& font) const
{
    MutexLocker locker { m_fonts_mutex };
    FontCacheKey key { font.point_size(), font.variation_settings().to_sorted_list(), font.features() };
    // Invisible variants are not in the cache, and a newer font may have replaced this one.
    if (auto it = m_fonts.find(key); it != m_fonts.end() && it->value == &font)
        m_fonts.remove(it);
}

hb_face_t* Typeface::harfbuzz_typeface() const
{
    call_once(m_harfbuzz_face_once, [&] {
        m_harfbuzz_face = create_harfbuzz_face();
        hb_face_make_immutable(m_harfbuzz_face);
    });
    return m_harfbuzz_face;
}

u32 Typeface::glyph_count() const
{
    return hb_face_get_glyph_count(harfbuzz_typeface());
}

u16 Typeface::units_per_em() const
{
    return hb_face_get_upem(harfbuzz_typeface());
}

u32 Typeface::glyph_id_for_code_point(u32 code_point) const
{
    return glyph_page(code_point / GlyphPage::glyphs_per_page).glyph_ids[code_point % GlyphPage::glyphs_per_page];
}

Typeface::GlyphPage const& Typeface::glyph_page(size_t page_index) const
{
    struct GlyphPageCache {
        AK_ALLOC_WITH_KMALLOC;

        u64 last_use { 0 };
        OwnPtr<GlyphPage> page_zero;
        HashMap<size_t, NonnullOwnPtr<GlyphPage>> pages;
    };
    struct ThreadGlyphPageCaches {
        u64 last_typeface_id { 0 };
        GlyphPageCache* last_cache { nullptr };
        u64 use_clock { 0 };
        HashMap<u64, NonnullOwnPtr<GlyphPageCache>> caches;
    };
    // NB: -Wexit-time-destructors is a Clang-only warning, and GCC rejects the
    //     unknown option name in the pragma.
#ifdef AK_COMPILER_CLANG
    AK_IGNORE_DIAGNOSTIC("-Wexit-time-destructors", static thread_local ThreadGlyphPageCaches thread_caches)
#else
    static thread_local ThreadGlyphPageCaches thread_caches;
#endif

    auto& caches = thread_caches.caches;
    auto* cache = thread_caches.last_cache;
    if (thread_caches.last_typeface_id != m_glyph_cache_id) {
        if (auto it = caches.find(m_glyph_cache_id); it != caches.end()) {
            cache = it->value.ptr();
        } else {
            constexpr size_t maximum_cached_typefaces = 128;
            if (caches.size() >= maximum_cached_typefaces) {
                // NB: Evict the cache this thread used least recently. The caches of typefaces that are gone go first,
                //     and the ones a text run alternates between stay: evicting any other would have them evict each
                //     other at every switch once the caches of a long session filled up.
                auto least_recently_used = caches.begin();
                for (auto it = caches.begin(); it != caches.end(); ++it) {
                    if (it->value->last_use < least_recently_used->value->last_use)
                        least_recently_used = it;
                }
                caches.remove(least_recently_used);
            }
            auto new_cache = make<GlyphPageCache>();
            cache = new_cache.ptr();
            caches.set(m_glyph_cache_id, move(new_cache));
        }
        cache->last_use = ++thread_caches.use_clock;
        thread_caches.last_typeface_id = m_glyph_cache_id;
        thread_caches.last_cache = cache;
    }

    if (page_index == 0) {
        if (!cache->page_zero) {
            cache->page_zero = make<GlyphPage>();
            populate_glyph_page(*cache->page_zero, 0);
        }
        return *cache->page_zero;
    }
    if (auto it = cache->pages.find(page_index); it != cache->pages.end()) {
        return *it->value;
    }

    auto glyph_page = make<GlyphPage>();
    populate_glyph_page(*glyph_page, page_index);
    auto const* glyph_page_ptr = glyph_page.ptr();
    cache->pages.set(page_index, move(glyph_page));
    return *glyph_page_ptr;
}

static thread_local u64 s_glyph_pages_populated_on_this_thread = 0;

u64 Typeface::glyph_pages_populated_on_this_thread()
{
    return s_glyph_pages_populated_on_this_thread;
}

void Typeface::populate_glyph_page(GlyphPage& glyph_page, size_t page_index) const
{
    ++s_glyph_pages_populated_on_this_thread;
    u32 first_code_point = page_index * GlyphPage::glyphs_per_page;
    auto* font = cmap_font();
    auto glyph_count = this->glyph_count();
    for (size_t i = 0; i < GlyphPage::glyphs_per_page; ++i) {
        u32 code_point = first_code_point + i;
        hb_codepoint_t glyph_id = 0;
        // A cmap can name a glyph that the face does not have. Such a code point has no glyph.
        if (!hb_font_get_nominal_glyph(font, code_point, &glyph_id) || glyph_id >= glyph_count)
            glyph_id = 0;
        glyph_page.glyph_ids[i] = static_cast<u16>(glyph_id);
    }
}

// An unscaled font for code point lookups. The face's cmap gives the same glyphs at every size.
hb_font_t* Typeface::cmap_font() const
{
    call_once(m_cmap_font_once, [&] {
        m_cmap_font = hb_font_create(harfbuzz_typeface());
        hb_font_make_immutable(m_cmap_font);
    });
    return m_cmap_font;
}

FaceDescription const& Typeface::description() const
{
    call_once(m_description_once, [&] {
        m_description = FaceDescription::read(harfbuzz_typeface());
    });
    return *m_description;
}

FaceStyle Typeface::style_for_variations(ReadonlySpan<FontVariationAxis> variations) const
{
    if (auto style = fixed_style(); style.has_value())
        return *style;
    return description().style_for_variations(variations);
}

// Recording asks for this while the document thread may ask the same typeface for it, so the
// read of a half-written memo has to be impossible. The same `call_once` the HarfBuzz face uses
// above publishes it.
Typeface::BoundingBoxInFontUnits Typeface::bounding_box_in_font_units() const
{
    call_once(m_bounding_box_once, [&] {
        auto* face = harfbuzz_typeface();
        auto* head_table = hb_face_reference_table(face, HB_TAG('h', 'e', 'a', 'd'));
        unsigned head_table_length = 0;
        auto const* head_table_data = reinterpret_cast<u8 const*>(hb_blob_get_data(head_table, &head_table_length));

        // https://learn.microsoft.com/en-us/typography/opentype/spec/head
        // xMin, yMin, xMax and yMax are big-endian int16 values at byte offsets 36, 38, 40 and 42 of a 54-byte table.
        constexpr unsigned head_table_size = 54;
        if (head_table_data && head_table_length >= head_table_size) {
            auto read_i16 = [&](unsigned offset) {
                return static_cast<i16>((head_table_data[offset] << 8) | head_table_data[offset + 1]);
            };
            m_bounding_box_in_font_units.x_min = read_i16(36);
            m_bounding_box_in_font_units.y_min = read_i16(38);
            m_bounding_box_in_font_units.x_max = read_i16(40);
            m_bounding_box_in_font_units.y_max = read_i16(42);
            m_bounding_box_in_font_units.units_per_em = hb_face_get_upem(face);
        }
        hb_blob_destroy(head_table);
    });
    return m_bounding_box_in_font_units;
}

hb_face_t* Typeface::create_harfbuzz_face() const
{
    if (!m_harfbuzz_blob)
        m_harfbuzz_blob = hb_blob_create(reinterpret_cast<char const*>(buffer().data()), buffer().size(), HB_MEMORY_MODE_READONLY, nullptr, [](void*) { });
    return hb_face_create(m_harfbuzz_blob, ttc_index());
}

void Typeface::encode_font_data_for_ipc(IPC::Encoder& encoder) const
{
    if (m_system_font_identifier.has_value()) {
        MUST(encoder.encode(FontDataFormat::SystemFontId));
        MUST(encoder.encode(m_system_font_identifier->generation));
        MUST(encoder.encode(m_system_font_identifier->face_id));
        return;
    }

    if (m_file_path.has_value()) {
        MUST(encoder.encode(FontDataFormat::MappedFile));
        MUST(encoder.encode(*m_file_path));
        MUST(encoder.encode(ttc_index()));
        return;
    }

    VERIFY(m_font_data);

    m_font_data->storage.visit(
        [&](Core::AnonymousBuffer const& anonymous_buffer) {
            MUST(encoder.encode(FontDataFormat::RawFontData));
            MUST(encoder.encode(anonymous_buffer));
            MUST(encoder.encode(ttc_index()));
        },
        [&](NonnullOwnPtr<Core::MappedFile> const&) {
            // NB: SharedMappedFile backing should have an associated m_system_font_identifier or m_file_path and
            //     therefore have already been handled above.
            VERIFY_NOT_REACHED();
        });
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::Typeface const& typeface)
{
    typeface.encode_font_data_for_ipc(encoder);
    return {};
}

template<>
ErrorOr<NonnullRefPtr<Gfx::Typeface const>> decode(Decoder& decoder)
{
    auto format = TRY(decoder.decode<Gfx::Typeface::FontDataFormat>());

    switch (format) {
    case Gfx::Typeface::FontDataFormat::RawFontData: {
        auto font_data = TRY(decoder.decode<Core::AnonymousBuffer>());
        auto ttc_index = TRY(decoder.decode<u32>());
        if (!font_data.is_valid())
            return Error::from_string_literal("Typeface IPC data contained invalid font data");
        return TRY(Gfx::Typeface::try_load_from_anonymous_buffer(move(font_data), ttc_index));
    }
    case Gfx::Typeface::FontDataFormat::MappedFile: {
        auto file_path = TRY(decoder.decode<String>());
        auto ttc_index = TRY(decoder.decode<u32>());

        auto backing = make_ref_counted<Gfx::Typeface::FontDataBacking>(TRY(Core::MappedFile::map(file_path)));
        auto bytes = backing->storage.get<NonnullOwnPtr<Core::MappedFile>>()->bytes();

        auto typeface = TRY(Gfx::TypefaceSkia::load_from_buffer(bytes, ttc_index, backing));
        typeface->set_file_path(move(file_path));
        return typeface;
    }
    case Gfx::Typeface::FontDataFormat::SystemFont: {
        auto family_name = TRY(decoder.decode<String>());
        auto weight = TRY(decoder.decode<u16>());
        auto width = TRY(decoder.decode<u16>());
        auto slope = TRY(decoder.decode<u8>());
        auto typeface = TRY(Gfx::TypefaceSkia::match_family_style(family_name.bytes_as_string_view(), weight, width, slope));
        if (!typeface)
            return Error::from_string_literal("Typeface IPC data referred to an unavailable system font");
        return typeface.release_nonnull();
    }
    case Gfx::Typeface::FontDataFormat::SystemUIFont: {
        auto style = TRY(decoder.decode<Gfx::SystemUIFontStyle>());
#ifdef AK_OS_MACOS
        if (auto typeface = Gfx::TypefaceCoreText::system_ui(style))
            return typeface.release_nonnull();
#else
        (void)style;
#endif
        return Error::from_string_literal("Typeface IPC data referred to an unavailable system UI font");
    }
    case Gfx::Typeface::FontDataFormat::SystemFontId: {
        auto generation = TRY(decoder.decode<u64>());
        auto face_id = TRY(decoder.decode<u64>());
        auto matched_typeface = Gfx::FontDatabase::the().get_typeface_by_id(generation, face_id);
        if (!matched_typeface)
            return Error::from_string_literal("Typeface IPC data referred to an unavailable system font");
        return matched_typeface.release_nonnull();
    }
    }

    return Error::from_string_literal("Typeface IPC data contained invalid font data format");
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::SystemUIFontStyle const& style)
{
    TRY(encoder.encode(style.kind));
    TRY(encoder.encode(style.weight));
    TRY(encoder.encode(style.width));
    TRY(encoder.encode(style.slope));
    return {};
}

template<>
ErrorOr<Gfx::SystemUIFontStyle> decode(Decoder& decoder)
{
    auto kind = TRY(decoder.decode<Gfx::SystemUIFontKind>());
    auto weight = TRY(decoder.decode<u16>());
    auto width = TRY(decoder.decode<u16>());
    auto slope = TRY(decoder.decode<u8>());
    return Gfx::SystemUIFontStyle { kind, weight, width, slope };
}

}
