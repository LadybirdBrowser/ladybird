/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibWeb/HTML/SupportedImageTypes.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web::HTML {

bool is_supported_image_type(Utf16View type)
{
    type = type.trim(u" \t\n\r\f"sv);
    if (type.is_empty())
        return true;
    auto mime_type = MimeSniff::MimeType::parse(type);
    if (!mime_type.has_value())
        return false;
    auto const& essence = mime_type->essence();
    return essence == "image/avif"sv
        || essence == "image/bmp"sv
        || essence == "image/gif"sv
        || essence == "image/vnd.microsoft.icon"sv
        || essence == "image/x-icon"sv
        || essence == "image/jpeg"sv
        || essence == "image/jpg"sv
        || essence == "image/pjpeg"sv
        || essence == "image/jxl"sv
        || essence == "image/png"sv
        || essence == "image/apng"sv
        || essence == "image/x-png"sv
        || essence == "image/webp"sv
        || essence == "image/svg+xml"sv;
}

}
