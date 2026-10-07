/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashFunctions.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Mutex.h>
#include <AK/OwnPtr.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/Font/FontCatalog.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/Font/SystemFontMatcher.h>
#include <LibIPC/File.h>

namespace Gfx {

struct BrokeredFontFile {
    u32 ttc_index { 0 };
    FontFileFormat format { FontFileFormat::OpenType };
    IPC::File file;
};

struct BrokeredFont {
    u64 face_id { 0 };
    Variant<Empty, BrokeredFontFile, PlatformFontName> source;
};

// Any thread may ask these. The provider asks one question at a time.
struct SharedFontProviderCallbacks {
    Function<BrokeredFont(u64 generation, u64 face_id)> open_font;
    Function<BrokeredFont(String const& name)> match_local_font;
    Function<BrokeredFont(String const& family, u16 weight, u16 width, u8 slope)> match_font;
    Function<BrokeredFont(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)> match_font_for_code_point;
    Function<Optional<FlyString>(String const& family, u16 weight, u8 slope)> resolve_generic_family;
};

class SharedFontProvider final : public SystemFontProvider {
    AK_MAKE_NONCOPYABLE(SharedFontProvider);
    AK_MAKE_NONMOVABLE(SharedFontProvider);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<SharedFontProvider>> create(NonnullOwnPtr<Core::MappedFile>, u64 generation, SharedFontProviderCallbacks&&);
    static ErrorOr<NonnullOwnPtr<SharedFontProvider>> create_from_catalog_file_or_empty(IPC::File, u64 size, u64 generation, SharedFontProviderCallbacks&&);
    static ErrorOr<NonnullOwnPtr<SharedFontProvider>> create_empty(u64 generation, SharedFontProviderCallbacks&&);
    virtual ~SharedFontProvider() override;

    virtual RefPtr<Gfx::Font> get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope, Optional<FontVariationSettings> const& = {}, Optional<Gfx::ShapeFeatures> const& = {}) override;
    virtual void for_each_typeface_with_family_name(FlyString const&, Function<void(Typeface const&)>) override;
    virtual RefPtr<Typeface> get_typeface_by_id(u64 generation, u64 face_id) override;
    virtual RefPtr<Typeface> get_typeface_by_local_name(String const&) override;
    virtual RefPtr<Gfx::Font> get_font_for_code_point(u32 code_point, float point_size, u16 weight, u16 width, u8 slope, bool prefer_color_emoji) override;
    virtual Optional<FlyString> resolve_generic_family(StringView family_name, u16 weight, u8 slope) override;

private:
    struct CodePointCacheKey {
        u32 code_point { 0 };
        u16 weight { 0 };
        u16 width { 0 };
        u8 slope { 0 };
        bool prefer_color_emoji { false };

        bool operator==(CodePointCacheKey const&) const = default;
    };

    struct CodePointCacheKeyTraits : public DefaultTraits<CodePointCacheKey> {
        static unsigned hash(CodePointCacheKey const& key)
        {
            auto style_hash = pair_int_hash(pair_int_hash(key.weight, key.width), pair_int_hash(key.slope, key.prefer_color_emoji));
            return pair_int_hash(key.code_point, style_hash);
        }
    };

    SharedFontProvider(NonnullOwnPtr<Core::MappedFile>, NonnullOwnPtr<FontCatalog>, SharedFontProviderCallbacks);

    // NB: These run with m_mutex held.
    RefPtr<Typeface> load_catalog_face(FontCatalogFace const&);
    RefPtr<Typeface> load_brokered_font(BrokeredFont);
    RefPtr<Typeface> load_font_file(u64 face_id, u32 ttc_index, FontFileFormat, IPC::File);
    RefPtr<Typeface> load_platform_font(u64 face_id, PlatformFontName const&);

    NonnullOwnPtr<Core::MappedFile> const m_catalog_mapping;
    NonnullOwnPtr<FontCatalog> const m_catalog;
    SharedFontProviderCallbacks const m_callbacks;

    // Guards everything below. A font cascade can be resolved off the document thread, and every resolution fills
    // these caches on its way past. The lock is held across a round trip to the font service, so that one face is
    // opened once however many threads want it.
    Mutex m_mutex;
    PathFontProvider m_resource_fonts;
    HashMap<u64, NonnullRefPtr<Typeface>> m_typeface_cache;
    HashTable<u64> m_failed_face_ids;
    HashMap<CodePointCacheKey, RefPtr<Typeface>, CodePointCacheKeyTraits> m_code_point_cache;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::BrokeredFontFile const&);

template<>
ErrorOr<Gfx::BrokeredFontFile> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::PlatformFontName const&);

template<>
ErrorOr<Gfx::PlatformFontName> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::BrokeredFont const&);

template<>
ErrorOr<Gfx::BrokeredFont> decode(Decoder&);

}
