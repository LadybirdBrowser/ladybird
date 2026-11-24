/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Math.h>
#include <AK/NeverDestroyed.h>
#include <AK/ScopeGuard.h>
#include <AK/TypeCasts.h>
#include <Compositor/FontSkia.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/RasterizerData.h>

#include <core/SkData.h>
#include <core/SkFont.h>
#include <core/SkFontArguments.h>
#include <core/SkFontMgr.h>
#include <core/SkFontTypes.h>
#include <core/SkStream.h>
#include <core/SkSurfaceProps.h>
#include <core/SkTypeface.h>
#include <ports/SkFontMgr_empty.h>

#if defined(USE_FONTCONFIG)
#    include <LibGfx/Font/GlobalFontConfig.h>
#endif

#ifdef AK_OS_MACOS
#    include <LibGfx/Font/TypefaceCoreText.h>
#    include <ports/SkTypeface_mac.h>
#    include <src/ports/SkTypeface_mac_ct.h>
#endif

#ifdef AK_OS_WINDOWS
#    include <src/ports/SkTypeface_win_dw.h>
#endif

using Compositor::FontRasterizer;

static FontRasterizer s_font_rasterizer { FontRasterizer::Platform };
static bool s_use_test_font_rendering { false };

#if defined(USE_FONTCONFIG)
static SkFontHinting to_skia_hinting(Gfx::FontHintingStyle style)
{
    switch (style) {
    case Gfx::FontHintingStyle::None:
        return SkFontHinting::kNone;
    case Gfx::FontHintingStyle::Slight:
        return SkFontHinting::kSlight;
    case Gfx::FontHintingStyle::Normal:
        return SkFontHinting::kNormal;
    case Gfx::FontHintingStyle::Full:
        return SkFontHinting::kFull;
    }
    VERIFY_NOT_REACHED();
}

// The scale's bits sit above, bit 0 says the word holds an answer, bits 1 and 2 carry the style and bit 3 the
// autohinting flag, and bit 4 records whether the font allows LCD text.
static constexpr u64 rendering_options_memo_holds_answer = 1;

static u64 encode_rendering_options(float scale, Gfx::FontRenderingOptions options)
{
    return (static_cast<u64>(bit_cast<u32>(scale)) << 32)
        | rendering_options_memo_holds_answer
        | (static_cast<u64>(to_underlying(options.hinting_style)) << 1)
        | (static_cast<u64>(options.force_autohinting) << 3)
        | (static_cast<u64>(options.allows_lcd_text) << 4);
}
#endif

// What the compositor keeps with each typeface.
struct Gfx::TypefaceRasterizerData final : public Gfx::RasterizerData {
    explicit TypefaceRasterizerData(sk_sp<SkTypeface> typeface)
        : typeface(move(typeface))
    {
    }

    sk_sp<SkTypeface> typeface;
};

