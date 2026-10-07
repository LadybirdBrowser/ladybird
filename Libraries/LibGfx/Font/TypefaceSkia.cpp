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
#    include <ports/SkFontMgr_mac_ct.h>
#    include <ports/SkTypeface_mac.h>
#    include <src/ports/SkTypeface_mac_ct.h>
#endif

namespace Gfx {

static auto& skia_font_manager()
{
    static NeverDestroyed<sk_sp<SkFontMgr>> font_manager;
    return *font_manager;
}

struct TypefaceSkia::Impl {
    AK_ALLOC_WITH_KMALLOC;

    Impl(sk_sp<SkTypeface> skia_typeface, std::unique_ptr<SkStreamAsset> stream = {})
        : skia_typeface(move(skia_typeface))
        , stream(move(stream))
    {
    }

    sk_sp<SkTypeface> skia_typeface;
    std::unique_ptr<SkStreamAsset> stream;
};

static SkFontMgr& font_manager()
{
    auto& font_manager = skia_font_manager();
    if (!font_manager) {
#ifdef AK_OS_MACOS
        if (!Gfx::FontDatabase::the().force_freetype_rasterization()) {
            font_manager = SkFontMgr_New_CoreText(nullptr);
        }
#endif
#if defined(AK_OS_ANDROID)
        font_manager = SkFontMgr_New_Android(nullptr);
#elif defined(AK_OS_WINDOWS)
        if (Gfx::FontDatabase::the().force_freetype_rasterization())
            font_manager = SkFontMgr_New_Custom_Empty();
        else
            font_manager = SkFontMgr_New_DirectWrite();
#else
        if (!font_manager) {
            font_manager = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
        }
#endif
    }
    VERIFY(font_manager);
    return *font_manager;
}

static std::unique_ptr<SkMemoryStream> copy_stream_to_memory_stream(SkStreamAsset& stream)
{
    auto stream_copy = stream.duplicate();
    VERIFY(stream_copy);

    auto data = SkData::MakeFromStream(stream_copy.get(), stream_copy->getLength());
    VERIFY(data);
    VERIFY(data->size() == stream.getLength());

    return std::make_unique<SkMemoryStream>(move(data));
}

static SkFontStyle::Slant slope_to_skia_slant(u8 slope)
{
    switch (slope) {
    case 1:
        return SkFontStyle::kItalic_Slant;
    case 2:
        return SkFontStyle::kOblique_Slant;
    default:
        return SkFontStyle::kUpright_Slant;
    }
}

ErrorOr<RefPtr<TypefaceSkia>> TypefaceSkia::typeface_from_skia_typeface(sk_sp<SkTypeface> skia_typeface)
{
    if (!skia_typeface)
        return RefPtr<TypefaceSkia> {};

    int skia_ttc_index = 0;
    auto stream = skia_typeface->openStream(&skia_ttc_index);
    auto ttc_index = static_cast<u32>(skia_ttc_index);

    if (stream && stream->getMemoryBase()) {
        // NB: Safe to reference without copying because we hold on to the stream.
        ReadonlyBytes bytes { static_cast<u8 const*>(stream->getMemoryBase()), stream->getLength() };
        return adopt_ref(*new TypefaceSkia {
            make<TypefaceSkia::Impl>(skia_typeface, std::move(stream)),
            bytes,
            ttc_index });
    }

    if (!stream)
        return Error::from_string_literal("Failed to get font data from typeface");

    auto memory_stream = copy_stream_to_memory_stream(*stream);
    auto bytes = ReadonlyBytes { static_cast<u8 const*>(memory_stream->getMemoryBase()), memory_stream->getLength() };
    return adopt_ref(*new TypefaceSkia {
        make<TypefaceSkia::Impl>(skia_typeface, move(memory_stream)),
        bytes,
        ttc_index });
}

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
    // NB: Skia's CoreText stream loader only supports collection index zero. Ask CoreText for the collection's
    //     descriptors directly, then wrap the selected face in Skia, as we do for system UI fonts.
    if (ttc_index != 0 && !FontDatabase::the().force_freetype_rasterization()) {
        if (buffer.size() > static_cast<size_t>(NumericLimits<CFIndex>::max()))
            return Error::from_string_literal("Font data is too large for CoreText");
        auto* blob = hb_blob_create(reinterpret_cast<char const*>(buffer.data()), buffer.size(), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        ScopeGuard destroy_blob = [&] { hb_blob_destroy(blob); };
        auto* face = hb_face_create(blob, ttc_index);
        ScopeGuard destroy_face = [&] { hb_face_destroy(face); };
        unsigned entry_count = 0;
        auto const* entries = hb_ot_name_list_names(face, &entry_count);
        auto language = HB_LANGUAGE_INVALID;
        for (unsigned index = 0; index < entry_count; ++index) {
            if (entries[index].name_id != HB_OT_NAME_ID_POSTSCRIPT_NAME)
                continue;
            if (language == HB_LANGUAGE_INVALID)
                language = entries[index].language;
            if (entries[index].language == hb_language_from_string("en", -1)) {
                language = entries[index].language;
                break;
            }
        }
        unsigned name_length = hb_ot_name_get_utf8(face, HB_OT_NAME_ID_POSTSCRIPT_NAME, language, nullptr, nullptr);
        if (name_length == 0 || name_length == NumericLimits<unsigned>::max())
            return Error::from_string_literal("Font collection face has no PostScript name");
        auto name = TRY(ByteBuffer::create_uninitialized(static_cast<size_t>(name_length) + 1));
        unsigned capacity = name_length + 1;
        hb_ot_name_get_utf8(face, HB_OT_NAME_ID_POSTSCRIPT_NAME, language, &capacity, reinterpret_cast<char*>(name.data()));
        auto postscript_name = CFStringCreateWithBytes(kCFAllocatorDefault, name.data(), capacity, kCFStringEncodingUTF8, false);
        if (!postscript_name)
            return Error::from_string_literal("Invalid font PostScript name");
        ScopeGuard release_name = [&] { CFRelease(postscript_name); };
        // NB: CoreText must share the same bytes as Skia, retaining their backing for as long as it uses them.
        CFAllocatorContext allocator_context {};
        allocator_context.info = data.get();
        allocator_context.retain = [](void const* info) -> void const* {
            static_cast<SkData const*>(info)->ref();
            return info;
        };
        allocator_context.release = [](void const* info) { static_cast<SkData const*>(info)->unref(); };
        allocator_context.deallocate = [](void*, void*) { };
        auto allocator = CFAllocatorCreate(kCFAllocatorDefault, &allocator_context);
        if (!allocator)
            return Error::from_string_literal("Failed to create CoreText font data allocator");
        ScopeGuard release_allocator = [&] { CFRelease(allocator); };
        auto cf_data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, static_cast<u8 const*>(data->data()), static_cast<CFIndex>(data->size()), allocator);
        if (!cf_data)
            return Error::from_string_literal("Failed to create CoreText font data");
        ScopeGuard release_data = [&] { CFRelease(cf_data); };
        auto descriptors = CTFontManagerCreateFontDescriptorsFromData(cf_data);
        if (!descriptors)
            return Error::from_string_literal("Failed to read CoreText font descriptors");
        ScopeGuard release_descriptors = [&] { CFRelease(descriptors); };
        // NB: CoreText also lists named variation instances. Match the actual TTC face's PostScript name rather
        //     than interpreting a descriptor array position as a collection index.
        CTFontDescriptorRef descriptor = nullptr;
        for (CFIndex index = 0; index < CFArrayGetCount(descriptors); ++index) {
            auto candidate = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(descriptors, index));
            auto candidate_name = CTFontDescriptorCopyAttribute(candidate, kCTFontNameAttribute);
            if (!candidate_name)
                continue;
            ScopeGuard release_candidate_name = [&] { CFRelease(candidate_name); };
            if (CFEqual(candidate_name, postscript_name)) {
                descriptor = candidate;
                break;
            }
        }
        if (!descriptor)
            return Error::from_string_literal("CoreText did not describe the requested collection face");
        auto ct_font = CTFontCreateWithFontDescriptor(descriptor, 0, nullptr);
        if (!ct_font)
            return Error::from_string_literal("Failed to create CoreText font");
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

void TypefaceSkia::encode_font_data_for_ipc(IPC::Encoder& encoder) const
{
    if (has_font_data_backing()) {
        Typeface::encode_font_data_for_ipc(encoder);
        return;
    }

    auto family_name = family().to_string();

    MUST(encoder.encode(FontDataFormat::SystemFont));
    MUST(encoder.encode(family_name));
    MUST(encoder.encode(weight()));
    MUST(encoder.encode(width()));
    MUST(encoder.encode(slope()));
}

ErrorOr<RefPtr<TypefaceSkia>> TypefaceSkia::match_family_style(StringView family_name, u16 weight, u16 width, u8 slope)
{
    auto skia_typeface = font_manager().matchFamilyStyle(ByteString(family_name).characters(), SkFontStyle { weight, width, slope_to_skia_slant(slope) });
    return typeface_from_skia_typeface(move(skia_typeface));
}

ErrorOr<RefPtr<TypefaceSkia>> TypefaceSkia::find_typeface_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    SkFontStyle style(weight, width, slope_to_skia_slant(slope));

