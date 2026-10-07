/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/Diagnostics.h>
#include <AK/LsanSuppressions.h>
#include <AK/Math.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

#include <core/SkData.h>
#include <core/SkFontArguments.h>
#include <core/SkFontMgr.h>
#include <core/SkStream.h>
#include <core/SkString.h>
#include <core/SkTypeface.h>
#include <harfbuzz/hb-ot.h>
#include <harfbuzz/hb.h>
#include <ports/SkFontScanner_FreeType.h>
#if defined(AK_OS_ANDROID)
#    include <ports/SkFontMgr_android.h>
#elif defined(AK_OS_WINDOWS)
#    include <ports/SkFontMgr_empty.h>
#    include <ports/SkTypeface_win.h>
#    include <src/ports/SkTypeface_win_dw.h>
#else
#    include <ports/SkFontMgr_fontconfig.h>
#endif

#ifdef AK_OS_MACOS
#    include <CoreText/CoreText.h>
#    include <LibGfx/Font/TypefaceCoreText.h>
#    include <ports/SkFontMgr_mac_ct.h>
#    include <ports/SkTypeface_mac.h>
#    include <src/ports/SkTypeface_mac_ct.h>
#endif

namespace Gfx {

struct TypefaceSkia::Impl {
    AK_ALLOC_WITH_KMALLOC;

    explicit Impl(sk_sp<SkTypeface> skia_typeface)
        : skia_typeface(move(skia_typeface))
    {
    }

    sk_sp<SkTypeface> skia_typeface;
};

// Text is shaped with HarfBuzz, so font data is of no use if HarfBuzz cannot read the face in it, even if Skia can
// draw it. FreeType also reads font formats such as WOFF, which have to be decoded first.
static bool harfbuzz_can_read_face(ReadonlyBytes buffer, u32 ttc_index)
{
    if (buffer.size() > NumericLimits<unsigned>::max())
        return false;
    auto* blob = hb_blob_create(reinterpret_cast<char const*>(buffer.data()), buffer.size(), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    ScopeGuard destroy_blob = [&] { hb_blob_destroy(blob); };
    // The upper 16 bits of the index select a named instance of a variable face.
    return (ttc_index & 0xFFFF) < hb_face_count(blob);
}

ErrorOr<NonnullRefPtr<TypefaceSkia>> TypefaceSkia::load_from_buffer(AK::ReadonlyBytes buffer, u32 ttc_index, NonnullRefPtr<FontDataBacking> backing)
{
    if (!harfbuzz_can_read_face(buffer, ttc_index))
        return Error::from_string_literal("Font data has no face that HarfBuzz can read");

    // NB: Skia can retain the typeface in text blobs and glyph caches after our Typeface is destroyed.
    //     Keep the backing alive through SkData.
    backing->ref();

    sk_sp<SkData> data = SkData::MakeWithProc(buffer.data(), buffer.size(), [](void const*, void* context) { static_cast<FontDataBacking*>(context)->unref(); }, backing.ptr());

    sk_sp<SkTypeface> skia_typeface;
#ifdef AK_OS_MACOS
    // NB: Skia's CoreText stream loader only supports collection index zero.
    if (ttc_index != 0 && !FontDatabase::the().force_freetype_rasterization()) {
        auto ct_font = create_core_text_font_from_data(backing, buffer, ttc_index);
        if (!ct_font)
            return Error::from_string_literal("CoreText did not load the collection face");
        ScopeGuard release_font = [&] { CFRelease(ct_font); };
        skia_typeface = SkMakeTypefaceFromCTFont(ct_font);
    } else
#endif
    {
        // https://learn.microsoft.com/en-us/typography/opentype/spec/otff#ttc-header
        // TrueType Collection files bundle multiple fonts (often different weights of the same
        // family). We use SkFontArguments to specify which font to load from the collection.
        SkFontArguments font_args;
        font_args.setCollectionIndex(static_cast<int>(ttc_index));

        auto stream = std::make_unique<SkMemoryStream>(data);
        // Loading supplied font data does not need a manager that discovers system fonts.
#if defined(AK_OS_WINDOWS)
        if (!FontDatabase::the().force_freetype_rasterization()) {
            skia_typeface = DWriteFontTypeface::MakeFromStream(std::move(stream), font_args);
        } else
#elif defined(AK_OS_MACOS)
        if (!FontDatabase::the().force_freetype_rasterization()) {
            skia_typeface = SkTypeface_Mac::MakeFromStream(std::move(stream), font_args);
        } else
#endif
        {
            skia_typeface = SkFontScanner_Make_FreeType()->MakeFromStream(std::move(stream), font_args);
        }
    }

    if (!skia_typeface) {
        return Error::from_string_literal("Failed to load typeface from buffer");
    }

    auto typeface = adopt_ref(*new TypefaceSkia { make<TypefaceSkia::Impl>(skia_typeface), buffer, ttc_index });
    typeface->set_font_data(move(backing));
    return typeface;
}

SkTypeface const* TypefaceSkia::sk_typeface() const
{
    return impl().skia_typeface.get();
}

TypefaceSkia::TypefaceSkia(NonnullOwnPtr<Impl> impl, ReadonlyBytes buffer, u32 ttc_index)
    : m_impl(move(impl))
    , m_buffer(buffer)
    , m_ttc_index(ttc_index)
{
}

}