namespace {

#if defined(AK_OS_MACOS)
// CoreText places the bitmap glyphs of an installed font, such as Apple Color Emoji, lower than it places the same
// glyphs of font data, whose bitmaps would then sit on the baseline. So an installed font is drawn by its name. Only
// the font that draws the glyphs changes, which has the same glyphs as the data that shapes them.
static sk_sp<SkTypeface> make_installed_bitmap_font_skia_typeface(Gfx::Typeface const& typeface)
{
    if (!typeface.system_font_identifier().has_value() || !Gfx::typeface_has_bitmap_glyphs(typeface))
        return nullptr;
    auto postscript_name = typeface.postscript_name();
    if (!postscript_name.has_value())
        return nullptr;
    auto installed_typeface = Gfx::TypefaceCoreText::try_load_postscript_name(*postscript_name);
    if (installed_typeface.is_error())
        return nullptr;
    return SkMakeTypefaceFromCTFont(installed_typeface.value()->core_text_font());
}
#endif

// The Skia typeface of font data from the font manager of the platform. Null where FreeType is that font manager.
// The typeface loaders are called directly, because a font manager also finds the system fonts when it is made.
sk_sp<SkTypeface> make_platform_skia_typeface([[maybe_unused]] sk_sp<SkData> const& data, [[maybe_unused]] Gfx::Typeface const& typeface)
{
    if (s_font_rasterizer == FontRasterizer::FreeType)
        return nullptr;
#if defined(AK_OS_MACOS)
    if (auto installed_typeface = make_installed_bitmap_font_skia_typeface(typeface))
        return installed_typeface;

    // The CoreText typeface loader of Skia loads only the first face of font data.
    if (typeface.collection_index() != 0) {
        auto font = Gfx::create_core_text_font_from_data(typeface.font_data_backing().release_nonnull(), typeface.font_data(), typeface.collection_index());
        if (!font)
            return nullptr;
        ScopeGuard release_font = [&] { CFRelease(font); };
        return SkMakeTypefaceFromCTFont(font);
    }
    return SkTypeface_Mac::MakeFromStream(std::make_unique<SkMemoryStream>(data), SkFontArguments {});
#elif defined(AK_OS_WINDOWS)
    return DWriteFontTypeface::MakeFromStream(std::make_unique<SkMemoryStream>(data), SkFontArguments {}.setCollectionIndex(static_cast<int>(typeface.collection_index())));
#else
    return nullptr;
#endif
}

sk_sp<SkTypeface> make_skia_typeface(Gfx::Typeface const& typeface)
{
#ifdef AK_OS_MACOS
    if (auto const* core_text_typeface = as_if<Gfx::TypefaceCoreText>(typeface))
        return SkMakeTypefaceFromCTFont(core_text_typeface->core_text_font());
#endif
    auto backing = typeface.font_data_backing();
    if (!backing)
        return nullptr;

    // Skia can keep a typeface in text blobs and glyph caches after the typeface is gone, so the data keeps its backing.
    auto bytes = typeface.font_data();
    auto data = SkData::MakeWithProc(
        bytes.data(), bytes.size(), [](void const*, void* context) { static_cast<Gfx::Typeface::FontDataBacking*>(context)->unref(); }, backing.leak_ref());
    if (auto skia_typeface = make_platform_skia_typeface(data, typeface))
        return skia_typeface;

    // FreeType also loads some data that the font manager of the platform does not.
    static NeverDestroyed<sk_sp<SkFontMgr>> font_manager { SkFontMgr_New_Custom_Empty() };
    if (auto skia_typeface = (*font_manager)->makeFromStream(std::make_unique<SkMemoryStream>(data), SkFontArguments {}.setCollectionIndex(static_cast<int>(typeface.collection_index()))))
        return skia_typeface;

    dbgln("Compositor: Skia does not load the typeface of family '{}'", typeface.family());
    return nullptr;
}

// The Skia typeface of a typeface. Null if Skia cannot load it.
sk_sp<SkTypeface> const& skia_typeface(Gfx::Typeface const& typeface)
{
    return typeface.rasterizer_data([&] { return make<Gfx::TypefaceRasterizerData>(make_skia_typeface(typeface)); }).typeface;
}

// The typeface of the font's variations. Without variations, or if Skia cannot apply them, it is the typeface itself.
sk_sp<SkTypeface> typeface_with_variations(Gfx::Font const& font)
{
    auto const& typeface = skia_typeface(font.typeface());
    if (!typeface)
        return nullptr;
    auto axes = font.variation_settings().to_sorted_list();
    if (axes.is_empty())
        return typeface;

    Vector<SkFontArguments::VariationPosition::Coordinate> coordinates;
    coordinates.ensure_capacity(axes.size());
    for (auto const& axis : axes)
        coordinates.unchecked_append({ axis.tag.to_u32(), axis.value });
    SkFontArguments arguments;
    arguments.setVariationDesignPosition({ coordinates.data(), static_cast<int>(coordinates.size()) });
    arguments.setCollectionIndex(static_cast<int>(font.typeface().collection_index()));
    if (auto clone = typeface->makeClone(arguments))
        return clone;
    return typeface;
}

}

// What the compositor keeps with each font.
struct Gfx::FontRasterizerData final : public Gfx::RasterizerData {
    explicit FontRasterizerData(Font const& font)
        : typeface(typeface_with_variations(font))
    {
    }

    sk_sp<SkTypeface> typeface;
#if defined(USE_FONTCONFIG)
    // A font is drawn at one scale most of the time, and fontconfig's answer is a pure function of the family, the
    // scaled pixel size, the weight and the slope, so one answer is kept.
    Atomic<u64> rendering_options_memo { 0 };
#endif
};