    // The "und-Zsye" language tag steers the font matcher towards a color emoji font. Without it, a text-presentation
    // font is preferred for emoji-capable code points.
    char const* emoji_locale[] = { "und-Zsye" };
    auto skia_typeface = font_manager().matchFamilyStyleCharacter(
        nullptr, style, prefer_color_emoji ? emoji_locale : nullptr, prefer_color_emoji ? 1 : 0, code_point);

    if (!skia_typeface)
        return RefPtr<TypefaceSkia> {};

    return typeface_from_skia_typeface(move(skia_typeface));
}

Optional<FlyString> TypefaceSkia::resolve_generic_family(StringView family_name, u16 weight, u8 slope)
{
    SkFontStyle style(weight, SkFontStyle::kNormal_Width, slope_to_skia_slant(slope));
    auto skia_typeface = font_manager().matchFamilyStyle(
        ByteString(family_name).characters(), style);

    if (!skia_typeface)
        return {};

    SkString resolved_family;
    skia_typeface->getFamilyName(&resolved_family);
    auto result_or_error = FlyString::from_utf8(StringView { resolved_family.c_str(), resolved_family.size() });
    if (result_or_error.is_error())
        return {};
    return result_or_error.release_value();
}

SkTypeface const* TypefaceSkia::sk_typeface() const
{
    return impl().skia_typeface.get();
}

u32 TypefaceSkia::platform_typeface_id() const
{
    return impl().skia_typeface->uniqueID();
}

TypefaceSkia::TypefaceSkia(NonnullOwnPtr<Impl> impl, ReadonlyBytes buffer, u32 ttc_index)
    : m_impl(move(impl))
    , m_buffer(buffer)
    , m_ttc_index(ttc_index)
{
}

}
