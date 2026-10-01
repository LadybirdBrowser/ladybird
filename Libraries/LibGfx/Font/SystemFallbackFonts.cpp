/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/Singleton.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/SystemFallbackFonts.h>

namespace Gfx {

namespace {

struct SystemFallbackFontKeyTraits : public DefaultTraits<SystemFallbackFontKey> {
    static unsigned hash(SystemFallbackFontKey const& key)
    {
        auto style = pair_int_hash(pair_int_hash(key.weight, key.width), pair_int_hash(key.slope, key.prefer_color_emoji));
        return pair_int_hash(key.code_point, style);
    }
};

struct SystemFallbackFontCache {
    AK_ALLOC_WITH_KMALLOC;

    Mutex mutex;
    // A miss is an answer too: without it every code point no family covers asks the provider again
    // on every lookup, and for the shared provider that is an IPC round trip.
    // NB: The memo holds typefaces, not fonts, so the sizes a page asks for stay bounded by the typeface's
    //     own font cache instead of each one being kept here until the font set changes.
    HashMap<SystemFallbackFontKey, RefPtr<Typeface const>, SystemFallbackFontKeyTraits> typefaces;
};

Singleton<SystemFallbackFontCache> s_system_fallback_font_cache;

SystemFallbackFontCache& system_fallback_font_cache()
{
    return *s_system_fallback_font_cache;
}

}

RefPtr<Font const> system_fallback_font(SystemFallbackFontKey const& key, float point_size)
{
    auto& cache = system_fallback_font_cache();
    // NB: The lookup runs under the lock rather than beside it, so one code point is matched once
    //     even when several threads want it. The cost falls on misses only.
    // FIXME: SharedFontProvider answers a miss with a synchronous IPC round trip on the client's
    //        connection, which belongs to the document thread. Until another thread has a font
    //        service connection of its own, only cache hits are genuinely available off it.
    MutexLocker locker(cache.mutex);
    if (auto cached = cache.typefaces.get(key); cached.has_value()) {
        if (!*cached)
            return nullptr;
        return (*cached)->font(point_size, {});
    }
    RefPtr<Font const> font = FontDatabase::the().get_font_for_code_point(
        key.code_point, point_size, key.weight, key.width, key.slope, key.prefer_color_emoji);
    cache.typefaces.set(key, font ? &font->typeface() : nullptr);
    return font;
}

void clear_system_fallback_font_cache()
{
    auto& cache = system_fallback_font_cache();
    MutexLocker locker(cache.mutex);
    cache.typefaces.clear();
}

size_t system_fallback_font_cache_size()
{
    auto& cache = system_fallback_font_cache();
    MutexLocker locker(cache.mutex);
    return cache.typefaces.size();
}

}