namespace {

Gfx::FontRasterizerData& font_data(Gfx::Font const& font)
{
    return font.rasterizer_data([&] { return make<Gfx::FontRasterizerData>(font); });
}

#if defined(USE_FONTCONFIG)
Gfx::FontRenderingOptions rendering_options(Gfx::Font const& font, float scale, Gfx::FontRasterizerData& data)
{
    auto& memo = data.rendering_options_memo;
    auto value = memo.load(AK::MemoryOrder::memory_order_relaxed);
    if ((value & rendering_options_memo_holds_answer) != 0 && bit_cast<float>(static_cast<u32>(value >> 32)) == scale) {
        return Gfx::FontRenderingOptions {
            .hinting_style = static_cast<Gfx::FontHintingStyle>((value >> 1) & 3),
            .force_autohinting = ((value >> 3) & 1) != 0,
            .allows_lcd_text = ((value >> 4) & 1) != 0,
        };
    }

    auto options = Gfx::GlobalFontConfig::the().rendering_options_for_font(font.family(), font.pixel_size() * scale, font.weight(), font.slope());
    memo.store(encode_rendering_options(scale, options), AK::MemoryOrder::memory_order_relaxed);
    return options;
}
#endif

}

namespace Compositor {

void set_font_rasterizer(FontRasterizer rasterizer)
{
    s_font_rasterizer = rasterizer;
}

void use_test_font_rendering()
{
    s_use_test_font_rendering = true;
}

#if defined(AK_OS_LINUX) && defined(USE_FONTCONFIG)
static SkPixelGeometry configured_pixel_geometry()
{
    switch (Gfx::GlobalFontConfig::the().subpixel_order()) {
    case Gfx::FontSubpixelOrder::None:
        return kUnknown_SkPixelGeometry;
    case Gfx::FontSubpixelOrder::RGB:
        return kRGB_H_SkPixelGeometry;
    case Gfx::FontSubpixelOrder::BGR:
        return kBGR_H_SkPixelGeometry;
    case Gfx::FontSubpixelOrder::VerticalRGB:
        return kRGB_V_SkPixelGeometry;
    case Gfx::FontSubpixelOrder::VerticalBGR:
        return kBGR_V_SkPixelGeometry;
    }
    VERIFY_NOT_REACHED();
}
#endif

SkSurfaceProps skia_text_surface_properties(u32 flags, [[maybe_unused]] bool allows_lcd_text)
{
    auto geometry = kUnknown_SkPixelGeometry;
#if defined(AK_OS_LINUX) && defined(USE_FONTCONFIG)
    if (allows_lcd_text && !s_use_test_font_rendering) {
        static auto const configured_geometry = configured_pixel_geometry();
        geometry = configured_geometry;
    }
#endif
#if defined(AK_OS_LINUX)
    // Skia's default sRGB text gamma makes partially covered pixels too bright on dark backgrounds.
    // Use Chromium's Linux text contrast and gamma.
    if (!s_use_test_font_rendering)
        return { flags, geometry, 0.2f, 1.2f };
#endif
    return { flags, geometry };
}

Optional<SkFont> skia_font(Gfx::Font const& font, float scale, [[maybe_unused]] bool requests_lcd_text)
{
    auto& data = font_data(font);
    if (!data.typeface)
        return {};

    SkFont sk_font { data.typeface, font.pixel_size() * scale };
    sk_font.setSubpixel(true);

    // NB: Skia's x axis runs as text does but its y axis runs downward, so a slant that tips the glyphs' tops toward
    //     the end of the line is a negative skew.
    auto synthesis = font.synthesis();
    sk_font.setEmbolden(synthesis.bold);
    if (synthesis.oblique_angle != 0)
        sk_font.setSkewX(-AK::tan(AK::to_radians(synthesis.oblique_angle)));

#if defined(USE_FONTCONFIG)
    if (s_use_test_font_rendering) {
        sk_font.setHinting(SkFontHinting::kNormal);
    } else {
        auto options = rendering_options(font, scale, data);
        sk_font.setHinting(to_skia_hinting(options.hinting_style));
        sk_font.setForceAutoHinting(options.force_autohinting);
        if (requests_lcd_text && options.allows_lcd_text)
            sk_font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    }
#endif

    return sk_font;
}

}
