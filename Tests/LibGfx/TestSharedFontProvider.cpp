/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/ResourceImplementation.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontCatalog.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/Font/SharedFontProvider.h>
#include <LibTest/TestCase.h>

namespace {

struct Global {
    Global()
    {
        Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::formatted("{}/Base/res", LADYBIRD_SOURCE_DIR))));
    }
} global;

static IPC::File copy_bytes_to_file(ReadonlyBytes bytes)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(bytes.size()));
    bytes.copy_to({ buffer.data<u8>(), buffer.size() });
    return MUST(IPC::File::clone_fd(buffer.fd()));
}

static NonnullOwnPtr<Core::MappedFile> map_bytes(ReadonlyBytes bytes)
{
    auto file = copy_bytes_to_file(bytes);
    return MUST(Core::MappedFile::map_from_fd_range_and_close(file.take_fd(), "font catalog test"sv, 0, bytes.size()));
}

static ByteBuffer make_catalog()
{
    auto builder = MUST(Gfx::FontCatalogBuilder::create(9));
    MUST(builder->add_face({
        .family = "Brokered Test"sv,
        .face_id = 17,
        .ttc_index = 0,
        .weight = 400,
        .width = Gfx::FontWidth::Normal,
        .slope = 0,
        .format = Gfx::FontFileFormat::OpenType,
    }));
    return MUST(builder->serialize());
}

static Gfx::BrokeredFont open_test_font(u64 face_id)
{
    auto path = MUST(String::formatted("{}/Tests/LibGfx/test-inputs/fonts/text.ttf", LADYBIRD_SOURCE_DIR));
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return {
        .face_id = face_id,
        .ttc_index = 0,
        .format = Gfx::FontFileFormat::OpenType,
        .file = IPC::File::adopt_file(move(file)),
    };
}

}

TEST_CASE(opens_catalog_faces_lazily_and_caches_them)
{
    auto catalog = make_catalog();
    size_t open_count = 0;
    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.open_font = [&](u64 generation, u64 face_id) {
        EXPECT_EQ(generation, 9u);
        EXPECT_EQ(face_id, 17u);
        ++open_count;
        return open_test_font(face_id);
    };

    auto provider = MUST(Gfx::SharedFontProvider::create(map_bytes(catalog), 9, move(callbacks)));
    EXPECT_EQ(open_count, 0u);

    auto first_font = provider->get_font("Brokered Test"_fly_string, 12, 400, Gfx::FontWidth::Normal, 0);
    EXPECT(first_font);
    EXPECT_EQ(open_count, 1u);

    auto second_font = provider->get_font("Brokered Test"_fly_string, 16, 400, Gfx::FontWidth::Normal, 0);
    EXPECT(second_font);
    EXPECT_EQ(open_count, 1u);

    auto identifier = first_font->typeface().system_font_identifier();
    EXPECT(identifier.has_value());
    EXPECT_EQ(identifier->generation, 9u);
    EXPECT_EQ(identifier->face_id, 17u);
}

TEST_CASE(negatively_caches_failed_catalog_face_opens)
{
    auto catalog = make_catalog();
    size_t open_count = 0;
    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.open_font = [&](u64, u64 face_id) {
        ++open_count;
        return Gfx::BrokeredFont {
            .face_id = face_id,
            .ttc_index = 0,
            .format = Gfx::FontFileFormat::OpenType,
            .file = {},
        };
    };

    auto provider = MUST(Gfx::SharedFontProvider::create(map_bytes(catalog), 9, move(callbacks)));
    EXPECT(!provider->get_font("Brokered Test"_fly_string, 12, 400, Gfx::FontWidth::Normal, 0));
    EXPECT(!provider->get_font("Brokered Test"_fly_string, 16, 400, Gfx::FontWidth::Normal, 0));
    EXPECT_EQ(open_count, 1u);
}

