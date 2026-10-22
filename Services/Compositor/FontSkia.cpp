/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/TypeCasts.h>
#include <Compositor/FontSkia.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/RasterizerData.h>
#include <LibGfx/Font/TypefaceSkia.h>

#include <core/SkFont.h>
#include <core/SkFontTypes.h>
#include <core/SkTypeface.h>

#if defined(USE_FONTCONFIG)
#    include <LibGfx/Font/GlobalFontConfig.h>
#endif

namespace Compositor {

#if defined(USE_FONTCONFIG)
static bool s_force_hinting_for_testing { false };

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
// autohinting flag.
static constexpr u64 hinting_memo_holds_answer = 1;

static u64 encode_hinting_memo(float scale, Gfx::FontHintingOptions options)
{
    return (static_cast<u64>(bit_cast<u32>(scale)) << 32)
        | hinting_memo_holds_answer
        | (static_cast<u64>(to_underlying(options.style)) << 1)
        | (static_cast<u64>(options.force_autohinting) << 3);
}
#endif

namespace {

// What the compositor keeps with each font.
struct FontData final : public Gfx::RasterizerData {
#if defined(USE_FONTCONFIG)
    // A font is drawn at one scale most of the time, and fontconfig's answer is a pure function of the family, the
    // scaled pixel size, the weight and the slope, so one answer is kept.
    Atomic<u64> hinting_memo { 0 };
#endif
};

FontData& font_data(Gfx::Font const& font)
{
    return font.rasterizer_data<FontData>([] { return make<FontData>(); });
}

#if defined(USE_FONTCONFIG)
Gfx::FontHintingOptions hinting_options(Gfx::Font const& font, float scale)
{
    auto& memo = font_data(font).hinting_memo;
    auto value = memo.load(AK::MemoryOrder::memory_order_relaxed);
    if ((value & hinting_memo_holds_answer) != 0 && bit_cast<float>(static_cast<u32>(value >> 32)) == scale) {
        return Gfx::FontHintingOptions {
            .style = static_cast<Gfx::FontHintingStyle>((value >> 1) & 3),
            .force_autohinting = ((value >> 3) & 1) != 0,
        };
    }

    auto options = Gfx::GlobalFontConfig::the().hinting_for_font(font.family(), font.pixel_size() * scale, font.weight(), font.slope());
    memo.store(encode_hinting_memo(scale, options), AK::MemoryOrder::memory_order_relaxed);
    return options;
}
#endif

}

void force_font_hinting_for_testing()
{
#if defined(USE_FONTCONFIG)
    s_force_hinting_for_testing = true;
#endif
}

Optional<SkFont> skia_font(Gfx::Font const& font, float scale)
{
    auto const* typeface = as<Gfx::TypefaceSkia>(font.typeface()).sk_typeface();
    if (!typeface)
        return {};

    SkFont sk_font { sk_ref_sp(typeface), font.pixel_size() * scale };
    sk_font.setSubpixel(true);

#if defined(USE_FONTCONFIG)
    if (s_force_hinting_for_testing) {
        sk_font.setHinting(SkFontHinting::kNormal);
    } else {
        auto options = hinting_options(font, scale);
        sk_font.setHinting(to_skia_hinting(options.style));
        sk_font.setForceAutoHinting(options.force_autohinting);
    }
#endif

    return sk_font;
}

}
