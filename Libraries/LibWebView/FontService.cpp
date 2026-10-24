/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibWebView/FontService.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/Font/TypefaceCoreText.h>
#endif

#include <fcntl.h>

#if !defined(AK_OS_WINDOWS)
#    include <sys/mman.h>
#    include <unistd.h>
#endif

namespace WebView {

NonnullRefPtr<FontService> FontService::create(Vector<String> additional_font_directories)
{
    return adopt_ref(*new FontService(move(additional_font_directories)));
}

FontService::FontService(Vector<String> additional_font_directories)
    : m_additional_font_directories(move(additional_font_directories))
    , m_worker(Threading::Thread::construct("Font catalog"sv, [this] {
        if (auto result = build_catalog(); result.is_error()) {
            dbgln("Unable to discover system fonts: {}. Using an empty catalog.", result.error());
            m_font_sources.clear();
            m_face_id_by_source.clear();
            m_local_font_names.clear();
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
    MutexLocker locker(m_mutex);
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
    directories.extend(m_additional_font_directories);
    for (auto const& directory : directories) {
        auto uri = TRY(String::formatted("file://{}", directory));
        auto collect_typeface = [&](String const& path, u32 ttc_index, Gfx::FontFileFormat format, NonnullRefPtr<Gfx::Typeface> typeface) {
            if (callback_error.has_value())
                return;
            // A face without a family name cannot be matched.
            if (typeface->family().is_empty())
                return;
#ifdef AK_OS_MACOS
            // The compositor draws the faces of installed fonts with CoreText, which does not load every face that
            // HarfBuzz reads, such as one with hvgl outlines.
            if (!Gfx::core_text_accepts_font_data(*typeface->font_data_backing(), ttc_index))
                return;
#endif
            auto face_id = next_face_id++;
            auto names = typeface->local_font_names();
            if (names.is_error()) {
                callback_error = names.release_error();
                return;
            }
            for (auto const& name : names.value()) {
                auto folded_name = name.to_casefold();
                if (folded_name.is_error()) {
                    callback_error = folded_name.release_error();
                    return;
                }
                m_local_font_names.set(folded_name.release_value(), face_id, AK::HashSetExistingEntryBehavior::Keep);
            }
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
            FontSource source { .path = path, .ttc_index = ttc_index, .format = format };
            m_face_id_by_source.set(source, face_id);
            m_font_sources.set(face_id, move(source));
        };
        Gfx::PathFontProvider::for_each_typeface_in_uri(uri, loaded_paths, move(collect_typeface));
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
#if defined(F_ADD_SEALS) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK) && defined(F_SEAL_WRITE) && defined(F_SEAL_SEAL)
    // Written through the descriptor: the write seal fails with EBUSY while any process maps the memory writable, which
    // a helper forked by the main thread would do until it execs.
    auto file = IPC::File::adopt_fd(TRY(Core::System::anon_create(bytes.size(), O_CLOEXEC, Core::System::AllowSealing::Yes)));
    while (!bytes.is_empty())
        bytes = bytes.slice(TRY(Core::System::write(file.fd(), bytes)));
    if (::fcntl(file.fd(), F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL) < 0)
        return Error::from_errno(errno);
    return file;
#elif defined(AK_OS_WINDOWS)
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(bytes.size()));
    bytes.copy_to({ buffer.data<u8>(), buffer.size() });
    return IPC::File::clone_fd(buffer.fd());
#else
    // Without file seals, hand out a descriptor that was opened read-only, so that a helper cannot map the data writable
    // and change it for every other process that uses it. Fill the object through a descriptor that nobody else sees.
    auto name = ByteString::formatted("/shm-{:016x}-font", get_random<u64>());
    auto writable_fd = shm_open(name.characters(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (writable_fd < 0)
        return Error::from_syscall("shm_open"sv, errno);
    ScopeGuard unlink_and_close_writable_fd = [&] {
        shm_unlink(name.characters());
        close(writable_fd);
    };
    if (::ftruncate(writable_fd, static_cast<off_t>(max(bytes.size(), 1uz))) < 0)
        return Error::from_syscall("ftruncate"sv, errno);
    if (!bytes.is_empty()) {
        auto* mapping = ::mmap(nullptr, bytes.size(), PROT_READ | PROT_WRITE, MAP_SHARED, writable_fd, 0);
        if (mapping == MAP_FAILED)
            return Error::from_syscall("mmap"sv, errno);
        bytes.copy_to({ static_cast<u8*>(mapping), bytes.size() });
        ::munmap(mapping, bytes.size());
    }
    auto read_only_fd = shm_open(name.characters(), O_RDONLY, 0);
    if (read_only_fd < 0)
        return Error::from_syscall("shm_open"sv, errno);
    if (::fcntl(read_only_fd, F_SETFD, FD_CLOEXEC) < 0) {
        auto saved_errno = errno;
        close(read_only_fd);
        return Error::from_syscall("fcntl"sv, saved_errno);
    }
    return IPC::File::adopt_fd(read_only_fd);
#endif
}

Gfx::BrokeredFont FontService::open_font(u64 generation, u64 face_id)
{
    MutexLocker locker(m_mutex);
    if (wait_until_ready().is_error())
        return {};
    return open_font_without_lock(generation, face_id);
}

Gfx::BrokeredFont FontService::open_font_without_lock(u64 generation, u64 face_id)
{
    if (generation != m_generation || face_id == 0)
        return {};

    if (auto source = m_font_sources.get(face_id); source.has_value()) {
        auto file = Core::File::open(source->path, Core::File::OpenMode::Read);
        if (file.is_error())
            return {};
        return {
            .face_id = face_id,
            .source = Gfx::BrokeredFontFile {
                .ttc_index = source->ttc_index,
                .format = source->format,
                .file = IPC::File::adopt_file(file.release_value()),
            },
        };
    }

    if (auto name = m_platform_font_names.get(face_id); name.has_value())
        return { .face_id = face_id, .source = *name };
    return {};
}

Gfx::BrokeredFont FontService::materialize(Gfx::SystemFontMatch match, String cache_key)
{
    return match.visit(
        [&](Gfx::SystemFontFile& file) { return materialize_file(move(file), move(cache_key)); },
        [&](Gfx::PlatformFontName& name) { return materialize_platform_font(move(name), move(cache_key)); });
}

Gfx::BrokeredFont FontService::materialize_file(Gfx::SystemFontFile file, String cache_key)
{
    // Every code point that a fallback font covers matches it under a key of its own, but the face is one face for all
    // of them. A face of the catalog keeps its catalog id.
    FontSource source { .path = move(file.path), .ttc_index = file.ttc_index, .format = file.format };
    auto face_id = m_face_id_by_source.ensure(source, [&] {
        auto new_face_id = m_next_dynamic_face_id++;
        m_font_sources.set(new_face_id, source);
        return new_face_id;
    });
    m_dynamic_match_cache.set(move(cache_key), face_id);
    return open_font_without_lock(m_generation, face_id);
}

Gfx::BrokeredFont FontService::materialize_platform_font(Gfx::PlatformFontName name, String cache_key)
{
    auto face_id = m_face_id_by_platform_font_name.ensure(name.postscript_name, [&] {
        auto new_face_id = m_next_dynamic_face_id++;
        m_platform_font_names.set(new_face_id, name);
        return new_face_id;
    });
    m_dynamic_match_cache.set(move(cache_key), face_id);
    return open_font_without_lock(m_generation, face_id);
}

Gfx::BrokeredFont FontService::match_local_font(String const& name)
{
    MutexLocker locker(m_mutex);
    if (wait_until_ready().is_error())
        return {};
    auto folded_name = name.to_casefold();
    if (folded_name.is_error())
        return {};
    auto face_id = m_local_font_names.get(folded_name.value());
    if (!face_id.has_value())
        return {};
    // https://drafts.csswg.org/css-fonts-4/#local-font-fallback
    // Platform substitutions for a given font name must not be used.
    return open_font_without_lock(m_generation, *face_id);
}

Gfx::BrokeredFont FontService::match_font(String const& family, u16 weight, u16 width, u8 slope)
{
    MutexLocker locker(m_mutex);
    if (wait_until_ready().is_error())
        return {};
    auto cache_key = MUST(String::formatted("family:{}:{}:{}:{}", family, weight, width, slope));
    if (auto face_id = m_dynamic_match_cache.get(cache_key); face_id.has_value())
        return open_font_without_lock(m_generation, *face_id);

    auto match = Gfx::SystemFontMatcher::match_family_style(family, weight, width, slope);
    if (!match.has_value())
        return {};
    return materialize(match.release_value(), move(cache_key));
}

Gfx::BrokeredFont FontService::match_font_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    MutexLocker locker(m_mutex);
    if (wait_until_ready().is_error())
        return {};
    auto cache_key = MUST(String::formatted("character:{}:{}:{}:{}:{}", code_point, weight, width, slope, prefer_color_emoji));
    if (auto face_id = m_dynamic_match_cache.get(cache_key); face_id.has_value())
        return open_font_without_lock(m_generation, *face_id);

    auto match = Gfx::SystemFontMatcher::match_code_point(code_point, weight, width, slope, prefer_color_emoji);
    if (!match.has_value())
        return {};
    return materialize(match.release_value(), move(cache_key));
}

Optional<FlyString> FontService::resolve_generic_family(String const& family, u16 weight, u8 slope)
{
    MutexLocker locker(m_mutex);
    if (wait_until_ready().is_error())
        return {};
    return Gfx::SystemFontMatcher::resolve_generic_family(family, weight, slope);
}

}
