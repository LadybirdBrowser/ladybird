/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCore/MappedFile.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibTest/TestCase.h>
#include <LibWebView/FontService.h>
#include <sys/mman.h>

TEST_CASE(font_catalog_is_shared_read_only)
{
    auto font_service = WebView::FontService::create({});
    auto catalog = MUST(font_service->clone_catalog());
    VERIFY(catalog.size > 0);

    // Every helper gets this descriptor. None of them may change the catalog that the others read. On Linux the
    // descriptor stays read-write and seals refuse writable mappings, elsewhere it is opened read-only.
    auto fd = catalog.file.fd();
    EXPECT_EQ(mmap(nullptr, catalog.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0), MAP_FAILED);

    auto* mapping = mmap(nullptr, catalog.size, PROT_READ, MAP_SHARED, fd, 0);
    EXPECT_NE(mapping, MAP_FAILED);
}

namespace WebView {

struct FontServiceTestAccess {
    static Gfx::BrokeredFont materialize(FontService& service, Gfx::SystemFontMatch match, String key)
    {
        MutexLocker locker(service.m_mutex);
        MUST(service.wait_until_ready());
        return service.materialize(move(match), move(key));
    }
};

}

static Gfx::SystemFontFile test_font_file(StringView name, u32 ttc_index = 0)
{
    auto path = MUST(FileSystem::real_path(ByteString::formatted("../LibGfx/test-inputs/fonts/{}", name)));
    return { .path = MUST(String::from_byte_string(path)), .ttc_index = ttc_index };
}

TEST_CASE(dynamic_matches_of_one_face_share_its_id)
{
    auto service = WebView::FontService::create({});
    auto first = WebView::FontServiceTestAccess::materialize(*service, test_font_file("text.ttf"sv), "first-match"_string);
    auto repeated = WebView::FontServiceTestAccess::materialize(*service, test_font_file("text.ttf"sv), "second-match"_string);
    auto other_file = WebView::FontServiceTestAccess::materialize(*service, test_font_file("mono-emoji.ttf"sv), "other-file"_string);
    auto other_face = WebView::FontServiceTestAccess::materialize(*service, test_font_file("styles.ttc"sv, 1), "other-face"_string);
    auto first_face = WebView::FontServiceTestAccess::materialize(*service, test_font_file("styles.ttc"sv, 0), "first-face"_string);
    EXPECT_NE(first.face_id, 0u);
    EXPECT_EQ(first.face_id, repeated.face_id);
    EXPECT_NE(first.face_id, other_file.face_id);
    EXPECT_NE(other_face.face_id, first_face.face_id);

    // The renderer gets the file itself, not a copy of its data.
    auto* font_file = first.source.get_pointer<Gfx::BrokeredFontFile>();
    EXPECT(font_file);
    auto mapped_file = MUST(Core::MappedFile::map_from_fd_and_close(font_file->file.take_fd(), "brokered font"sv));
    auto original_file = MUST(Core::MappedFile::map("../LibGfx/test-inputs/fonts/text.ttf"sv));
    EXPECT_EQ(mapped_file->bytes(), original_file->bytes());
}

TEST_CASE(dynamic_matches_of_a_catalog_face_use_its_catalog_id)
{
    auto directory = MUST(FileSystem::real_path("../LibGfx/test-inputs/fonts"sv));
    auto service = WebView::FontService::create({ MUST(String::from_byte_string(directory)) });

    auto file = MUST(Core::MappedFile::map("../LibGfx/test-inputs/fonts/text.ttf"sv));
    auto postscript_name = MUST(Gfx::Typeface::try_load_from_temporary_memory(file->bytes()))->postscript_name();
    EXPECT(postscript_name.has_value());
    auto catalog_face = service->match_local_font(*postscript_name);
    EXPECT_NE(catalog_face.face_id, 0u);

    auto match = WebView::FontServiceTestAccess::materialize(*service, test_font_file("text.ttf"sv), "catalog-face"_string);
    EXPECT_EQ(match.face_id, catalog_face.face_id);
}

TEST_CASE(dynamic_matches_reuse_platform_faces_without_merging_equal_metadata)
{
    auto file = MUST(Core::MappedFile::map("../LibGfx/test-inputs/fonts/text.ttf"sv));
    NonnullRefPtr<Gfx::TypefaceSkia> first_face = as<Gfx::TypefaceSkia>(*MUST(Gfx::Typeface::try_load_from_temporary_memory(file->bytes())));
    // Change a glyph advance without changing the family, style, collection index, or byte length.
    auto second_data = MUST(ByteBuffer::copy(file->bytes()));
    auto bytes = second_data.bytes();
    VERIFY(bytes.size() >= 12);
    size_t table_count = (static_cast<u16>(bytes[4]) << 8) | bytes[5];
    bool changed_metric = false;
    for (size_t index = 0; index < table_count; ++index) {
        size_t entry = 12 + 16 * index;
        VERIFY(entry + 16 <= bytes.size());
        if (bytes[entry] != 'h' || bytes[entry + 1] != 'm' || bytes[entry + 2] != 't' || bytes[entry + 3] != 'x')
            continue;
        u32 offset = (static_cast<u32>(bytes[entry + 8]) << 24)
            | (static_cast<u32>(bytes[entry + 9]) << 16)
            | (static_cast<u32>(bytes[entry + 10]) << 8) | bytes[entry + 11];
        VERIFY(static_cast<size_t>(offset) + 2 <= bytes.size());
        bytes[offset + 1] ^= 1;
        changed_metric = true;
        break;
    }
    VERIFY(changed_metric);
    NonnullRefPtr<Gfx::TypefaceSkia> second_face = as<Gfx::TypefaceSkia>(*MUST(Gfx::Typeface::try_load_from_temporary_memory(bytes)));
    EXPECT_NE(first_face->platform_typeface_id(), second_face->platform_typeface_id());
    EXPECT_EQ(first_face->family(), second_face->family());
    EXPECT_EQ(first_face->weight(), second_face->weight());
    EXPECT_EQ(first_face->width(), second_face->width());
    EXPECT_EQ(first_face->slope(), second_face->slope());
    EXPECT_EQ(first_face->collection_index(), second_face->collection_index());
    EXPECT_EQ(first_face->font_data().size(), second_face->font_data().size());

    auto service = WebView::FontService::create({});
    auto first = WebView::FontServiceTestAccess::materialize(*service, NonnullRefPtr<Gfx::Typeface> { first_face }, "first-match"_string);
    auto repeated = WebView::FontServiceTestAccess::materialize(*service, NonnullRefPtr<Gfx::Typeface> { first_face }, "second-match"_string);
    auto distinct = WebView::FontServiceTestAccess::materialize(*service, NonnullRefPtr<Gfx::Typeface> { second_face }, "distinct-face"_string);
    EXPECT_NE(first.face_id, 0u);
    EXPECT_NE(repeated.face_id, 0u);
    EXPECT_NE(distinct.face_id, 0u);
    EXPECT_EQ(first.face_id, repeated.face_id);
    EXPECT_NE(first.face_id, distinct.face_id);
}
