/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <LibCore/MappedFile.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Font/SystemFontMatcher.h>
#include <LibGfx/Font/WOFF/Loader.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/Font/TypefaceCoreText.h>
#endif

namespace Gfx::SystemFontMatcher {

namespace {

struct FileFace {
    ByteString path;
    u32 ttc_index { 0 };

    bool operator==(FileFace const&) const = default;
};

struct FileFaceTraits : public DefaultTraits<FileFace> {
    static unsigned hash(FileFace const& face) { return pair_int_hash(face.path.hash(), face.ttc_index); }
};

struct SystemFontFileTraits : public DefaultTraits<SystemFontFile> {
    static unsigned hash(SystemFontFile const& file) { return pair_int_hash(pair_int_hash(file.path.hash(), file.ttc_index), to_underlying(file.format)); }
};

// A fallback font covers many code points, and each code point that a page asks for matches the same face again. So
// each face is checked and loaded only once.
struct MatchCaches {
    Mutex mutex;
    HashMap<FileFace, Optional<SystemFontFile>, FileFaceTraits> loadable_files;
    HashMap<SystemFontFile, NonnullRefPtr<Typeface>, SystemFontFileTraits> typefaces;
    HashMap<String, NonnullRefPtr<Typeface>> platform_typefaces;
};

MatchCaches& match_caches()
{
    static NeverDestroyed<MatchCaches> caches;
    return *caches;
}

}

static FontFileFormat format_of(ReadonlyBytes bytes)
{
    // https://www.w3.org/TR/WOFF/#WOFFHeader
    if (bytes.size() >= 4 && bytes[0] == 'w' && bytes[1] == 'O' && bytes[2] == 'F' && bytes[3] == 'F')
        return FontFileFormat::WOFF;
    return FontFileFormat::OpenType;
}

static ErrorOr<NonnullRefPtr<Typeface>> load_mapped_file(NonnullOwnPtr<Core::MappedFile> mapped_file, u32 ttc_index, FontFileFormat format)
{
    if (format == FontFileFormat::WOFF)
        return WOFF::try_load_from_bytes(mapped_file->bytes(), ttc_index);
    return Typeface::try_load_from_mapped_file(move(mapped_file), ttc_index);
}

static Optional<SystemFontFile> find_loadable_file(StringView path, u32 ttc_index)
{
    auto canonical_path = FileSystem::real_path(path);
    if (canonical_path.is_error())
        return {};
    auto mapped_file = Core::MappedFile::map(canonical_path.value());
    if (mapped_file.is_error())
        return {};
    auto format = format_of(mapped_file.value()->bytes());
    auto typeface = load_mapped_file(mapped_file.release_value(), ttc_index, format);
    if (typeface.is_error())
        return {};
#ifdef AK_OS_MACOS
    // The compositor draws the faces of installed fonts with CoreText, which does not load every face that HarfBuzz
    // reads, such as one with hvgl outlines.
    if (!core_text_accepts_font_data(*typeface.value()->font_data_backing(), ttc_index))
        return {};
#endif
    auto string_path = String::from_byte_string(canonical_path.release_value());
    if (string_path.is_error())
        return {};
    return SystemFontFile { .path = string_path.release_value(), .ttc_index = ttc_index, .format = format };
}

Optional<SystemFontFile> loadable_file(StringView path, u32 ttc_index)
{
    auto& caches = match_caches();
    FileFace face { path, ttc_index };
    {
        MutexLocker locker { caches.mutex };
        if (auto it = caches.loadable_files.find(face); it != caches.loadable_files.end())
            return it->value;
    }
    auto file = find_loadable_file(path, ttc_index);
    MutexLocker locker { caches.mutex };
    caches.loadable_files.set(move(face), file);
    return file;
}

ErrorOr<NonnullRefPtr<Typeface>> load(SystemFontMatch const& match)
{
    auto& caches = match_caches();
    return match.visit(
        [&](SystemFontFile const& file) -> ErrorOr<NonnullRefPtr<Typeface>> {
            {
                MutexLocker locker { caches.mutex };
                if (auto it = caches.typefaces.find(file); it != caches.typefaces.end())
                    return it->value;
            }
            auto typeface = TRY(load_mapped_file(TRY(Core::MappedFile::map(file.path)), file.ttc_index, file.format));
            if (file.format == FontFileFormat::OpenType)
                typeface->set_file_path(file.path);
            MutexLocker locker { caches.mutex };
            return caches.typefaces.ensure(file, [&] { return move(typeface); });
        },
        [&](PlatformFontName const& name) -> ErrorOr<NonnullRefPtr<Typeface>> {
#ifdef AK_OS_MACOS
            {
                MutexLocker locker { caches.mutex };
                if (auto it = caches.platform_typefaces.find(name.postscript_name); it != caches.platform_typefaces.end())
                    return it->value;
            }
            NonnullRefPtr<Typeface> typeface = TRY(TypefaceCoreText::try_load_postscript_name(name.postscript_name));
            MutexLocker locker { caches.mutex };
            return caches.platform_typefaces.ensure(name.postscript_name, [&] { return move(typeface); });
#else
            (void)name;
            return Error::from_string_literal("Only CoreText opens fonts by name");
#endif
        });
}

}
