/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <LibGfx/Font/FontCatalog.h>
#include <LibGfx/Font/Typeface.h>

namespace Gfx {

// A face of an installed font file.
struct SystemFontFile {
    String path;
    u32 ttc_index { 0 };
    FontFileFormat format { FontFileFormat::OpenType };

    bool operator==(SystemFontFile const&) const = default;
};

// An installed font that the client opens by its PostScript name, for faces whose data the platform does not load back.
struct PlatformFontName {
    String postscript_name;
};

using SystemFontMatch = Variant<SystemFontFile, PlatformFontName>;

// Matches installed fonts by the rules of the platform. Each face that a match gives is one that Typeface loads.
namespace SystemFontMatcher {

// The face for a family and a style, if the family is installed.
Optional<SystemFontMatch> match_family_style(StringView family, u16 weight, u16 width, u8 slope);

// The face for a code point that the fonts of a page do not have.
Optional<SystemFontMatch> match_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji);

// The installed family for a generic family, such as 'sans-serif'.
Optional<FlyString> resolve_generic_family(StringView family, u16 weight, u8 slope);

ErrorOr<NonnullRefPtr<Typeface>> load(SystemFontMatch const&);

// The face of an installed font file at its canonical path, if Typeface loads it.
Optional<SystemFontFile> loadable_file(StringView path, u32 ttc_index);

}

}