TEST_CASE(empty_catalog_preserves_platform_broker_fallback)
{
    size_t match_count = 0;
    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.match_font = [&](String const& family, u16 weight, u16 width, u8 slope) {
        EXPECT_EQ(family, "Platform Alias"sv);
        EXPECT_EQ(weight, 400u);
        EXPECT_EQ(width, Gfx::FontWidth::Normal);
        EXPECT_EQ(slope, 0u);
        ++match_count;
        return open_test_font(91);
    };

    auto malformed = MUST(ByteBuffer::copy("not a font catalog"sv.bytes()));
    auto provider = Gfx::SharedFontProvider::create(map_bytes(malformed), 9, move(callbacks));
    EXPECT(provider.is_error());

    provider = Gfx::SharedFontProvider::create_empty(9, move(callbacks));
    EXPECT(!provider.is_error());
    auto font = provider.value()->get_font("Platform Alias"_fly_string, 12, 400, Gfx::FontWidth::Normal, 0);
    EXPECT(font);
    EXPECT_EQ(match_count, 1u);

    auto identifier = font->typeface().system_font_identifier();
    EXPECT(identifier.has_value());
    EXPECT_EQ(identifier->generation, 9u);
    EXPECT_EQ(identifier->face_id, 91u);
}

TEST_CASE(catalog_file_factory_preserves_callbacks_during_fallback)
{
    size_t match_count = 0;
    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.match_font = [&](String const&, u16, u16, u8) {
        ++match_count;
        return open_test_font(92);
    };

    auto malformed = MUST(ByteBuffer::copy("not a font catalog"sv.bytes()));
    auto file = copy_bytes_to_file(malformed);
    auto provider = MUST(Gfx::SharedFontProvider::create_from_catalog_file_or_empty(move(file), malformed.size(), 9, move(callbacks)));

    EXPECT(provider->get_font("Platform Alias"_fly_string, 12, 400, Gfx::FontWidth::Normal, 0));
    EXPECT_EQ(match_count, 1u);
}

TEST_CASE(catalog_file_factory_handles_mapping_failure)
{
    size_t match_count = 0;
    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.match_font = [&](String const&, u16, u16, u8) {
        ++match_count;
        return open_test_font(96);
    };

    auto provider = MUST(Gfx::SharedFontProvider::create_from_catalog_file_or_empty({}, 1, 9, move(callbacks)));

    EXPECT(provider->get_font("Platform Alias"_fly_string, 12, 400, Gfx::FontWidth::Normal, 0));
    EXPECT_EQ(match_count, 1u);
}

TEST_CASE(rejecting_replacement_preserves_current_catalog)
{
    auto catalog = make_catalog();
    size_t open_count = 0;
    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.open_font = [&](u64, u64 face_id) {
        ++open_count;
        return open_test_font(face_id);
    };

    auto provider = MUST(Gfx::SharedFontProvider::create(map_bytes(catalog), 9, move(callbacks)));
    auto malformed = MUST(ByteBuffer::copy("not a font catalog"sv.bytes()));
    auto file = copy_bytes_to_file(malformed);
    EXPECT(provider->replace_catalog(move(file), malformed.size(), 9).is_error());

    EXPECT(provider->get_font("Brokered Test"_fly_string, 12, 400, Gfx::FontWidth::Normal, 0));
    EXPECT_EQ(open_count, 1u);
}

TEST_CASE(unknown_width_uses_normal_variation_width)
{
    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.match_font = [](String const&, u16, u16, u8) {
        return open_test_font(93);
    };

    auto provider = MUST(Gfx::SharedFontProvider::create_empty(9, move(callbacks)));
    auto font = provider->get_font("Platform Alias"_fly_string, 12, 400, 99, 0);
    EXPECT(font);

    auto variation_width = font->variation_settings().axes.get(Gfx::FourCC("wdth"));
    EXPECT(variation_width.has_value());
    EXPECT_EQ(variation_width.value(), 100.0f);
}

