/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Utf16View.h>
#include <LibWeb/HTML/SupportedImageTypes.h>

namespace Web::HTML {

static size_t code_unit_length(StringView string)
{
    return string.length();
}

static size_t code_unit_length(Utf16View string)
{
    return string.length_in_code_units();
}

static u32 code_unit_at(StringView string, size_t index)
{
    return string[index];
}

static u32 code_unit_at(Utf16View string, size_t index)
{
    return string.code_unit_at(index);
}

template<typename StringType>
static bool equals_ignoring_ascii_case(StringType string, StringView ascii_string)
{
    if (code_unit_length(string) != ascii_string.length())
        return false;

    for (size_t i = 0; i < code_unit_length(string); ++i) {
        if (AK::to_ascii_lowercase(code_unit_at(string, i)) != AK::to_ascii_lowercase(ascii_string[i]))
            return false;
    }

    return true;
}

template<typename StringType>
static bool starts_with_ignoring_ascii_case(StringType string, StringView ascii_prefix)
{
    if (code_unit_length(string) < ascii_prefix.length())
        return false;
    return equals_ignoring_ascii_case(string.substring_view(0, ascii_prefix.length()), ascii_prefix);
}

template<typename StringType>
static bool is_supported_image_type_impl(StringType type)
{
    if (type.is_empty())
        return true;
    if (!starts_with_ignoring_ascii_case(type, "image/"sv))
        return false;
    return equals_ignoring_ascii_case(type, "image/avif"sv)
        || equals_ignoring_ascii_case(type, "image/bmp"sv)
        || equals_ignoring_ascii_case(type, "image/gif"sv)
        || equals_ignoring_ascii_case(type, "image/vnd.microsoft.icon"sv)
        || equals_ignoring_ascii_case(type, "image/x-icon"sv)
        || equals_ignoring_ascii_case(type, "image/jpeg"sv)
        || equals_ignoring_ascii_case(type, "image/jpg"sv)
        || equals_ignoring_ascii_case(type, "image/pjpeg"sv)
        || equals_ignoring_ascii_case(type, "image/jxl"sv)
        || equals_ignoring_ascii_case(type, "image/png"sv)
        || equals_ignoring_ascii_case(type, "image/apng"sv)
        || equals_ignoring_ascii_case(type, "image/x-png"sv)
        || equals_ignoring_ascii_case(type, "image/webp"sv)
        || equals_ignoring_ascii_case(type, "image/svg+xml"sv);
}

bool is_supported_image_type(StringView type)
{
    return is_supported_image_type_impl(type);
}

bool is_supported_image_type(Utf16View type)
{
    return is_supported_image_type_impl(type);
}

}
