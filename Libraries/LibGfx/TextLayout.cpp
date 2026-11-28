/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/StdLibExtras.h>
#include <LibUnicode/CharacterTypes.h>
#include <RustFFI.h>
#include <harfbuzz/hb.h>

// The Unicode lookups that shaping in Rust makes. Declared here so that each definition has a previous declaration.
extern "C" {
bool ladybird_gfx_code_point_has_cursive_script_extension(u32);
bool ladybird_gfx_code_point_is_default_ignorable(u32);
}

// Whether one of the script extensions of the code point is a cursive script:
// https://drafts.csswg.org/css-text-3/#cursive-script
extern "C" bool ladybird_gfx_code_point_has_cursive_script_extension(u32 code_point)
{
    static auto const cursive_scripts = [] {
        constexpr Array names { "Arab"sv, "Rohg"sv, "Mand"sv, "Mong"sv, "Nkoo"sv, "Phag"sv, "Syrc"sv };
        Array<Unicode::Script, names.size()> scripts;
        for (size_t i = 0; i < names.size(); ++i)
            scripts[i] = Unicode::script_from_string(names[i]).release_value();
        return scripts;
    }();
    return any_of(cursive_scripts, [&](auto script) { return Unicode::code_point_has_script_extension(code_point, script); });
}

extern "C" bool ladybird_gfx_code_point_is_default_ignorable(u32 code_point)
{
    return Unicode::code_point_has_default_ignorable_code_point_property(code_point);
}

// The HarfBuzz types and values that the Rust crate declares to call HarfBuzz with (Libraries/LibGfx/Rust/src/harfbuzz.rs),
// checked against the headers of the HarfBuzz that LibGfx links.
static_assert(sizeof(Gfx::FFI::HbGlyphInfo) == sizeof(hb_glyph_info_t));
static_assert(alignof(Gfx::FFI::HbGlyphInfo) == alignof(hb_glyph_info_t));
static_assert(IsSame<decltype(Gfx::FFI::HbGlyphInfo::codepoint), decltype(hb_glyph_info_t::codepoint)>);
static_assert(IsSame<decltype(Gfx::FFI::HbGlyphInfo::mask), decltype(hb_glyph_info_t::mask)>);
static_assert(IsSame<decltype(Gfx::FFI::HbGlyphInfo::cluster), decltype(hb_glyph_info_t::cluster)>);
static_assert(offsetof(Gfx::FFI::HbGlyphInfo, codepoint) == offsetof(hb_glyph_info_t, codepoint));
static_assert(offsetof(Gfx::FFI::HbGlyphInfo, mask) == offsetof(hb_glyph_info_t, mask));
static_assert(offsetof(Gfx::FFI::HbGlyphInfo, cluster) == offsetof(hb_glyph_info_t, cluster));
static_assert(offsetof(Gfx::FFI::HbGlyphInfo, var1) == offsetof(hb_glyph_info_t, var1));
static_assert(offsetof(Gfx::FFI::HbGlyphInfo, var2) == offsetof(hb_glyph_info_t, var2));

static_assert(sizeof(Gfx::FFI::HbGlyphPosition) == sizeof(hb_glyph_position_t));
static_assert(alignof(Gfx::FFI::HbGlyphPosition) == alignof(hb_glyph_position_t));
static_assert(IsSame<decltype(Gfx::FFI::HbGlyphPosition::x_advance), decltype(hb_glyph_position_t::x_advance)>);
static_assert(IsSame<decltype(Gfx::FFI::HbGlyphPosition::y_advance), decltype(hb_glyph_position_t::y_advance)>);
static_assert(IsSame<decltype(Gfx::FFI::HbGlyphPosition::x_offset), decltype(hb_glyph_position_t::x_offset)>);
static_assert(IsSame<decltype(Gfx::FFI::HbGlyphPosition::y_offset), decltype(hb_glyph_position_t::y_offset)>);
static_assert(offsetof(Gfx::FFI::HbGlyphPosition, x_advance) == offsetof(hb_glyph_position_t, x_advance));
static_assert(offsetof(Gfx::FFI::HbGlyphPosition, y_advance) == offsetof(hb_glyph_position_t, y_advance));
static_assert(offsetof(Gfx::FFI::HbGlyphPosition, x_offset) == offsetof(hb_glyph_position_t, x_offset));
static_assert(offsetof(Gfx::FFI::HbGlyphPosition, y_offset) == offsetof(hb_glyph_position_t, y_offset));
static_assert(offsetof(Gfx::FFI::HbGlyphPosition, var) == offsetof(hb_glyph_position_t, var));

static_assert(sizeof(Gfx::FFI::HbFeature) == sizeof(hb_feature_t));
static_assert(alignof(Gfx::FFI::HbFeature) == alignof(hb_feature_t));
static_assert(IsSame<decltype(Gfx::FFI::HbFeature::tag), decltype(hb_feature_t::tag)>);
static_assert(IsSame<decltype(Gfx::FFI::HbFeature::value), decltype(hb_feature_t::value)>);
static_assert(IsSame<decltype(Gfx::FFI::HbFeature::start), decltype(hb_feature_t::start)>);
static_assert(IsSame<decltype(Gfx::FFI::HbFeature::end), decltype(hb_feature_t::end)>);
static_assert(offsetof(Gfx::FFI::HbFeature, tag) == offsetof(hb_feature_t, tag));
static_assert(offsetof(Gfx::FFI::HbFeature, value) == offsetof(hb_feature_t, value));
static_assert(offsetof(Gfx::FFI::HbFeature, start) == offsetof(hb_feature_t, start));
static_assert(offsetof(Gfx::FFI::HbFeature, end) == offsetof(hb_feature_t, end));

