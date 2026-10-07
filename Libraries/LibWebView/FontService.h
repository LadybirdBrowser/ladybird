/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <LibGfx/Font/FontCatalog.h>
#include <LibGfx/Font/SharedFontProvider.h>
#include <LibGfx/Font/SystemFontMatcher.h>
#include <LibThreading/Thread.h>
#include <LibWebView/Export.h>

namespace WebView {

struct FontCatalogDescriptor {
    IPC::File file;
    u64 size { 0 };
    u64 generation { 0 };
};

// Held by the UI process and by each font connection thread that answers from it, so the service
// outlives every thread that may still be answering a question when the UI shuts down.
class WEBVIEW_API FontService : public AtomicRefCounted<FontService> {
    AK_MAKE_NONCOPYABLE(FontService);
    AK_MAKE_NONMOVABLE(FontService);

public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullRefPtr<FontService> create(Vector<String> additional_font_directories = {});
    ~FontService();

    ErrorOr<FontCatalogDescriptor> clone_catalog();
    Gfx::BrokeredFont open_font(u64 generation, u64 face_id);
    Gfx::BrokeredFont match_local_font(String const& name);
    Gfx::BrokeredFont match_font(String const& family, u16 weight, u16 width, u8 slope);
    Gfx::BrokeredFont match_font_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji);
    Optional<FlyString> resolve_generic_family(String const& family, u16 weight, u8 slope);

private:
    friend struct FontServiceTestAccess;

    explicit FontService(Vector<String> additional_font_directories);

    struct FontSource {
        String path;
        u32 ttc_index { 0 };
        Gfx::FontFileFormat format { Gfx::FontFileFormat::OpenType };

        bool operator==(FontSource const&) const = default;
    };

    struct FontSourceTraits : public DefaultTraits<FontSource> {
        static unsigned hash(FontSource const& source)
        {
            return pair_int_hash(pair_int_hash(source.path.hash(), source.ttc_index), to_underlying(source.format));
        }
    };

    using MemoryFontSource = Variant<Gfx::BrokeredFontFile, Gfx::PlatformFontName>;

    ErrorOr<void> build_catalog();
    ErrorOr<void> build_empty_catalog();
    ErrorOr<void> wait_until_ready();
    ErrorOr<IPC::File> create_immutable_font_data(ReadonlyBytes);
    Gfx::BrokeredFont materialize(Gfx::SystemFontMatch, String cache_key);
    Gfx::BrokeredFont materialize_file(Gfx::SystemFontFile, String cache_key);
    Gfx::BrokeredFont materialize_platform_font(Gfx::PlatformFontName, String cache_key);
    Gfx::BrokeredFont materialize_typeface(NonnullRefPtr<Gfx::Typeface>, String cache_key);
    Gfx::BrokeredFont open_font_without_lock(u64 generation, u64 face_id);

    Vector<String> m_additional_font_directories;
    HashMap<String, u64> m_local_font_names;
    NonnullRefPtr<Threading::Thread> m_worker;
    Optional<String> m_build_error;
    IPC::File m_catalog_file;
    u64 m_catalog_size { 0 };
    u64 m_generation { 1 };
    u64 m_next_dynamic_face_id { 1ull << 63 };
    HashMap<u64, FontSource> m_font_sources;
    HashMap<FontSource, u64, FontSourceTraits> m_face_id_by_source;
    HashMap<String, u64> m_face_id_by_platform_font_name;
    HashMap<u64, MemoryFontSource> m_memory_font_sources;
    HashMap<String, u64> m_dynamic_match_cache;

    // Font requests arrive on the UI process's renderer connections and on the dedicated font connections of the
    // Compositor and of each renderer's render side.
    Mutex m_mutex;
};

}
