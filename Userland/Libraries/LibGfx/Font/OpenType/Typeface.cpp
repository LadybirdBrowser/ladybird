/*
 * Copyright (c) 2020, Srimanta Barua <srimanta.barua1@gmail.com>
 * Copyright (c) 2021-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Jelle Raaijmakers <jelle@gmta.nl>
 * Copyright (c) 2023, Lukas Affolter <git@lukasach.dev>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/MemoryStream.h>
#include <AK/Try.h>
#include <LibCore/MappedFile.h>
#include <LibCore/Resource.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/DeprecatedPainter.h>
#include <LibGfx/Font/OpenType/Cmap.h>
#include <LibGfx/Font/OpenType/Glyf.h>
#include <LibGfx/Font/OpenType/Tables.h>
#include <LibGfx/Font/OpenType/Typeface.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <math.h>
#include <sys/mman.h>

namespace OpenType {

namespace {

class CmapCharCodeToGlyphIndex : public CharCodeToGlyphIndex {
public:
    static ErrorOr<NonnullOwnPtr<CharCodeToGlyphIndex>> from_slice(Optional<ReadonlyBytes>);

    virtual u32 glyph_id_for_code_point(u32 code_point) const override;

private:
    explicit CmapCharCodeToGlyphIndex(Cmap cmap)
        : m_cmap(cmap)
    {
    }

    Cmap m_cmap;
};

ErrorOr<NonnullOwnPtr<CharCodeToGlyphIndex>> CmapCharCodeToGlyphIndex::from_slice(Optional<ReadonlyBytes> opt_cmap_slice)
{
    if (!opt_cmap_slice.has_value())
        return Error::from_string_literal("Font is missing Cmap");

    auto cmap = TRY(Cmap::from_slice(opt_cmap_slice.value()));

    // Select cmap table. FIXME: Do this better. Right now, just looks for platform "Windows"
    // and corresponding encoding "Unicode full repertoire", or failing that, "Unicode BMP"
    Optional<u32> active_cmap_index;
    for (u32 i = 0; i < cmap.num_subtables(); i++) {
        auto opt_subtable = cmap.subtable(i);
        if (!opt_subtable.has_value()) {
            continue;
        }
        auto subtable = opt_subtable.value();
        auto platform = subtable.platform_id();
        if (!platform.has_value())
            return Error::from_string_literal("Invalid Platform ID");

        /* NOTE: The encoding records are sorted first by platform ID, then by encoding ID.
           This means that the Windows platform will take precedence over Macintosh, which is
           usually what we want here. */
        if (platform.value() == Cmap::Subtable::Platform::Unicode) {
            if (subtable.encoding_id() == (u16)Cmap::Subtable::UnicodeEncoding::Unicode2_0_FullRepertoire) {
                // "Encoding ID 3 should be used in conjunction with 'cmap' subtable formats 4 or 6."
                active_cmap_index = i;
                break;
            }
            if (subtable.encoding_id() == (u16)Cmap::Subtable::UnicodeEncoding::Unicode2_0_BMP_Only) {
                // "Encoding ID 4 should be used in conjunction with subtable formats 10 or 12."
                active_cmap_index = i;
                break;
            }
        } else if (platform.value() == Cmap::Subtable::Platform::Windows) {
            if (subtable.encoding_id() == (u16)Cmap::Subtable::WindowsEncoding::UnicodeFullRepertoire) {
                active_cmap_index = i;
                break;
            }
            if (subtable.encoding_id() == (u16)Cmap::Subtable::WindowsEncoding::UnicodeBMP) {
                active_cmap_index = i;
                break;
            }
        } else if (platform.value() == Cmap::Subtable::Platform::Macintosh) {
            active_cmap_index = i;
            // Intentionally no `break` so that Windows (value 3) wins over Macintosh (value 1).
        }
    }
    if (!active_cmap_index.has_value())
        return Error::from_string_literal("No suitable cmap subtable found");
    TRY(cmap.subtable(active_cmap_index.value()).value().validate_format_can_be_read());
    cmap.set_active_index(active_cmap_index.value());

    return adopt_nonnull_own_or_enomem(new CmapCharCodeToGlyphIndex(cmap));
}

