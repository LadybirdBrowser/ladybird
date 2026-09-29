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
#if defined(AK_OS_ANDROID)
#    include <ports/SkFontMgr_android.h>
#elif defined(AK_OS_WINDOWS)
#    include <ports/SkFontMgr_empty.h>
#    include <ports/SkTypeface_win.h>
#else
#    include <ports/SkFontMgr_fontconfig.h>
#    include <ports/SkFontScanner_FreeType.h>
#endif

#ifdef AK_OS_MACOS
#    include <CoreText/CoreText.h>
#    include <harfbuzz/hb-coretext.h>
#    include <ports/SkFontMgr_mac_ct.h>
#    include <ports/SkTypeface_mac.h>
#endif

namespace Gfx {

static Atomic<u64> s_next_glyph_cache_id { 1 };

static auto& skia_font_manager()
{
    static NeverDestroyed<sk_sp<SkFontMgr>> font_manager;
    return *font_manager;
}

struct TypefaceSkia::Impl {
    AK_ALLOC_WITH_KMALLOC;

    Impl(sk_sp<SkTypeface> skia_typeface, std::unique_ptr<SkStreamAsset> stream = {}, Optional<SystemUIFontStyle> system_ui_font_style = {}
#ifdef AK_OS_MACOS
        ,
        CGFontRef cg_font = nullptr
#endif
        )
        : skia_typeface(move(skia_typeface))
        , stream(move(stream))
        , system_ui_font_style(move(system_ui_font_style))
    {
#ifdef AK_OS_MACOS
        if (cg_font) {
            this->cg_font = cg_font;
            CFRetain(this->cg_font);
        }
#endif
    }

    ~Impl()
    {
#ifdef AK_OS_MACOS
        if (cg_font)
            CFRelease(cg_font);
#endif
    }

    sk_sp<SkTypeface> skia_typeface;
    std::unique_ptr<SkStreamAsset> stream;

    // Skia reads a CoreText UI font as upright and regular because its descriptor carries no numeric style traits,
    // so a system UI typeface answers style queries from the style it was matched for instead.
    Optional<SystemUIFontStyle> system_ui_font_style;
    Optional<u16> data_font_weight;
#ifdef AK_OS_MACOS
    CGFontRef cg_font { nullptr };
#endif
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

#ifdef AK_OS_MACOS
static CTFontRef create_system_ui_font(SystemUIFontKind, float point_size, u8 slope);

ErrorOr<RefPtr<TypefaceSkia>> TypefaceSkia::typeface_from_core_text_typeface(sk_sp<SkTypeface> skia_typeface, CTFontRef ct_font, SystemUIFontStyle system_ui_font_style)
{
    if (!skia_typeface)
        return RefPtr<TypefaceSkia> {};

    auto cg_font = CTFontCopyGraphicsFont(ct_font, nullptr);
    if (!cg_font)
        return Error::from_string_literal("Failed to get graphics font from CoreText font");

    auto typeface = adopt_ref(*new TypefaceSkia {
        make<TypefaceSkia::Impl>(move(skia_typeface), std::unique_ptr<SkStreamAsset> {}, system_ui_font_style, cg_font),
        {},
        0 });
    CFRelease(cg_font);
    return typeface;
}
#endif

ErrorOr<RefPtr<TypefaceSkia>> TypefaceSkia::typeface_from_skia_typeface(sk_sp<SkTypeface> skia_typeface, Optional<SystemUIFontStyle> system_ui_font_style)
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
            make<TypefaceSkia::Impl>(skia_typeface, std::move(stream), system_ui_font_style),
            bytes,
            ttc_index });
    }

    if (!stream)
        return Error::from_string_literal("Failed to get font data from typeface");

    auto memory_stream = copy_stream_to_memory_stream(*stream);
    auto bytes = ReadonlyBytes { static_cast<u8 const*>(memory_stream->getMemoryBase()), memory_stream->getLength() };
    return adopt_ref(*new TypefaceSkia {
        make<TypefaceSkia::Impl>(skia_typeface, move(memory_stream), system_ui_font_style),
        bytes,
        ttc_index });
}

