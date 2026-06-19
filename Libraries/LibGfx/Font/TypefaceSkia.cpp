/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/LsanSuppressions.h>
#include <AK/NeverDestroyed.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibIPC/Encoder.h>

#include <core/SkData.h>
#include <core/SkFontArguments.h>
#include <core/SkFontMgr.h>
#include <core/SkStream.h>
#include <core/SkString.h>
#include <core/SkTypeface.h>
#if defined(AK_OS_ANDROID)
#    include <ports/SkFontMgr_android.h>
#elif defined(AK_OS_WINDOWS)
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

static auto& skia_font_manager()
{
    static NeverDestroyed<sk_sp<SkFontMgr>> font_manager;
    return *font_manager;
}

struct TypefaceSkia::Impl {
    Impl(sk_sp<SkTypeface> skia_typeface, std::unique_ptr<SkStreamAsset> stream = {}, Optional<SystemUIFontKind> system_ui_font_kind = {}
#ifdef AK_OS_MACOS
        ,
        CGFontRef cg_font = nullptr
#endif
        )
        : skia_typeface(move(skia_typeface))
        , stream(move(stream))
        , system_ui_font_kind(move(system_ui_font_kind))
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
    Optional<SystemUIFontKind> system_ui_font_kind;
#ifdef AK_OS_MACOS
    CGFontRef cg_font { nullptr };
#endif
};

static SkFontMgr& font_manager()
{
    auto& font_manager = skia_font_manager();
    if (!font_manager) {
#ifdef AK_OS_MACOS
        if (Gfx::FontDatabase::the().system_font_provider_name() != "FontConfig"sv) {
            font_manager = SkFontMgr_New_CoreText(nullptr);
        }
#endif
#if defined(AK_OS_ANDROID)
        font_manager = SkFontMgr_New_Android(nullptr);
#elif defined(AK_OS_WINDOWS)
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

ErrorOr<RefPtr<TypefaceSkia>> TypefaceSkia::typeface_from_core_text_typeface(sk_sp<SkTypeface> skia_typeface, CTFontRef ct_font, SystemUIFontKind system_ui_font_kind)
{
    if (!skia_typeface)
        return RefPtr<TypefaceSkia> {};

    auto cg_font = CTFontCopyGraphicsFont(ct_font, nullptr);
    if (!cg_font)
        return Error::from_string_literal("Failed to get graphics font from CoreText font");

    auto typeface = adopt_ref(*new TypefaceSkia {
        make<TypefaceSkia::Impl>(move(skia_typeface), std::unique_ptr<SkStreamAsset> {}, system_ui_font_kind, cg_font),
        {},
        0 });
    CFRelease(cg_font);
    return typeface;
}
#endif

ErrorOr<RefPtr<TypefaceSkia>> TypefaceSkia::typeface_from_skia_typeface(sk_sp<SkTypeface> skia_typeface, Optional<SystemUIFontKind> system_ui_font_kind)
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
            make<TypefaceSkia::Impl>(skia_typeface, std::move(stream), system_ui_font_kind),
            bytes,
            ttc_index });
    }

    if (!stream)
        return Error::from_string_literal("Failed to get font data from typeface");

    auto memory_stream = copy_stream_to_memory_stream(*stream);
    auto bytes = ReadonlyBytes { static_cast<u8 const*>(memory_stream->getMemoryBase()), memory_stream->getLength() };
    return adopt_ref(*new TypefaceSkia {
        make<TypefaceSkia::Impl>(skia_typeface, move(memory_stream), system_ui_font_kind),
        bytes,
        ttc_index });
}

ErrorOr<NonnullRefPtr<TypefaceSkia>> TypefaceSkia::load_from_buffer(AK::ReadonlyBytes buffer, u32 ttc_index)
{
    auto data = SkData::MakeWithoutCopy(buffer.data(), buffer.size());

    // https://learn.microsoft.com/en-us/typography/opentype/spec/otff#ttc-header
    // TrueType Collection files bundle multiple fonts (often different weights of the same
    // family). We use SkFontArguments to specify which font to load from the collection.
    SkFontArguments font_args;
    font_args.setCollectionIndex(static_cast<int>(ttc_index));

    auto stream = std::make_unique<SkMemoryStream>(data);
    auto skia_typeface = font_manager().makeFromStream(std::move(stream), font_args);

    if (!skia_typeface) {
        return Error::from_string_literal("Failed to load typeface from buffer");
    }

    return adopt_ref(*new TypefaceSkia { make<TypefaceSkia::Impl>(skia_typeface), buffer, ttc_index });
}

