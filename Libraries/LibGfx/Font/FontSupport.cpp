/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/FontSupport.h>

#include <harfbuzz/hb.h>

namespace Gfx {

bool font_format_is_supported(FontFormat const format)
{
    // FIXME: Determine these automatically.
    switch (format) {
    case FontFormat::OpenType:
    case FontFormat::TrueType:
    case FontFormat::TrueTypeCollection:
    case FontFormat::WOFF:
    case FontFormat::WOFF2:
        return true;
    default:
        return false;
    }
}

bool font_tech_is_supported(FontTech const font_tech)
{
    // https://drafts.csswg.org/css-fonts-4/#font-tech-definitions
    // https://drafts.csswg.org/css-fonts-5/#font-tech-definitions

    // FIXME: Determine these automatically.
    // FIXME: FontTech::Variations does not actually seem to work and causes issues with the font weight on https://ladybird.org

    switch (font_tech) {
    case FontTech::FeaturesOpentype: // GSUB and GPOS, supported by HarfBuzz
    case FontTech::FeaturesAat: // morx and kerx, supported by HarfBuzz
    case FontTech::ColorColrv0:
    case FontTech::ColorColrv1: // COLR, supported by HarfBuzz
    case FontTech::ColorSvg: // SVG, supported by HarfBuzz
    case FontTech::ColorSbix: // sbix, supported by HarfBuzz
    case FontTech::ColorCbdt: // CBDT, supported by HarfBuzz
    case FontTech::Avar2: // avar version 2, supported by HarfBuzz
    case FontTech::Palettes: // CPAL, supported by HarfBuzz
        return true;
    default:
        return false;
    }
}

}
