/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/FontCatalog.h>
#include <LibTest/TestCase.h>

static ByteBuffer make_catalog()
{
    auto builder = MUST(Gfx::FontCatalogBuilder::create(42));
    MUST(builder->add_face({
        .family = "Example Sans"sv,
        .face_id = 17,
        .ttc_index = 2,
        .weight = 400,
        .width = 5,
        .slope = 0,
        .format = Gfx::FontFileFormat::OpenType,
    }));
    MUST(builder->add_face({
        .family = "Example Sans"sv,
        .face_id = 23,
        .ttc_index = 2,
        .weight = 700,
        .width = 5,
        .slope = 1,
        .format = Gfx::FontFileFormat::OpenType,
    }));
    return MUST(builder->serialize());
}

TEST_CASE(round_trip_and_lookup)
{
    auto bytes = make_catalog();
    auto catalog = MUST(Gfx::FontCatalog::parse(bytes, 42));

    EXPECT_EQ(catalog->generation(), 42u);
    EXPECT_EQ(catalog->face_count(), 2u);
    EXPECT_EQ(catalog->family_face_count("example sans"sv), 2u);

    auto face = catalog->match_style("EXAMPLE SANS"sv, 700, 5, 1);
    EXPECT(face.has_value());
    EXPECT_EQ(face->face_id, 23u);
    EXPECT_EQ(face->ttc_index, 2u);
    EXPECT(!catalog->match_style("Example Sans"sv, 500, 5, 0).has_value());

    face = catalog->face_by_id(17);
    EXPECT(face.has_value());
    EXPECT_EQ(face->family, "Example Sans"sv);
    EXPECT(!catalog->face_by_id(99).has_value());
}

TEST_CASE(builder_rejects_invalid_faces)
{
    auto builder = MUST(Gfx::FontCatalogBuilder::create(1));
    EXPECT(builder->add_face({ .family = "Invalid ID"sv, .face_id = 0, .weight = 400, .width = 5, .slope = 0 }).is_error());
    EXPECT(builder->add_face({ .family = "Invalid"sv, .face_id = 1, .weight = 400, .width = 5, .slope = 3 }).is_error());
    MUST(builder->add_face({ .family = "Valid"sv, .face_id = 1, .weight = 400, .width = 5, .slope = 0 }));
    EXPECT(builder->add_face({ .family = "Duplicate"sv, .face_id = 1, .weight = 400, .width = 5, .slope = 0 }).is_error());
    EXPECT(builder->add_face({ .family = ""sv, .face_id = 2, .weight = 400, .width = 5, .slope = 0 }).is_error());
    EXPECT(builder->add_face({ .family = "Bad Weight"sv, .face_id = 3, .weight = 0, .width = 5, .slope = 0 }).is_error());
    EXPECT(builder->add_face({ .family = "Bad Weight"sv, .face_id = 3, .weight = 1001, .width = 5, .slope = 0 }).is_error());
    EXPECT(builder->add_face({ .family = "Bad Width"sv, .face_id = 3, .weight = 400, .width = 0, .slope = 0 }).is_error());
    EXPECT(builder->add_face({ .family = "Bad Width"sv, .face_id = 3, .weight = 400, .width = 10, .slope = 0 }).is_error());
}

TEST_CASE(parser_rejects_malformed_snapshots)
{
    auto bytes = make_catalog();
    EXPECT(Gfx::FontCatalog::parse(bytes.bytes().slice(0, bytes.size() - 1), 42).is_error());
    EXPECT(Gfx::FontCatalog::parse(bytes, 41).is_error());

    auto trailing_data = MUST(ByteBuffer::copy(bytes));
    trailing_data.append(0);
    EXPECT(Gfx::FontCatalog::parse(trailing_data, 42).is_error());

    auto invalid_magic = MUST(ByteBuffer::copy(bytes));
    invalid_magic[0] = 'X';
    EXPECT(Gfx::FontCatalog::parse(invalid_magic, 42).is_error());

    auto invalid_version = MUST(ByteBuffer::copy(bytes));
    invalid_version[8] = 2;
    EXPECT(Gfx::FontCatalog::parse(invalid_version, 42).is_error());

    auto invalid_string = MUST(ByteBuffer::copy(bytes));
    invalid_string[112] = 0xff;
    EXPECT(Gfx::FontCatalog::parse(invalid_string, 42).is_error());

    auto invalid_format = MUST(ByteBuffer::copy(bytes));
    invalid_format[77] = 2;
    EXPECT(Gfx::FontCatalog::parse(invalid_format, 42).is_error());

    auto out_of_range_string = MUST(ByteBuffer::copy(bytes));
    for (size_t index = 56; index < 64; ++index)
        out_of_range_string[index] = 0xff;
    EXPECT(Gfx::FontCatalog::parse(out_of_range_string, 42).is_error());

    auto duplicate_id = MUST(ByteBuffer::copy(bytes));
    for (size_t index = 0; index < sizeof(u64); ++index)
        duplicate_id[80 + index] = duplicate_id[48 + index];
    EXPECT(Gfx::FontCatalog::parse(duplicate_id, 42).is_error());
}