void TypefaceSkia::encode_font_data_for_ipc(IPC::Encoder& encoder) const
{
    if (has_font_data_backing()) {
        Typeface::encode_font_data_for_ipc(encoder);
        return;
    }

    auto family_name = family().to_string();

    MUST(encoder.encode(FontDataFormat::SystemFont));
    MUST(encoder.encode(impl().system_ui_font_kind));
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

ErrorOr<RefPtr<TypefaceSkia>> TypefaceSkia::match_system_ui(SystemUIFontKind kind, float point_size, u16 weight, double width, u8 slope)
{
#ifdef AK_OS_MACOS
    (void)weight;
    (void)width;
    auto ct_font = create_system_ui_font(kind, point_size, slope);
    if (!ct_font)
        return RefPtr<TypefaceSkia> {};

    auto skia_typeface = SkMakeTypefaceFromCTFont(ct_font);
    auto typeface = typeface_from_core_text_typeface(move(skia_typeface), ct_font, kind);
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

    if (has_font_data_backing()) {
        auto typeface = adopt_ref(*new TypefaceSkia {
            make<TypefaceSkia::Impl>(skia_typeface, std::unique_ptr<SkStreamAsset> {}, impl().system_ui_font_kind),
            m_buffer,
            m_ttc_index });
        typeface->copy_font_data_from(*this);
        return typeface;
    }

#ifdef AK_OS_MACOS
    if (impl().cg_font) {
        return adopt_ref(*new TypefaceSkia {
            make<TypefaceSkia::Impl>(move(skia_typeface), std::unique_ptr<SkStreamAsset> {}, impl().system_ui_font_kind, impl().cg_font),
            {},
            m_ttc_index });
    }
#endif

    auto typeface_or_error = typeface_from_skia_typeface(move(skia_typeface), impl().system_ui_font_kind);
    if (typeface_or_error.is_error())
        return {};
    return typeface_or_error.release_value();
}

SkTypeface const* TypefaceSkia::sk_typeface() const
{
    return impl().skia_typeface.get();
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
{
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
    if (page_index == 0) {
        if (!m_glyph_page_zero) {
            m_glyph_page_zero = make<GlyphPage>();
            populate_glyph_page(*m_glyph_page_zero, 0);
        }
        return *m_glyph_page_zero;
    }
    if (auto it = m_glyph_pages.find(page_index); it != m_glyph_pages.end()) {
        return *it->value;
    }

    auto glyph_page = make<GlyphPage>();
    populate_glyph_page(*glyph_page, page_index);
    auto const* glyph_page_ptr = glyph_page.ptr();
    m_glyph_pages.set(page_index, move(glyph_page));
    return *glyph_page_ptr;
}

void TypefaceSkia::populate_glyph_page(GlyphPage& glyph_page, size_t page_index) const
{
    u32 first_code_point = page_index * GlyphPage::glyphs_per_page;
    for (size_t i = 0; i < GlyphPage::glyphs_per_page; ++i) {
        u32 code_point = first_code_point + i;
        glyph_page.glyph_ids[i] = impl().skia_typeface->unicharToGlyph(code_point);
    }
}

FlyString const& TypefaceSkia::family() const
{
    return m_family.ensure([&] {
        SkString family_name;
        impl().skia_typeface->getFamilyName(&family_name);
        return FlyString::from_utf8_without_validation(ReadonlyBytes { family_name.c_str(), family_name.size() });
    });
}

u16 TypefaceSkia::weight() const
{
    return impl().skia_typeface->fontStyle().weight();
}

u16 TypefaceSkia::width() const
{
    return impl().skia_typeface->fontStyle().width();
}

u8 TypefaceSkia::slope() const
{
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