static u16 font_weight_from_data(ReadonlyBytes buffer, u32 ttc_index, Vector<FontVariationAxis> const& axes = {})
{
    auto* blob = hb_blob_create(reinterpret_cast<char const*>(buffer.data()), buffer.size(), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    ScopeGuard destroy_blob = [&] { hb_blob_destroy(blob); };
    auto* face = hb_face_create(blob, ttc_index);
    ScopeGuard destroy_face = [&] { hb_face_destroy(face); };
    auto* font = hb_font_create(face);
    ScopeGuard destroy_font = [&] { hb_font_destroy(font); };
    Vector<hb_variation_t> variations;
    for (auto const& axis : axes)
        variations.append({ axis.tag.to_u32(), axis.value });
    hb_font_set_variations(font, variations.data(), variations.size());
    return round_to<u16>(clamp(hb_style_get_value(font, HB_STYLE_TAG_WEIGHT), 0.0f, 1000.0f));
}

ErrorOr<NonnullRefPtr<TypefaceSkia>> TypefaceSkia::load_from_buffer(AK::ReadonlyBytes buffer, u32 ttc_index, RefPtr<FontDataBacking> backing)
{
    // NB: Skia can retain the typeface in text blobs and glyph caches after our Typeface is destroyed.
    //     Keep the backing alive through SkData, or copy bytes whose ownership is external.
    sk_sp<SkData> data;
    if (backing) {
        backing->ref();
        data = SkData::MakeWithProc(buffer.data(), buffer.size(), [](void const*, void* context) { static_cast<FontDataBacking*>(context)->unref(); }, backing.ptr());
    } else {
        data = SkData::MakeWithCopy(buffer.data(), buffer.size());
    }

    sk_sp<SkTypeface> skia_typeface;
    Optional<u16> data_font_weight;
#ifdef AK_OS_MACOS
    // NB: Skia's CoreText stream loader only supports collection index zero. Ask CoreText for the collection's
    //     descriptors directly, then wrap the selected face in Skia, as we do for system UI fonts.
    if (ttc_index != 0 && !FontDatabase::the().force_freetype_rasterization()) {
        if (buffer.size() > static_cast<size_t>(NumericLimits<CFIndex>::max()))
            return Error::from_string_literal("Font data is too large for CoreText");
        if (buffer.size() > NumericLimits<unsigned>::max())
            return Error::from_string_literal("Font data is too large for HarfBuzz");
        auto* blob = hb_blob_create(reinterpret_cast<char const*>(buffer.data()), buffer.size(), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        ScopeGuard destroy_blob = [&] { hb_blob_destroy(blob); };
        if (ttc_index >= hb_face_count(blob))
            return Error::from_string_literal("Font collection index is out of range");
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
        // NB: SkMakeTypefaceFromCTFont applies native system-font weight conversion even for data-created fonts.
        //     Use the supplied face's weight, as we would when loading it through Skia's stream loader.
        data_font_weight = font_weight_from_data(buffer, ttc_index);
    } else
#endif
    {
        // https://learn.microsoft.com/en-us/typography/opentype/spec/otff#ttc-header
        // TrueType Collection files bundle multiple fonts (often different weights of the same
        // family). We use SkFontArguments to specify which font to load from the collection.
        SkFontArguments font_args;
        font_args.setCollectionIndex(static_cast<int>(ttc_index));

        auto stream = std::make_unique<SkMemoryStream>(data);
        skia_typeface = font_manager().makeFromStream(std::move(stream), font_args);
    }

    if (!skia_typeface) {
        return Error::from_string_literal("Failed to load typeface from buffer");
    }

    auto typeface = adopt_ref(*new TypefaceSkia { make<TypefaceSkia::Impl>(skia_typeface), buffer, ttc_index });
    typeface->impl().data_font_weight = data_font_weight;
    if (backing)
        typeface->set_font_data(backing.release_nonnull());
    return typeface;
}

void TypefaceSkia::encode_font_data_for_ipc(IPC::Encoder& encoder) const
{
    if (has_font_data_backing()) {
        Typeface::encode_font_data_for_ipc(encoder);
        return;
    }

    if (impl().system_ui_font_style.has_value()) {
        MUST(encoder.encode(FontDataFormat::SystemUIFont));
        MUST(encoder.encode(*impl().system_ui_font_style));
        return;
    }

    auto family_name = family().to_string();

    MUST(encoder.encode(FontDataFormat::SystemFont));
    MUST(encoder.encode(family_name));
    MUST(encoder.encode(weight()));
    MUST(encoder.encode(width()));
    MUST(encoder.encode(slope()));
}

#ifdef AK_OS_MACOS
// NB: These are the CoreText string values behind the public AppKit NSFontDescriptorSystemDesign constants.
// Keeping them here avoids pulling Objective-C headers into this C++ file.
static CFStringRef core_text_ui_font_design(SystemUIFontKind kind)
{
    switch (kind) {
    case SystemUIFontKind::System:
        return CFSTR("NSCTFontUIFontDesignDefault");
    case SystemUIFontKind::Serif:
        return CFSTR("NSCTFontUIFontDesignSerif");
    case SystemUIFontKind::Monospace:
        return CFSTR("NSCTFontUIFontDesignMonospaced");
    case SystemUIFontKind::Rounded:
        return CFSTR("NSCTFontUIFontDesignRounded");
    }
    VERIFY_NOT_REACHED();
}

static CTFontDescriptorRef create_system_ui_font_descriptor(SystemUIFontKind kind, u8 slope)
{
    CGFloat core_text_slant = slope == 0 ? 0.0f : 1.0f;
    auto slant_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &core_text_slant);
    if (!slant_number)
        return nullptr;

    CFTypeRef trait_keys[] = { kCTFontSlantTrait, CFSTR("NSCTFontUIFontDesignTrait") };
    CFTypeRef trait_values[] = { slant_number, core_text_ui_font_design(kind) };
    auto traits = CFDictionaryCreate(kCFAllocatorDefault, trait_keys, trait_values, array_size(trait_keys), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(slant_number);
    if (!traits)
        return nullptr;

    CFTypeRef attribute_keys[] = { kCTFontTraitsAttribute };
    CFTypeRef attribute_values[] = { traits };
    auto attributes = CFDictionaryCreate(kCFAllocatorDefault, attribute_keys, attribute_values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(traits);
    if (!attributes)
        return nullptr;

    auto descriptor = CTFontDescriptorCreateWithAttributes(attributes);
    CFRelease(attributes);
    return descriptor;
}

static CTFontRef create_system_ui_font(SystemUIFontKind kind, float point_size, u8 slope)
{
    auto base_font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, point_size, nullptr);
    if (!base_font)
        return nullptr;

    auto descriptor = create_system_ui_font_descriptor(kind, slope);
    if (!descriptor) {
        CFRelease(base_font);
        return nullptr;
    }

    auto font = CTFontCreateCopyWithAttributes(base_font, point_size, nullptr, descriptor);
    if (!font)
        font = CTFontCreateWithFontDescriptor(descriptor, point_size, nullptr);
    CFRelease(base_font);
    CFRelease(descriptor);
    return font;
}
#endif

ErrorOr<RefPtr<TypefaceSkia>> TypefaceSkia::match_system_ui(SystemUIFontKind kind, float point_size, u16 weight, u16 width, u8 slope)
{
#ifdef AK_OS_MACOS
    auto ct_font = create_system_ui_font(kind, point_size, slope);
    if (!ct_font)
        return RefPtr<TypefaceSkia> {};

    auto skia_typeface = SkMakeTypefaceFromCTFont(ct_font);
    auto typeface = typeface_from_core_text_typeface(move(skia_typeface), ct_font, SystemUIFontStyle { kind, weight, width, slope });
    CFRelease(ct_font);
    return typeface;
#else
    (void)kind;
    (void)point_size;
    (void)weight;
    (void)width;
    (void)slope;
    return RefPtr<TypefaceSkia> {};
#endif
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

RefPtr<TypefaceSkia const> TypefaceSkia::clone_with_variations(Vector<FontVariationAxis> const& axes) const
{
    if (axes.is_empty())
        return this;

    SkFontArguments font_args;

    Vector<SkFontArguments::VariationPosition::Coordinate> coords;
    coords.ensure_capacity(axes.size());
    for (size_t i = 0; i < axes.size(); ++i) {
        coords.unchecked_append({ axes[i].tag.to_u32(), axes[i].value });
    }
    SkFontArguments::VariationPosition variation_pos;
    variation_pos.coordinates = coords.data();
    variation_pos.coordinateCount = static_cast<int>(coords.size());
    font_args.setVariationDesignPosition(variation_pos);

    font_args.setCollectionIndex(static_cast<int>(m_ttc_index));

    auto skia_typeface = impl().skia_typeface->makeClone(font_args);
    if (!skia_typeface)
        return {};

    if (has_font_data_backing() || impl().data_font_weight.has_value()) {
        auto typeface = adopt_ref(*new TypefaceSkia {
            make<TypefaceSkia::Impl>(skia_typeface, std::unique_ptr<SkStreamAsset> {}, impl().system_ui_font_style),
            m_buffer,
            m_ttc_index });
        typeface->copy_font_data_from(*this);
        if (impl().data_font_weight.has_value())
            typeface->impl().data_font_weight = font_weight_from_data(m_buffer, m_ttc_index, axes);
        return typeface;
    }

#ifdef AK_OS_MACOS
    if (impl().cg_font) {
        return adopt_ref(*new TypefaceSkia {
            make<TypefaceSkia::Impl>(move(skia_typeface), std::unique_ptr<SkStreamAsset> {}, impl().system_ui_font_style, impl().cg_font),
            {},
            m_ttc_index });
    }
#endif

    auto typeface_or_error = typeface_from_skia_typeface(move(skia_typeface), impl().system_ui_font_style);
    if (typeface_or_error.is_error())
        return {};
    return typeface_or_error.release_value();
}

SkTypeface const* TypefaceSkia::sk_typeface() const
{
    return impl().skia_typeface.get();
}

u32 TypefaceSkia::platform_typeface_id() const
{
    return impl().skia_typeface->uniqueID();
}

hb_face_t* TypefaceSkia::create_harfbuzz_face() const
{
#ifdef AK_OS_MACOS
    if (impl().cg_font)
        return hb_coretext_face_create(impl().cg_font);
#endif
    return Typeface::create_harfbuzz_face();
}

TypefaceSkia::TypefaceSkia(NonnullOwnPtr<Impl> impl, ReadonlyBytes buffer, u32 ttc_index)
    : m_impl(move(impl))
    , m_buffer(buffer)
    , m_ttc_index(ttc_index)
    , m_glyph_cache_id(s_next_glyph_cache_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
{
    VERIFY(m_glyph_cache_id != 0);
}

u32 TypefaceSkia::glyph_count() const
{
    return impl().skia_typeface->countGlyphs();
}

u16 TypefaceSkia::units_per_em() const
{
    return impl().skia_typeface->getUnitsPerEm();
}

u32 TypefaceSkia::glyph_id_for_code_point(u32 code_point) const
{
    return glyph_page(code_point / GlyphPage::glyphs_per_page).glyph_ids[code_point % GlyphPage::glyphs_per_page];
}

TypefaceSkia::GlyphPage const& TypefaceSkia::glyph_page(size_t page_index) const
{
    struct GlyphPageCache {
        AK_ALLOC_WITH_KMALLOC;

        u64 last_use { 0 };
        OwnPtr<GlyphPage> page_zero;
        HashMap<size_t, NonnullOwnPtr<GlyphPage>> pages;
    };
    struct ThreadGlyphPageCaches {
        u64 last_typeface_id { 0 };
        GlyphPageCache* last_cache { nullptr };
        u64 use_clock { 0 };
        HashMap<u64, NonnullOwnPtr<GlyphPageCache>> caches;
    };
    // NB: -Wexit-time-destructors is a Clang-only warning, and GCC rejects the
    //     unknown option name in the pragma.
#ifdef AK_COMPILER_CLANG
    AK_IGNORE_DIAGNOSTIC("-Wexit-time-destructors", static thread_local ThreadGlyphPageCaches thread_caches)
#else
    static thread_local ThreadGlyphPageCaches thread_caches;
#endif

    auto& caches = thread_caches.caches;
    auto* cache = thread_caches.last_cache;
    if (thread_caches.last_typeface_id != m_glyph_cache_id) {
        if (auto it = caches.find(m_glyph_cache_id); it != caches.end()) {
            cache = it->value.ptr();
        } else {
            constexpr size_t maximum_cached_typefaces = 128;
            if (caches.size() >= maximum_cached_typefaces) {
                // NB: Evict the cache this thread used least recently. The caches of typefaces that are gone go first,
                //     and the ones a text run alternates between stay: evicting any other would have them evict each
                //     other at every switch once the caches of a long session filled up.
                auto least_recently_used = caches.begin();
                for (auto it = caches.begin(); it != caches.end(); ++it) {
                    if (it->value->last_use < least_recently_used->value->last_use)
                        least_recently_used = it;
                }
                caches.remove(least_recently_used);
            }
            auto new_cache = make<GlyphPageCache>();
            cache = new_cache.ptr();
            caches.set(m_glyph_cache_id, move(new_cache));
        }
        cache->last_use = ++thread_caches.use_clock;
        thread_caches.last_typeface_id = m_glyph_cache_id;
        thread_caches.last_cache = cache;
    }

    if (page_index == 0) {
        if (!cache->page_zero) {
            cache->page_zero = make<GlyphPage>();
            populate_glyph_page(*cache->page_zero, 0);
        }
        return *cache->page_zero;
    }
    if (auto it = cache->pages.find(page_index); it != cache->pages.end()) {
        return *it->value;
    }

    auto glyph_page = make<GlyphPage>();
    populate_glyph_page(*glyph_page, page_index);
    auto const* glyph_page_ptr = glyph_page.ptr();
    cache->pages.set(page_index, move(glyph_page));
    return *glyph_page_ptr;
}

static thread_local u64 s_glyph_pages_populated_on_this_thread = 0;

u64 TypefaceSkia::glyph_pages_populated_on_this_thread()
{
    return s_glyph_pages_populated_on_this_thread;
}

void TypefaceSkia::populate_glyph_page(GlyphPage& glyph_page, size_t page_index) const
{
    ++s_glyph_pages_populated_on_this_thread;
    u32 first_code_point = page_index * GlyphPage::glyphs_per_page;
    for (size_t i = 0; i < GlyphPage::glyphs_per_page; ++i) {
        u32 code_point = first_code_point + i;
        glyph_page.glyph_ids[i] = impl().skia_typeface->unicharToGlyph(code_point);
    }
}

FlyString const& TypefaceSkia::family() const
{
    call_once(m_family_once, [&] {
        SkString family_name;
        impl().skia_typeface->getFamilyName(&family_name);
        m_family = FlyString::from_utf8_without_validation(ReadonlyBytes { family_name.c_str(), family_name.size() });
    });
    return *m_family;
}

u16 TypefaceSkia::weight() const
{
    if (auto const& style = impl().system_ui_font_style; style.has_value())
        return style->weight;
    if (impl().data_font_weight.has_value())
        return impl().data_font_weight.value();
    return impl().skia_typeface->fontStyle().weight();
}

u16 TypefaceSkia::width() const
{
    if (auto const& style = impl().system_ui_font_style; style.has_value())
        return style->width;
    return impl().skia_typeface->fontStyle().width();
}

u8 TypefaceSkia::slope() const
{
    if (auto const& style = impl().system_ui_font_style; style.has_value())
        return style->slope;

    auto slant = impl().skia_typeface->fontStyle().slant();
    switch (slant) {
    case SkFontStyle::kUpright_Slant:
        return 0;
    case SkFontStyle::kItalic_Slant:
        return 1;
    case SkFontStyle::kOblique_Slant:
        return 2;
    default:
        return 0;
    }
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::SystemUIFontStyle const& style)
{
    TRY(encoder.encode(style.kind));
    TRY(encoder.encode(style.weight));
    TRY(encoder.encode(style.width));
    TRY(encoder.encode(style.slope));
    return {};
}

template<>
ErrorOr<Gfx::SystemUIFontStyle> decode(Decoder& decoder)
{
    auto kind = TRY(decoder.decode<Gfx::SystemUIFontKind>());
    auto weight = TRY(decoder.decode<u16>());
    auto width = TRY(decoder.decode<u16>());
    auto slope = TRY(decoder.decode<u8>());
    return Gfx::SystemUIFontStyle { kind, weight, width, slope };
}

}
