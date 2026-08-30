/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibCore/File.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibWebView/FontService.h>

#include <fcntl.h>

namespace WebView {

NonnullOwnPtr<FontService> FontService::create()
{
    return adopt_own(*new FontService);
}

FontService::FontService()
    : m_worker(Threading::Thread::construct("Font catalog"sv, [this] {
        if (auto result = build_catalog(); result.is_error()) {
            dbgln("Unable to discover system fonts: {}. Using an empty catalog.", result.error());
            m_font_sources.clear();
            if (auto fallback_result = build_empty_catalog(); fallback_result.is_error())
                m_build_error = MUST(String::formatted("{}", fallback_result.error()));
        }
        return 0;
    }))
{
    m_worker->start();
}

FontService::~FontService()
{
    if (m_worker->needs_to_be_joined())
        (void)m_worker->join();
}

ErrorOr<void> FontService::wait_until_ready()
{
    if (m_worker->needs_to_be_joined()) {
        auto result = m_worker->join();
        if (result.is_error())
            return Error::from_errno(result.error().value());
    }
    if (m_build_error.has_value())
        return Error::from_string_view(m_build_error->bytes_as_string_view());
    return {};
}

ErrorOr<FontCatalogDescriptor> FontService::clone_catalog()
{
    TRY(wait_until_ready());
    return FontCatalogDescriptor {
        .file = TRY(IPC::File::clone_fd(m_catalog_file.fd())),
        .size = m_catalog_size,
        .generation = m_generation,
    };
}

ErrorOr<void> FontService::build_catalog()
{
    auto builder = TRY(Gfx::FontCatalogBuilder::create(m_generation));
    HashTable<String> loaded_paths;
    Optional<Error> callback_error;
    u64 next_face_id = 1;

    auto directories = TRY(Gfx::FontDatabase::font_directories());
    for (auto const& directory : directories) {
        auto uri = TRY(String::formatted("file://{}", directory));
        Gfx::PathFontProvider::for_each_typeface_in_uri(uri, loaded_paths, [&](String const& path, u32 ttc_index, Gfx::FontFileFormat format, NonnullRefPtr<Gfx::Typeface> typeface) {
            if (callback_error.has_value())
                return;
            auto face_id = next_face_id++;
            auto result = builder->add_face({
                .family = typeface->family().bytes_as_string_view(),
                .face_id = face_id,
                .ttc_index = ttc_index,
                .weight = typeface->weight(),
                .width = typeface->width(),
                .slope = typeface->slope(),
                .format = format,
            });
            if (result.is_error()) {
                callback_error = result.release_error();
                return;
            }
            m_font_sources.set(face_id, FontSource {
                                            .path = path,
                                            .ttc_index = ttc_index,
                                            .format = format,
                                        });
        });
        if (callback_error.has_value())
            return callback_error.release_value();
    }

    auto serialized = TRY(builder->serialize());
    m_catalog_size = serialized.size();
    m_catalog_file = TRY(create_immutable_font_data(serialized.bytes()));
    return {};
}

ErrorOr<void> FontService::build_empty_catalog()
{
    auto builder = TRY(Gfx::FontCatalogBuilder::create(m_generation));
    auto serialized = TRY(builder->serialize());
    m_catalog_size = serialized.size();
    m_catalog_file = TRY(create_immutable_font_data(serialized.bytes()));
    return {};
}

ErrorOr<IPC::File> FontService::create_immutable_font_data(ReadonlyBytes bytes)
{
    IPC::File file;
    {
        auto buffer = TRY(Core::AnonymousBuffer::create_with_size(bytes.size(), Core::AnonymousBuffer::Sealability::Sealable));
        bytes.copy_to({ buffer.data<u8>(), buffer.size() });
        file = TRY(IPC::File::clone_fd(buffer.fd()));
    }

#if defined(F_ADD_SEALS) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK) && defined(F_SEAL_WRITE) && defined(F_SEAL_SEAL)
    if (::fcntl(file.fd(), F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL) < 0)
        return Error::from_errno(errno);
#endif

    return file;
}

Gfx::BrokeredFont FontService::open_font(u64 generation, u64 face_id)
{
    if (wait_until_ready().is_error() || generation != m_generation || face_id == 0)
        return {};

    if (auto source = m_font_sources.get(face_id); source.has_value()) {
        auto file = Core::File::open(source->path, Core::File::OpenMode::Read);
        if (file.is_error())
            return {};
        return {
            .face_id = face_id,
            .ttc_index = source->ttc_index,
            .format = source->format,
            .file = IPC::File::adopt_file(file.release_value()),
        };
    }

    if (auto source = m_memory_font_sources.get(face_id); source.has_value()) {
        auto file = IPC::File::clone_fd(source->file.fd());
        if (file.is_error())
            return {};
        return {
            .face_id = face_id,
            .ttc_index = source->ttc_index,
            .format = source->format,
            .file = file.release_value(),
        };
    }
    return {};
}

Gfx::BrokeredFont FontService::materialize_typeface(NonnullRefPtr<Gfx::TypefaceSkia> typeface, String cache_key)
{
    if (auto face_id = m_dynamic_match_cache.get(cache_key); face_id.has_value())
        return open_font(m_generation, *face_id);

    auto file = create_immutable_font_data(typeface->font_data());
    if (file.is_error())
        return {};

    auto face_id = m_next_dynamic_face_id++;
    auto ttc_index = typeface->collection_index();
    m_memory_font_sources.set(face_id, MemoryFontSource {
                                           .file = file.release_value(),
                                           .ttc_index = ttc_index,
                                           .format = Gfx::FontFileFormat::OpenType,
                                       });
    m_dynamic_match_cache.set(move(cache_key), face_id);
    return open_font(m_generation, face_id);
}

Gfx::BrokeredFont FontService::match_font(String const& family, u16 weight, u16 width, u8 slope)
{
    if (wait_until_ready().is_error())
        return {};
    auto cache_key = MUST(String::formatted("family:{}:{}:{}:{}", family, weight, width, slope));
    if (auto face_id = m_dynamic_match_cache.get(cache_key); face_id.has_value())
        return open_font(m_generation, *face_id);

    auto typeface = Gfx::TypefaceSkia::match_family_style(family.bytes_as_string_view(), weight, width, slope);
    if (typeface.is_error() || !typeface.value())
        return {};
    return materialize_typeface(typeface.release_value().release_nonnull(), move(cache_key));
}

Gfx::BrokeredFont FontService::match_font_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    if (wait_until_ready().is_error())
        return {};
    auto cache_key = MUST(String::formatted("character:{}:{}:{}:{}:{}", code_point, weight, width, slope, prefer_color_emoji));
    if (auto face_id = m_dynamic_match_cache.get(cache_key); face_id.has_value())
        return open_font(m_generation, *face_id);

    auto typeface = Gfx::TypefaceSkia::find_typeface_for_code_point(code_point, weight, width, slope, prefer_color_emoji);
    if (typeface.is_error() || !typeface.value())
        return {};
    return materialize_typeface(typeface.release_value().release_nonnull(), move(cache_key));
}

Optional<FlyString> FontService::resolve_generic_family(String const& family, u16 weight, u8 slope)
{
    if (wait_until_ready().is_error())
        return {};
    return Gfx::TypefaceSkia::resolve_generic_family(family.bytes_as_string_view(), weight, slope);
}

}