u32 CmapCharCodeToGlyphIndex::glyph_id_for_code_point(u32 code_point) const
{
    return m_cmap.glyph_id_for_code_point(code_point);
}

}

// https://learn.microsoft.com/en-us/typography/opentype/spec/otff#ttc-header
struct [[gnu::packed]] TTCHeaderV1 {
    Tag ttc_tag;                      // Font Collection ID string: 'ttcf' (used for fonts with CFF or CFF2 outlines as well as TrueType outlines)
    BigEndian<u16> major_version;     // Major version of the TTC Header, = 1.
    BigEndian<u16> minor_version;     // Minor version of the TTC Header, = 0.
    BigEndian<u32> num_fonts;         // Number of fonts in TTC
    Offset32 table_directory_offsets; // Array of offsets to the TableDirectory for each font from the beginning of the file
};
static_assert(AssertSize<TTCHeaderV1, 16>());

}

template<>
class AK::Traits<OpenType::TTCHeaderV1> : public DefaultTraits<OpenType::TTCHeaderV1> {
public:
    static constexpr bool is_trivially_serializable() { return true; }
};

namespace OpenType {

u16 be_u16(u8 const*);
u32 be_u32(u8 const*);
i16 be_i16(u8 const*);

u16 be_u16(u8 const* ptr)
{
    return (((u16)ptr[0]) << 8) | ((u16)ptr[1]);
}

u32 be_u32(u8 const* ptr)
{
    return (((u32)ptr[0]) << 24) | (((u32)ptr[1]) << 16) | (((u32)ptr[2]) << 8) | ((u32)ptr[3]);
}

i16 be_i16(u8 const* ptr)
{
    return (((i16)ptr[0]) << 8) | ((i16)ptr[1]);
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_resource(Core::Resource const& resource, unsigned index)
{
    auto font_data = Gfx::FontData::create_from_resource(resource);
    return try_load_from_font_data(move(font_data), { .index = index });
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_font_data(NonnullOwnPtr<Gfx::FontData> font_data, Options options)
{
    auto font = TRY(try_load_from_externally_owned_memory(font_data->bytes(), move(options)));
    font->m_font_data = move(font_data);
    return font;
}

static ErrorOr<Tag> read_tag(ReadonlyBytes buffer)
{
    FixedMemoryStream stream { buffer };
    return stream.read_value<Tag>();
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_externally_owned_memory(ReadonlyBytes buffer, Options options)
{
    auto tag = TRY(read_tag(buffer));
    if (tag == HeaderTag_FontCollection) {
        // It's a font collection
        FixedMemoryStream stream { buffer };
        auto ttc_header_v1 = TRY(stream.read_in_place<TTCHeaderV1>());
        // FIXME: Check for major_version == 2.

        if (options.index >= ttc_header_v1->num_fonts)
            return Error::from_string_literal("Requested font index is too large");

        TRY(stream.seek(ttc_header_v1->table_directory_offsets + sizeof(u32) * options.index, SeekMode::SetPosition));
        auto offset = TRY(stream.read_value<BigEndian<u32>>());
        return try_load_from_offset(buffer, offset, move(options));
    }
    if (tag == HeaderTag_CFFOutlines)
        return Error::from_string_literal("CFF fonts not supported yet");

    if (tag != HeaderTag_TrueTypeOutlines && tag != HeaderTag_TrueTypeOutlinesApple)
        return Error::from_string_literal("Not a valid font");

    return try_load_from_offset(buffer, 0, move(options));
}

static ErrorOr<void> for_each_table_record(ReadonlyBytes buffer, u32 offset, Function<ErrorOr<void>(Tag, ReadonlyBytes)> callback)
{
    FixedMemoryStream stream { buffer };
    TRY(stream.seek(offset, AK::SeekMode::SetPosition));

    auto& table_directory = *TRY(stream.read_in_place<TableDirectory const>());
    for (auto i = 0; i < table_directory.num_tables; i++) {
        auto& table_record = *TRY(stream.read_in_place<TableRecord const>());

        if (Checked<u32>::addition_would_overflow(static_cast<u32>(table_record.offset), static_cast<u32>(table_record.length)))
            return Error::from_string_literal("Invalid table offset or length in font");

        if (buffer.size() < table_record.offset + table_record.length)
            return Error::from_string_literal("Font file too small");

        auto buffer_here = buffer.slice(table_record.offset, table_record.length);
        TRY(callback(table_record.table_tag, buffer_here));
    }

    return {};
}

// FIXME: "loca" and "glyf" are not available for CFF fonts.
ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_offset(ReadonlyBytes buffer, u32 offset, Options options)
{
    Optional<ReadonlyBytes> opt_head_slice = {};
    Optional<ReadonlyBytes> opt_name_slice = {};
    Optional<ReadonlyBytes> opt_hhea_slice = {};
    Optional<ReadonlyBytes> opt_maxp_slice = {};
    Optional<ReadonlyBytes> opt_hmtx_slice = {};
    Optional<ReadonlyBytes> opt_cmap_slice = {};
    Optional<ReadonlyBytes> opt_loca_slice = {};
    Optional<ReadonlyBytes> opt_glyf_slice = {};
    Optional<ReadonlyBytes> opt_os2_slice = {};
    Optional<ReadonlyBytes> opt_kern_slice = {};

    Optional<GPOS> gpos;

    TRY(for_each_table_record(buffer, offset, [&](Tag table_tag, ReadonlyBytes tag_buffer) -> ErrorOr<void> {
        // Get the table offsets we need.
        if (table_tag == Tag("head")) {
            opt_head_slice = tag_buffer;
        } else if (table_tag == Tag("name")) {
            opt_name_slice = tag_buffer;
        } else if (table_tag == Tag("hhea")) {
            opt_hhea_slice = tag_buffer;
        } else if (table_tag == Tag("maxp")) {
            opt_maxp_slice = tag_buffer;
        } else if (table_tag == Tag("hmtx")) {
            opt_hmtx_slice = tag_buffer;
        } else if (table_tag == Tag("cmap")) {
            opt_cmap_slice = tag_buffer;
        } else if (table_tag == Tag("loca")) {
            opt_loca_slice = tag_buffer;
        } else if (table_tag == Tag("glyf")) {
            opt_glyf_slice = tag_buffer;
        } else if (table_tag == Tag("OS/2")) {
            opt_os2_slice = tag_buffer;
        } else if (table_tag == Tag("kern")) {
            opt_kern_slice = tag_buffer;
        } else if (table_tag == Tag("GPOS")) {
            gpos = TRY(GPOS::from_slice(tag_buffer));
        }
        return {};
    }));

    if (!opt_head_slice.has_value())
        return Error::from_string_literal("Font is missing Head");
    auto head = TRY(Head::from_slice(opt_head_slice.value()));

    Optional<Name> name;
    if (!(options.skip_tables & Options::SkipTables::Name)) {
        if (!opt_name_slice.has_value())
            return Error::from_string_literal("Font is missing Name");
        name = TRY(Name::from_slice(opt_name_slice.value()));
    }

    if (!opt_hhea_slice.has_value())
        return Error::from_string_literal("Font is missing Hhea");
    auto hhea = TRY(Hhea::from_slice(opt_hhea_slice.value()));

    if (!opt_maxp_slice.has_value())
        return Error::from_string_literal("Font is missing Maxp");
    auto maxp = TRY(Maxp::from_slice(opt_maxp_slice.value()));

    bool can_omit_hmtx = (options.skip_tables & Options::SkipTables::Hmtx);
    Optional<Hmtx> hmtx;
    if (opt_hmtx_slice.has_value()) {
        auto hmtx_or_error = Hmtx::from_slice(opt_hmtx_slice.value(), maxp.num_glyphs(), hhea.number_of_h_metrics());
        if (!hmtx_or_error.is_error())
            hmtx = hmtx_or_error.release_value();
        else if (!can_omit_hmtx)
            return hmtx_or_error.release_error();
    } else if (!can_omit_hmtx) {
        return Error::from_string_literal("Font is missing Hmtx");
    }

    if (!options.external_cmap && !opt_cmap_slice.has_value())
        return Error::from_string_literal("Font is missing Cmap");
    NonnullOwnPtr<CharCodeToGlyphIndex> cmap = options.external_cmap ? options.external_cmap.release_nonnull() : TRY(CmapCharCodeToGlyphIndex::from_slice(opt_cmap_slice.value()));

    Optional<Loca> loca;
    if (opt_loca_slice.has_value())
        loca = TRY(Loca::from_slice(opt_loca_slice.value(), maxp.num_glyphs(), head.index_to_loc_format()));

    Optional<Glyf> glyf;
    if (opt_glyf_slice.has_value()) {
        glyf = Glyf(opt_glyf_slice.value());
    }

    Optional<OS2> os2;
    if (opt_os2_slice.has_value()) {
        auto os2_or_error = OS2::from_slice(opt_os2_slice.value());
        if (!os2_or_error.is_error())
            os2 = os2_or_error.release_value();
        else if (!(options.skip_tables & Options::SkipTables::OS2))
            return os2_or_error.release_error();
    }

    Optional<Kern> kern {};
    if (opt_kern_slice.has_value())
        kern = TRY(Kern::from_slice(opt_kern_slice.value()));

    return adopt_ref(*new Typeface(
        move(head),
        move(name),
        move(hhea),
        move(maxp),
        move(hmtx),
        move(cmap),
        move(loca),
        move(glyf),
        move(os2),
        move(kern),
        move(gpos),
        buffer.slice(offset),
        options.index));
}

Gfx::ScaledFontMetrics Typeface::metrics([[maybe_unused]] float x_scale, float y_scale) const
{
    i16 raw_ascender;
    i16 raw_descender;
    i16 raw_line_gap;
    Optional<i16> x_height;

    if (m_os2.has_value() && m_os2->use_typographic_metrics()) {
        raw_ascender = m_os2->typographic_ascender();
        raw_descender = m_os2->typographic_descender();
        raw_line_gap = m_os2->typographic_line_gap();
        x_height = m_os2->x_height();
    } else {
        raw_ascender = m_hhea.ascender();
        raw_descender = m_hhea.descender();
        raw_line_gap = m_hhea.line_gap();
    }

    if (!x_height.has_value()) {
        x_height = glyph_metrics(glyph_id_for_code_point('x'), 1, 1, 1, 1).ascender;
    }

    return Gfx::ScaledFontMetrics {
        .ascender = static_cast<float>(raw_ascender) * y_scale,
        .descender = -static_cast<float>(raw_descender) * y_scale,
        .line_gap = static_cast<float>(raw_line_gap) * y_scale,
        .x_height = static_cast<float>(x_height.value()) * y_scale,
    };
}

Gfx::ScaledGlyphMetrics Typeface::glyph_metrics(u32 glyph_id, float x_scale, float y_scale, float, float) const
{
    if (!m_loca.has_value() || !m_glyf.has_value() || !m_hmtx.has_value()) {
        return Gfx::ScaledGlyphMetrics {};
    }

    if (glyph_id >= glyph_count()) {
        glyph_id = 0;
    }
    auto horizontal_metrics = m_hmtx->get_glyph_horizontal_metrics(glyph_id);
    auto glyph_offset = m_loca->get_glyph_offset(glyph_id);
    auto glyph = m_glyf->glyph(glyph_offset);
    return Gfx::ScaledGlyphMetrics {
        .ascender = glyph.has_value() ? static_cast<float>(glyph->ascender()) * y_scale : 0,
        .descender = glyph.has_value() ? static_cast<float>(glyph->descender()) * y_scale : 0,
        .advance_width = static_cast<float>(horizontal_metrics.advance_width) * x_scale,
        .left_side_bearing = static_cast<float>(horizontal_metrics.left_side_bearing) * x_scale,
    };
}

u32 Typeface::glyph_count() const
{
    return m_maxp.num_glyphs();
}

u16 Typeface::units_per_em() const
{
    return m_head.units_per_em();
}

String Typeface::family() const
{
    if (!m_name.has_value())
        return {};

    if (!m_family.has_value()) {
        m_family = [&] {
            auto string = m_name->typographic_family_name();
            if (!string.is_empty())
                return string;
            return m_name->family_name();
        }();
    }
    return *m_family;
}

u16 Typeface::weight() const
{
    if (!m_weight.has_value()) {
        m_weight = [&]() -> u16 {
            constexpr u16 bold_bit { 1 };
            if (m_os2.has_value() && m_os2->weight_class())
                return m_os2->weight_class();
            if (m_head.style() & bold_bit)
                return 700;
            return 400;
        }();
    }
    return *m_weight;
}

u16 Typeface::width() const
{
    if (!m_width.has_value()) {
        m_width = [&]() -> u16 {
            if (m_os2.has_value())
                return m_os2->width_class();
            return Gfx::FontWidth::Normal;
        }();
    }
    return *m_width;
}

u8 Typeface::slope() const
{
    if (!m_slope.has_value()) {
        m_slope = [&]() -> u8 {
            // https://docs.microsoft.com/en-us/typography/opentype/spec/os2
            constexpr u16 italic_selection_bit { 1 };
            constexpr u16 oblique_selection_bit { 512 };
            // https://docs.microsoft.com/en-us/typography/opentype/spec/head
            constexpr u16 italic_style_bit { 2 };

            if (m_os2.has_value() && m_os2->selection() & oblique_selection_bit)
                return 2;
            if (m_os2.has_value() && m_os2->selection() & italic_selection_bit)
                return 1;
            if (m_head.style() & italic_style_bit)
                return 1;
            return 0;
        }();
    }
    return *m_slope;
}

u32 Typeface::glyph_id_for_code_point(u32 code_point) const
{
    return glyph_page(code_point / GlyphPage::glyphs_per_page).glyph_ids[code_point % GlyphPage::glyphs_per_page];
}

Typeface::GlyphPage const& Typeface::glyph_page(size_t page_index) const
{
    if (page_index == 0) {
        if (!m_glyph_page_zero) {
            m_glyph_page_zero = make<GlyphPage>();
            populate_glyph_page(*m_glyph_page_zero, 0);
        }
        return *m_glyph_page_zero;
    }
    if (auto it = m_glyph_pages.find(page_index); it != m_glyph_pages.end()) {
        return *it->value;
    }

    auto glyph_page = make<GlyphPage>();
    populate_glyph_page(*glyph_page, page_index);
    auto const* glyph_page_ptr = glyph_page.ptr();
    m_glyph_pages.set(page_index, move(glyph_page));
    return *glyph_page_ptr;
}

void Typeface::populate_glyph_page(GlyphPage& glyph_page, size_t page_index) const
{
    u32 first_code_point = page_index * GlyphPage::glyphs_per_page;
    for (size_t i = 0; i < GlyphPage::glyphs_per_page; ++i) {
        u32 code_point = first_code_point + i;
        glyph_page.glyph_ids[i] = m_cmap->glyph_id_for_code_point(code_point);
    }
}

}
