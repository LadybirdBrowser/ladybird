/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/MappedFile.h>
#include <LibFileSystem/FileSystem.h>
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

    // A platform font is one face by its PostScript name.
    auto platform_font = WebView::FontServiceTestAccess::materialize(*service, Gfx::PlatformFontName { "Ladybird-Test"_string }, "platform-font"_string);
    auto repeated_platform_font = WebView::FontServiceTestAccess::materialize(*service, Gfx::PlatformFontName { "Ladybird-Test"_string }, "repeated-platform-font"_string);
    EXPECT_NE(platform_font.face_id, 0u);
    EXPECT_EQ(platform_font.face_id, repeated_platform_font.face_id);
    EXPECT_NE(platform_font.face_id, first.face_id);
    EXPECT(platform_font.source.has<Gfx::PlatformFontName>());

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
