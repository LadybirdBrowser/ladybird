/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Wtf8ByteView.h>
#include <AK/Wtf8String.h>

namespace AK {

// Utf8String is a strongly owned sequence of Unicode code points encoded as UTF-8.
// The data may or may not be heap-allocated, and may or may not be reference counted.
// There is no guarantee that the underlying bytes are null-terminated.
class Utf8String : public UnicodeCodePointIterableBase<Utf8String, Utf8View, Wtf8String> {
public:
    constexpr Utf8String() = default;

    Utf8View unicode_code_point_view() const&
    {
        return Utf8View::from_string_view_unchecked(bytes_as_string_view());
    }

    // Creates a new Utf8String from a sequence of UTF-8 encoded code points.
    static ErrorOr<Utf8String> from_utf8(Utf8View);
};

template<>
struct Formatter<Utf8String> : Formatter<Wtf8String> { };

}

[[nodiscard]] ALWAYS_INLINE AK::Utf8String operator""_utf8_string(char const* cstring, size_t length)
{
    return AK::Utf8String::from_utf8(MUST(Wtf8ByteView(StringView { cstring, length }).validate_utf8())).release_value();
}
