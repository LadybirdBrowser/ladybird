/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <LibGfx/Font/FontCatalog.h>
#include <LibGfx/Font/SharedFontProvider.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibThreading/Thread.h>
#include <LibWebView/Export.h>

namespace WebView {

struct FontCatalogDescriptor {
    IPC::File file;
    u64 size { 0 };
    u64 generation { 0 };
};

class WEBVIEW_API FontService {
    AK_MAKE_NONCOPYABLE(FontService);
    AK_MAKE_NONMOVABLE(FontService);

public:
    static NonnullOwnPtr<FontService> create();
    ~FontService();

    ErrorOr<FontCatalogDescriptor> clone_catalog();
    Gfx::BrokeredFont open_font(u64 generation, u64 face_id);
    Gfx::BrokeredFont match_font(String const& family, u16 weight, u16 width, u8 slope);
    Gfx::BrokeredFont match_font_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji);
    Optional<FlyString> resolve_generic_family(String const& family, u16 weight, u8 slope);

private:
    FontService();

    struct FontSource {
        String path;
        u32 ttc_index { 0 };
        Gfx::FontFileFormat format { Gfx::FontFileFormat::OpenType };
    };

    struct MemoryFontSource {
        IPC::File file;
        u32 ttc_index { 0 };
        Gfx::FontFileFormat format { Gfx::FontFileFormat::OpenType };
    };

    ErrorOr<void> build_catalog();
    ErrorOr<void> build_empty_catalog();
    ErrorOr<void> wait_until_ready();
    ErrorOr<IPC::File> create_immutable_font_data(ReadonlyBytes);
    Gfx::BrokeredFont materialize_typeface(NonnullRefPtr<Gfx::TypefaceSkia>, String cache_key);

    NonnullRefPtr<Threading::Thread> m_worker;
    Optional<String> m_build_error;
    IPC::File m_catalog_file;
    u64 m_catalog_size { 0 };
    u64 m_generation { 1 };
    u64 m_next_dynamic_face_id { 1ull << 63 };
    HashMap<u64, FontSource> m_font_sources;
    HashMap<u64, MemoryFontSource> m_memory_font_sources;
    HashMap<String, u64> m_dynamic_match_cache;
};

}
