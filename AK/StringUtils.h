/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020, Fei Wu <f.eiwu@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/EnumBits.h>
#include <AK/Forward.h>

namespace AK {

namespace Detail {

template<Concepts::AnyString T, Concepts::AnyString U>
inline constexpr bool IsHashCompatible<T, U> = true;

}

enum class CaseSensitivity {
    CaseInsensitive,
    CaseSensitive,
};

enum class ReplaceMode {
    All,
    FirstOnly,
};

enum class TrimMode {
    Left,
    Right,
    Both
};

enum class TrimWhitespace {
    Yes,
    No,
};

enum class SplitBehavior : unsigned {
    // Neither keep empty substrings nor keep the trailing separator.
    // This is the default behavior if unspecified.
    Nothing = 0,

    // If two separators follow each other without any characters
    // in between, keep a "" in the resulting vector. (or only the
    // separator if KeepTrailingSeparator is used)
    KeepEmpty = 1,

    // Do not strip off the separator at the end of the string.
    KeepTrailingSeparator = 2,
};
AK_ENUM_BITWISE_OPERATORS(SplitBehavior);

enum class TrailingCodePointTransformation : u8 {
    // Default behaviour; Puts the first typographic letter unit of each word, if lowercase, in titlecase; the other characters in lowercase.
    Lowercase,
    // Puts the first typographic letter unit of each word, if lowercase, in titlecase; other characters are unaffected. (https://drafts.csswg.org/css-text/#valdef-text-transform-capitalize)
    PreserveExisting,
};

struct MaskSpan {
    size_t start;
    size_t length;

    bool operator==(MaskSpan const& other) const
    {
        return start == other.start && length == other.length;
    }
};

namespace StringUtils {

bool matches(StringView str, StringView mask, CaseSensitivity = CaseSensitivity::CaseInsensitive, Vector<MaskSpan>* match_spans = nullptr);
template<typename T = int>
Optional<T> convert_to_int(StringView, TrimWhitespace = TrimWhitespace::Yes);
template<typename T = unsigned>
Optional<T> convert_to_uint(StringView, TrimWhitespace = TrimWhitespace::Yes);
template<typename T = unsigned>
Optional<T> convert_to_uint_from_hex(StringView, TrimWhitespace = TrimWhitespace::Yes);
template<typename T = unsigned>
Optional<T> convert_to_uint_from_octal(StringView, TrimWhitespace = TrimWhitespace::Yes);
template<typename T>
Optional<T> convert_to_floating_point(StringView, TrimWhitespace = TrimWhitespace::Yes);
bool equals_ignoring_ascii_case(StringView, StringView);
bool ends_with(StringView a, StringView b, CaseSensitivity);
bool starts_with(StringView, StringView, CaseSensitivity);
bool contains(StringView, StringView, CaseSensitivity);
bool is_whitespace(StringView);
StringView trim(StringView string, StringView characters, TrimMode mode);
StringView trim_whitespace(StringView string, TrimMode mode);

Optional<size_t> find(StringView haystack, char needle, size_t start = 0);
Optional<size_t> find(StringView haystack, StringView needle, size_t start = 0);
Optional<size_t> find_last(StringView haystack, char needle);
Optional<size_t> find_last(StringView haystack, StringView needle);
Optional<size_t> find_last_not(StringView haystack, char needle);
Vector<size_t> find_all(StringView haystack, StringView needle);
enum class SearchDirection {
    Forward,
    Backward
};
Optional<size_t> find_any_of(StringView haystack, StringView needles, SearchDirection);

ByteString to_snakecase(StringView);
String to_titlecase(StringView);

ByteString replace(StringView, StringView needle, StringView replacement, ReplaceMode);
ErrorOr<String> replace(String const&, StringView needle, StringView replacement, ReplaceMode);

size_t count(StringView, StringView needle);
size_t count(StringView, char needle);

}

}

#if USING_AK_GLOBALLY
using AK::CaseSensitivity;
using AK::ReplaceMode;
using AK::SplitBehavior;
using AK::TrailingCodePointTransformation;
using AK::TrimMode;
using AK::TrimWhitespace;
#endif
