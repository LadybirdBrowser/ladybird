/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace Gfx {

namespace FFI {

struct FontCatalog;
struct FontCatalogBuilder;

}

enum class FontFileFormat : u8 {
    OpenType,
    WOFF,
};

struct FontCatalogFace {
    StringView family;
    u64 face_id { 0 };
    u32 ttc_index { 0 };
    u16 weight { 0 };
    u16 width { 0 };
    u8 slope { 0 };
    FontFileFormat format { FontFileFormat::OpenType };
};

class FontCatalogBuilder {
    AK_MAKE_NONCOPYABLE(FontCatalogBuilder);
    AK_MAKE_NONMOVABLE(FontCatalogBuilder);

public:
    static ErrorOr<NonnullOwnPtr<FontCatalogBuilder>> create(u64 generation);
    ~FontCatalogBuilder();

    ErrorOr<void> add_face(FontCatalogFace const&);
    ErrorOr<ByteBuffer> serialize() const;

private:
    explicit FontCatalogBuilder(FFI::FontCatalogBuilder&);

    FFI::FontCatalogBuilder* m_builder { nullptr };
};

class FontCatalog {
    AK_MAKE_NONCOPYABLE(FontCatalog);
    AK_MAKE_NONMOVABLE(FontCatalog);

public:
    static ErrorOr<NonnullOwnPtr<FontCatalog>> parse(ReadonlyBytes, u64 expected_generation);
    ~FontCatalog();

    u64 generation() const;
    size_t face_count() const;
    Optional<FontCatalogFace> face_at(size_t index) const;
    Optional<FontCatalogFace> face_by_id(u64 face_id) const;
    Optional<FontCatalogFace> match_style(StringView family, u16 weight, u16 width, u8 slope) const;
    size_t family_face_count(StringView family) const;
    Optional<FontCatalogFace> family_face_at(StringView family, size_t index) const;

private:
    explicit FontCatalog(FFI::FontCatalog&);

    FFI::FontCatalog* m_catalog { nullptr };
};

}
