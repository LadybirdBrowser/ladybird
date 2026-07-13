/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibWeb/HTML/SupportedImageTypes.h>

namespace Web::HTML {

bool is_supported_image_type(Utf16View type)
{
    if (type.is_empty())
        return true;
    if (type.length_in_code_units() < 6 || !type.substring_view(0, 6).equals_ignoring_ascii_case(u"image/"sv))
        return false;
    return type.equals_ignoring_ascii_case(u"image/avif"sv)
        || type.equals_ignoring_ascii_case(u"image/bmp"sv)
        || type.equals_ignoring_ascii_case(u"image/gif"sv)
        || type.equals_ignoring_ascii_case(u"image/vnd.microsoft.icon"sv)
        || type.equals_ignoring_ascii_case(u"image/x-icon"sv)
        || type.equals_ignoring_ascii_case(u"image/jpeg"sv)
        || type.equals_ignoring_ascii_case(u"image/jpg"sv)
        || type.equals_ignoring_ascii_case(u"image/pjpeg"sv)
        || type.equals_ignoring_ascii_case(u"image/jxl"sv)
        || type.equals_ignoring_ascii_case(u"image/png"sv)
        || type.equals_ignoring_ascii_case(u"image/apng"sv)
        || type.equals_ignoring_ascii_case(u"image/x-png"sv)
        || type.equals_ignoring_ascii_case(u"image/webp"sv)
        || type.equals_ignoring_ascii_case(u"image/svg+xml"sv);
}

}