static_assert(sizeof(Gfx::FFI::HbGlyphExtents) == sizeof(hb_glyph_extents_t));
static_assert(alignof(Gfx::FFI::HbGlyphExtents) == alignof(hb_glyph_extents_t));
static_assert(IsSame<decltype(Gfx::FFI::HbGlyphExtents::x_bearing), decltype(hb_glyph_extents_t::x_bearing)>);
static_assert(IsSame<decltype(Gfx::FFI::HbGlyphExtents::y_bearing), decltype(hb_glyph_extents_t::y_bearing)>);
static_assert(IsSame<decltype(Gfx::FFI::HbGlyphExtents::width), decltype(hb_glyph_extents_t::width)>);
static_assert(IsSame<decltype(Gfx::FFI::HbGlyphExtents::height), decltype(hb_glyph_extents_t::height)>);
static_assert(offsetof(Gfx::FFI::HbGlyphExtents, x_bearing) == offsetof(hb_glyph_extents_t, x_bearing));
static_assert(offsetof(Gfx::FFI::HbGlyphExtents, y_bearing) == offsetof(hb_glyph_extents_t, y_bearing));
static_assert(offsetof(Gfx::FFI::HbGlyphExtents, width) == offsetof(hb_glyph_extents_t, width));
static_assert(offsetof(Gfx::FFI::HbGlyphExtents, height) == offsetof(hb_glyph_extents_t, height));

static_assert(sizeof(Gfx::FFI::HbDrawState) == sizeof(hb_draw_state_t));
static_assert(alignof(Gfx::FFI::HbDrawState) == alignof(hb_draw_state_t));
static_assert(IsSame<decltype(Gfx::FFI::HbDrawState::path_open), decltype(hb_draw_state_t::path_open)>);
static_assert(IsSame<decltype(Gfx::FFI::HbDrawState::current_x), decltype(hb_draw_state_t::current_x)>);
static_assert(IsSame<decltype(Gfx::FFI::HbDrawState::current_y), decltype(hb_draw_state_t::current_y)>);
static_assert(offsetof(Gfx::FFI::HbDrawState, path_open) == offsetof(hb_draw_state_t, path_open));
static_assert(offsetof(Gfx::FFI::HbDrawState, path_start_x) == offsetof(hb_draw_state_t, path_start_x));
static_assert(offsetof(Gfx::FFI::HbDrawState, path_start_y) == offsetof(hb_draw_state_t, path_start_y));
static_assert(offsetof(Gfx::FFI::HbDrawState, current_x) == offsetof(hb_draw_state_t, current_x));
static_assert(offsetof(Gfx::FFI::HbDrawState, current_y) == offsetof(hb_draw_state_t, current_y));
static_assert(offsetof(Gfx::FFI::HbDrawState, reserved) == offsetof(hb_draw_state_t, reserved1));

// The enumerations are as wide as the u32 that the crate passes and returns them as.
static_assert(sizeof(hb_direction_t) == sizeof(u32));
static_assert(sizeof(hb_script_t) == sizeof(u32));
static_assert(sizeof(hb_bool_t) == sizeof(int));
static_assert(Gfx::FFI::DIRECTION_LTR == static_cast<u32>(HB_DIRECTION_LTR));
static_assert(Gfx::FFI::DIRECTION_RTL == static_cast<u32>(HB_DIRECTION_RTL));
static_assert(Gfx::FFI::SCRIPT_COMMON == static_cast<u32>(HB_SCRIPT_COMMON));
static_assert(Gfx::FFI::SCRIPT_INHERITED == static_cast<u32>(HB_SCRIPT_INHERITED));
static_assert(Gfx::FFI::SCRIPT_LATIN == static_cast<u32>(HB_SCRIPT_LATIN));
static_assert(Gfx::FFI::SCRIPT_ARABIC == static_cast<u32>(HB_SCRIPT_ARABIC));
static_assert(Gfx::FFI::SCRIPT_HANIFI_ROHINGYA == static_cast<u32>(HB_SCRIPT_HANIFI_ROHINGYA));
static_assert(Gfx::FFI::SCRIPT_MANDAIC == static_cast<u32>(HB_SCRIPT_MANDAIC));
static_assert(Gfx::FFI::SCRIPT_MONGOLIAN == static_cast<u32>(HB_SCRIPT_MONGOLIAN));
static_assert(Gfx::FFI::SCRIPT_NKO == static_cast<u32>(HB_SCRIPT_NKO));
static_assert(Gfx::FFI::SCRIPT_PHAGS_PA == static_cast<u32>(HB_SCRIPT_PHAGS_PA));
static_assert(Gfx::FFI::SCRIPT_SYRIAC == static_cast<u32>(HB_SCRIPT_SYRIAC));
static_assert(Gfx::FFI::GLYPH_FLAG_UNSAFE_TO_BREAK == static_cast<u32>(HB_GLYPH_FLAG_UNSAFE_TO_BREAK));
static_assert(Gfx::FFI::GLYPH_FLAG_DEFINED == static_cast<u32>(HB_GLYPH_FLAG_DEFINED));
static_assert(Gfx::FFI::FEATURE_GLOBAL_END == HB_FEATURE_GLOBAL_END);
