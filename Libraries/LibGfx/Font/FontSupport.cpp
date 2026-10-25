/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/FontSupport.h>

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

}
