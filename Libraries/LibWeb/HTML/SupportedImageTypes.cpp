/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/SupportedImageTypes.h>

namespace Web::HTML {

bool is_supported_image_type(StringView type)
{
    if (type.is_empty())
        return true;
    if (!type.starts_with("image/"sv, CaseSensitivity::CaseInsensitive))
        return false;
    return type.equals_ignoring_ascii_case("image/avif"sv)
        || type.equals_ignoring_ascii_case("image/bmp"sv)
        || type.equals_ignoring_ascii_case("image/gif"sv)
        || type.equals_ignoring_ascii_case("image/vnd.microsoft.icon"sv)
        || type.equals_ignoring_ascii_case("image/x-icon"sv)
        || type.equals_ignoring_ascii_case("image/jpeg"sv)
        || type.equals_ignoring_ascii_case("image/jpg"sv)
        || type.equals_ignoring_ascii_case("image/pjpeg"sv)
        || type.equals_ignoring_ascii_case("image/jxl"sv)
        || type.equals_ignoring_ascii_case("image/png"sv)
        || type.equals_ignoring_ascii_case("image/apng"sv)
        || type.equals_ignoring_ascii_case("image/x-png"sv)
        || type.equals_ignoring_ascii_case("image/tiff"sv)
        || type.equals_ignoring_ascii_case("image/tinyvg"sv)
        || type.equals_ignoring_ascii_case("image/webp"sv)
        || type.equals_ignoring_ascii_case("image/svg+xml"sv);
}

}