TEST_CASE(caches_code_point_fallback_matches_and_misses)
{
    size_t match_count = 0;
    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.match_font_for_code_point = [&](u32 code_point, u16, u16, u8, bool) {
        ++match_count;
        if (code_point == 'A')
            return open_test_font(94);
        return Gfx::BrokeredFont {};
    };

    auto provider = MUST(Gfx::SharedFontProvider::create_empty(9, move(callbacks)));
    EXPECT(provider->get_font_for_code_point('A', 12, 400, Gfx::FontWidth::Normal, 0, false));
    EXPECT(provider->get_font_for_code_point('A', 16, 400, Gfx::FontWidth::Normal, 0, false));
    EXPECT_EQ(match_count, 1u);

    EXPECT(!provider->get_font_for_code_point('B', 12, 400, Gfx::FontWidth::Normal, 0, false));
    EXPECT(!provider->get_font_for_code_point('B', 16, 400, Gfx::FontWidth::Normal, 0, false));
    EXPECT_EQ(match_count, 2u);

    EXPECT(provider->get_font_for_code_point('A', 12, 700, Gfx::FontWidth::Normal, 0, false));
    EXPECT(provider->get_font_for_code_point('A', 12, 400, Gfx::FontWidth::Normal, 0, true));
    EXPECT_EQ(match_count, 4u);
}

TEST_CASE(replacing_catalog_clears_code_point_fallback_cache)
{
    size_t match_count = 0;
    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.match_font_for_code_point = [&](u32, u16, u16, u8, bool) {
        ++match_count;
        return open_test_font(95);
    };

    auto provider = MUST(Gfx::SharedFontProvider::create_empty(9, move(callbacks)));
    EXPECT(provider->get_font_for_code_point('A', 12, 400, Gfx::FontWidth::Normal, 0, false));
    EXPECT_EQ(match_count, 1u);

    auto replacement = make_catalog();
    MUST(provider->replace_catalog(map_bytes(replacement), 9));
    EXPECT(provider->get_font_for_code_point('A', 12, 400, Gfx::FontWidth::Normal, 0, false));
    EXPECT_EQ(match_count, 2u);
}

#ifndef AK_OS_WINDOWS
TEST_CASE(path_provider_deduplicates_canonical_paths)
{
    auto font_directory = MUST(String::formatted("{}/Tests/LibGfx/test-inputs/fonts", LADYBIRD_SOURCE_DIR));
    auto source_directory = MUST(FileSystem::real_path(font_directory));
    auto temporary_directory = MUST(String::formatted("{}/ladybird-font-dedup-{:016x}", Core::StandardPaths::tempfile_directory(), get_random<u64>()));
    MUST(Core::Directory::create(temporary_directory.to_byte_string(), Core::Directory::CreateDirectories::Yes));
    auto symlink_path = MUST(String::formatted("{}/fonts", temporary_directory));
    ScopeGuard cleanup = [&] {
        (void)Core::System::unlink(symlink_path);
        (void)Core::System::rmdir(temporary_directory);
    };
    MUST(Core::System::symlink(source_directory, symlink_path));

    HashTable<String> loaded_paths;
    size_t face_count = 0;
    Gfx::PathFontProvider::for_each_typeface_in_uri(MUST(String::formatted("file://{}", source_directory)), loaded_paths, [&](String const&, u32, Gfx::FontFileFormat, NonnullRefPtr<Gfx::Typeface>) {
        ++face_count;
    });
    auto original_face_count = face_count;
    EXPECT(original_face_count > 0);

    Gfx::PathFontProvider::for_each_typeface_in_uri(MUST(String::formatted("file://{}", symlink_path)), loaded_paths, [&](String const&, u32, Gfx::FontFileFormat, NonnullRefPtr<Gfx::Typeface>) {
        ++face_count;
    });
    EXPECT_EQ(face_count, original_face_count);
}
#endif
