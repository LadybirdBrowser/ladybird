/*
 * Copyright (c) 2019-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/File.h>
#include <ctype.h>

inline String title_casify(StringView dashy_name)
{
    auto parts = dashy_name.split_view('-');
    StringBuilder builder;
    for (auto& part : parts) {
        if (part.is_empty())
            continue;
        builder.append(toupper(part[0]));
        if (part.length() == 1)
            continue;
        builder.append(part.substring_view(1, part.length() - 1));
    }
    return MUST(builder.to_string());
}

inline String camel_casify(StringView dashy_name)
{
    auto parts = dashy_name.split_view('-');
    StringBuilder builder;
    bool first = true;
    for (auto& part : parts) {
        if (part.is_empty())
            continue;
        char ch = part[0];
        if (!first)
            ch = toupper(ch);
        else
            first = false;
        builder.append(ch);
        if (part.length() == 1)
            continue;
        builder.append(part.substring_view(1, part.length() - 1));
    }
    return MUST(builder.to_string());
}

enum class TrimLeadingUnderscores : u8 {
    No,
    Yes,
};
inline String snake_casify(StringView dashy_name, TrimLeadingUnderscores trim_leading_underscores = TrimLeadingUnderscores::No)
{
    // FIXME: We don't really need to convert dashy_name to a String first, but currently
    //        all the `replace` functions that take a StringView return ByteString.
    auto snake_case = MUST(MUST(String::from_utf8(dashy_name)).replace("-"sv, "_"sv, ReplaceMode::All));

    if (trim_leading_underscores == TrimLeadingUnderscores::Yes && snake_case.starts_with('_')) {
        return MUST(snake_case.trim("_"sv, TrimMode::Left));
    }

    return snake_case;
}

inline String make_name_acceptable_cpp(String const& name)
{
    if (name.is_one_of("float")) {
        StringBuilder builder;
        builder.append(name);
        builder.append('_');
        return MUST(builder.to_string());
    }

    return name;
}

inline ErrorOr<JsonValue> read_entire_file_as_json(StringView filename)
{
    auto file = TRY(Core::File::open(filename, Core::File::OpenMode::Read));
    auto json_size = TRY(file->size());
    auto json_data = TRY(ByteBuffer::create_uninitialized(json_size));
    TRY(file->read_until_filled(json_data.bytes()));
    return JsonValue::from_string(json_data);
}

// https://drafts.csswg.org/cssom/#css-property-to-idl-attribute
inline String css_property_to_idl_attribute(StringView property_name, bool lowercase_first = false)
{
    // The CSS property to IDL attribute algorithm for property, optionally with a lowercase first flag set, is as follows:
    // 1. Let output be the empty string.
    StringBuilder output;

    // 2. Let uppercase next be unset.
    bool uppercase_next = false;

    // 3. If the lowercase first flag is set, remove the first character from property.
    StringView actual_property_name;
    if (lowercase_first) {
        actual_property_name = property_name.substring_view(1);
    } else {
        actual_property_name = property_name;
    }

    // 4. For each character c in property:
    for (auto c : actual_property_name) {
        // 1. If c is "-" (U+002D), let uppercase next be set.
        if (c == '-') {
            uppercase_next = true;
        }
        // 2. Otherwise, if uppercase next is set, let uppercase next be unset and append c converted to ASCII uppercase to output.
        else if (uppercase_next) {
            uppercase_next = false;
            output.append(to_ascii_uppercase(c));
        }
        // 3. Otherwise, append c to output.
        else {
            output.append(c);
        }
    }

    // 5. Return output.
    return MUST(output.to_string());
}

inline StringView underlying_type_for_enum(size_t member_count)
{
    if (member_count <= NumericLimits<u8>::max())
        return "u8"sv;
    if (member_count <= NumericLimits<u16>::max())
        return "u16"sv;
    if (member_count <= NumericLimits<u32>::max())
        return "u32"sv;
    return "u64"sv;
}
