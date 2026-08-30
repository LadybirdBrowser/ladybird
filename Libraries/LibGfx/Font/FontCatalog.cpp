/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/FontCatalog.h>
#include <LibGfx/RustFFI.h>

namespace Gfx {

static Optional<FontCatalogFace> convert_face(FFI::FfiFontCatalogFace const& face)
{
    if (face.format > to_underlying(FontFileFormat::WOFF))
        return {};
    return FontCatalogFace {
        .family = StringView { face.family, face.family_length },
        .face_id = face.face_id,
        .ttc_index = face.ttc_index,
        .weight = face.weight,
        .width = face.width,
        .slope = face.slope,
        .format = static_cast<FontFileFormat>(face.format),
    };
}

ErrorOr<NonnullOwnPtr<FontCatalogBuilder>> FontCatalogBuilder::create(u64 generation)
{
    auto* builder = FFI::font_catalog_builder_create(generation);
    if (!builder)
        return Error::from_string_literal("Failed to create font catalog builder");
    return adopt_nonnull_own_or_enomem(new (nothrow) FontCatalogBuilder(*builder));
}

FontCatalogBuilder::FontCatalogBuilder(FFI::FontCatalogBuilder& builder)
    : m_builder(&builder)
{
}

FontCatalogBuilder::~FontCatalogBuilder()
{
    FFI::font_catalog_builder_destroy(m_builder);
}

ErrorOr<void> FontCatalogBuilder::add_face(FontCatalogFace const& face)
{
    FFI::FfiFontCatalogFaceInput input {
        .family = reinterpret_cast<u8 const*>(face.family.characters_without_null_termination()),
        .family_length = face.family.length(),
        .face_id = face.face_id,
        .ttc_index = face.ttc_index,
        .weight = face.weight,
        .width = face.width,
        .slope = face.slope,
        .format = to_underlying(face.format),
    };
    if (!FFI::font_catalog_builder_add_face(m_builder, input))
        return Error::from_string_literal("Invalid or duplicate font catalog face");
    return {};
}

ErrorOr<ByteBuffer> FontCatalogBuilder::serialize() const
{
    auto size = FFI::font_catalog_builder_serialize(m_builder, nullptr, 0);
    if (size == 0)
        return Error::from_string_literal("Failed to size serialized font catalog");
    auto output = TRY(ByteBuffer::create_uninitialized(size));
    if (FFI::font_catalog_builder_serialize(m_builder, output.data(), output.size()) != output.size())
        return Error::from_string_literal("Failed to serialize font catalog");
    return output;
}

ErrorOr<NonnullOwnPtr<FontCatalog>> FontCatalog::parse(ReadonlyBytes bytes, u64 expected_generation)
{
    auto* catalog = FFI::font_catalog_parse(bytes.data(), bytes.size(), expected_generation);
    if (!catalog)
        return Error::from_string_literal("Invalid font catalog");
    return adopt_nonnull_own_or_enomem(new (nothrow) FontCatalog(*catalog));
}

FontCatalog::FontCatalog(FFI::FontCatalog& catalog)
    : m_catalog(&catalog)
{
}

FontCatalog::~FontCatalog()
{
    FFI::font_catalog_destroy(m_catalog);
}

u64 FontCatalog::generation() const
{
    return FFI::font_catalog_generation(m_catalog);
}

size_t FontCatalog::face_count() const
{
    return FFI::font_catalog_face_count(m_catalog);
}

Optional<FontCatalogFace> FontCatalog::face_at(size_t index) const
{
    FFI::FfiFontCatalogFace face {};
    if (!FFI::font_catalog_face_at(m_catalog, index, &face))
        return {};
    return convert_face(face);
}

Optional<FontCatalogFace> FontCatalog::face_by_id(u64 face_id) const
{
    FFI::FfiFontCatalogFace face {};
    if (!FFI::font_catalog_face_by_id(m_catalog, face_id, &face))
        return {};
    return convert_face(face);
}

Optional<FontCatalogFace> FontCatalog::match_style(StringView family, u16 weight, u16 width, u8 slope) const
{
    FFI::FfiFontCatalogFace face {};
    if (!FFI::font_catalog_match_style(m_catalog, reinterpret_cast<u8 const*>(family.characters_without_null_termination()), family.length(), weight, width, slope, &face))
        return {};
    return convert_face(face);
}

size_t FontCatalog::family_face_count(StringView family) const
{
    return FFI::font_catalog_family_face_count(m_catalog, reinterpret_cast<u8 const*>(family.characters_without_null_termination()), family.length());
}

Optional<FontCatalogFace> FontCatalog::family_face_at(StringView family, size_t index) const
{
    FFI::FfiFontCatalogFace face {};
    if (!FFI::font_catalog_family_face_at(m_catalog, reinterpret_cast<u8 const*>(family.characters_without_null_termination()), family.length(), index, &face))
        return {};
    return convert_face(face);
}

}
