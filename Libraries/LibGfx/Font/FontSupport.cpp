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
    case FontFormat::EmbeddedOpenType:
        return false;
    case FontFormat::OpenType:
        return true;
    case FontFormat::SVG:
        return false;
    case FontFormat::TrueType:
        return true;
    case FontFormat::TrueTypeCollection:
        return true;
    case FontFormat::WOFF:
        return true;
    case FontFormat::WOFF2:
        return true;
    }

    return false;
}

bool font_tech_is_supported(FontTech const font_tech)
{
    // https://drafts.csswg.org/css-fonts-4/#font-tech-definitions
    // FIXME: Determine these automatically.
    switch (font_tech) {
    case FontTech::FeaturesOpentype:
        // GSUB and GPOS, supported by HarfBuzz
        return true;
    case FontTech::FeaturesAat:
        // morx and kerx, supported by HarfBuzz
        return true;
    case FontTech::FeaturesGraphite:
        // Silf, Glat , Gloc , Feat and Sill. HarfBuzz may or may not be built with support for it.
#if HB_HAS_GRAPHITE
        return true;
#else
        return false;
#endif
    case FontTech::Variations:
        // avar, cvar, fvar, gvar, HVAR, MVAR, STAT, and VVAR, supported by HarfBuzz
        return true;
    case FontTech::ColorColrv0:
    case FontTech::ColorColrv1:
        // COLR, supported by HarfBuzz
        return true;
    case FontTech::ColorSvg:
        // SVG, supported by HarfBuzz
        return true;
    case FontTech::ColorSbix:
        // sbix, supported by HarfBuzz
        return true;
    case FontTech::ColorCbdt:
        // CBDT, supported by HarfBuzz
        return true;
    case FontTech::Palettes:
        // CPAL, supported by HarfBuzz
        return true;
    case FontTech::Incremental:
        // Incremental Font Transfer: https://w3c.github.io/IFT/Overview.html
        return false;
    // https://drafts.csswg.org/css-fonts-5/#font-tech-definitions
    case FontTech::Avar2:
        // avar version 2, supported by HarfBuzz
        return true;
    }
    return false;
}

}
