/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/Array.h>
#include <AK/Checked.h>
#include <AK/FlyString.h>
#include <AK/Format.h>
#include <AK/MemMem.h>
#include <AK/Stream.h>
#include <AK/String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <stdlib.h>

#include <simdutf.h>

namespace AK {

String String::from_utf8_without_validation(ReadonlyBytes bytes)
{
    String result;
    MUST(result.replace_with_new_string(bytes.size(), [&](Bytes buffer) {
        bytes.copy_to(buffer);
        return ErrorOr<void> {};
    }));
    return result;
}

ErrorOr<String> String::from_utf8(StringView view)
{
    if (!Utf8View { view }.validate())
        return Error::from_string_literal("String::from_utf8: Input was not valid UTF-8");

    String result;
    TRY(result.replace_with_new_string(view.length(), [&](Bytes buffer) {
        view.bytes().copy_to(buffer);
        return ErrorOr<void> {};
    }));
    return result;
}

ErrorOr<String> String::from_utf16(Utf16View const& utf16)
{
    if (!utf16.validate())
        return Error::from_string_literal("String::from_utf16: Input was not valid UTF-16");

    String result;

    auto utf8_length = simdutf::utf8_length_from_utf16(
        reinterpret_cast<char16_t const*>(utf16.data()),
        utf16.length_in_code_units());

    TRY(result.replace_with_new_string(utf8_length, [&](Bytes buffer) -> ErrorOr<void> {
        [[maybe_unused]] auto result = simdutf::convert_utf16_to_utf8(
            reinterpret_cast<char16_t const*>(utf16.data()),
            utf16.length_in_code_units(),
            reinterpret_cast<char*>(buffer.data()));
        ASSERT(result == buffer.size());

        return {};
    }));

    return result;
}

ErrorOr<String> String::from_stream(Stream& stream, size_t byte_count)
{
    String result;
    TRY(result.replace_with_new_string(byte_count, [&](Bytes buffer) -> ErrorOr<void> {
        TRY(stream.read_until_filled(buffer));
        if (!Utf8View { StringView { buffer } }.validate())
            return Error::from_string_literal("String::from_stream: Input was not valid UTF-8");
        return {};
    }));
    return result;
}

ErrorOr<String> String::repeated(u32 code_point, size_t count)
{
    VERIFY(is_unicode(code_point));

    Array<u8, 4> code_point_as_utf8;
    size_t i = 0;

    size_t code_point_byte_length = UnicodeUtils::code_point_to_utf8(code_point, [&](auto byte) {
        code_point_as_utf8[i++] = static_cast<u8>(byte);
    });

    auto total_byte_count = code_point_byte_length * count;

    String result;
    TRY(result.replace_with_new_string(total_byte_count, [&](Bytes buffer) {
        if (code_point_byte_length == 1) {
            buffer.fill(code_point_as_utf8[0]);
        } else {
            for (i = 0; i < count; ++i)
                memcpy(buffer.data() + (i * code_point_byte_length), code_point_as_utf8.data(), code_point_byte_length);
        }
        return ErrorOr<void> {};
    }));
    return result;
}

StringView String::bytes_as_string_view() const&
{
    return StringView(bytes());
}

bool String::is_empty() const
{
    return bytes().size() == 0;
}

ErrorOr<String> String::vformatted(StringView fmtstr, TypeErasedFormatParams& params)
{
    StringBuilder builder;
    TRY(vformat(builder, fmtstr, params));
    return builder.to_string();
}

ErrorOr<Vector<String>> String::split(u32 separator, SplitBehavior split_behavior) const
{
    return split_limit(separator, 0, split_behavior);
}

ErrorOr<Vector<String>> String::split_limit(u32 separator, size_t limit, SplitBehavior split_behavior) const
{
    Vector<String> result;

    if (is_empty())
        return result;

    bool keep_empty = has_flag(split_behavior, SplitBehavior::KeepEmpty);

    size_t substring_start = 0;
    for (auto it = code_points().begin(); it != code_points().end() && (result.size() + 1) != limit; ++it) {
        u32 code_point = *it;
        if (code_point == separator) {
            size_t substring_length = code_points().iterator_offset(it) - substring_start;
            if (substring_length != 0 || keep_empty)
                TRY(result.try_append(TRY(substring_from_byte_offset_with_shared_superstring(substring_start, substring_length))));
            substring_start = code_points().iterator_offset(it) + it.underlying_code_point_length_in_bytes();
        }
    }
    size_t tail_length = code_points().byte_length() - substring_start;
    if (tail_length != 0 || keep_empty)
        TRY(result.try_append(TRY(substring_from_byte_offset_with_shared_superstring(substring_start, tail_length))));
    return result;
}

Optional<size_t> String::find_byte_offset(u32 code_point, size_t from_byte_offset) const
{
    auto code_points = this->code_points();
    if (from_byte_offset >= code_points.byte_length())
        return {};

    for (auto it = code_points.iterator_at_byte_offset(from_byte_offset); it != code_points.end(); ++it) {
        if (*it == code_point)
            return code_points.byte_offset_of(it);
    }

    return {};
}

Optional<size_t> String::find_byte_offset(StringView substring, size_t from_byte_offset) const
{
    auto view = bytes_as_string_view();
    if (from_byte_offset >= view.length())
        return {};

    auto index = memmem_optional(
        view.characters_without_null_termination() + from_byte_offset, view.length() - from_byte_offset,
        substring.characters_without_null_termination(), substring.length());

    if (index.has_value())
        return *index + from_byte_offset;
    return {};
}

bool String::operator==(FlyString const& other) const
{
    return static_cast<StringBase const&>(*this) == other.data({});
}

bool String::operator==(StringView other) const
{
    return bytes_as_string_view() == other;
}

ErrorOr<String> String::substring_from_byte_offset(size_t start, size_t byte_count) const
{
    if (!byte_count)
        return String {};
    return String::from_utf8(bytes_as_string_view().substring_view(start, byte_count));
}

ErrorOr<String> String::substring_from_byte_offset(size_t start) const
{
    VERIFY(start <= bytes_as_string_view().length());
    return substring_from_byte_offset(start, bytes_as_string_view().length() - start);
}

ErrorOr<String> String::substring_from_byte_offset_with_shared_superstring(size_t start, size_t byte_count) const
{
    return String { TRY(StringBase::substring_from_byte_offset_with_shared_superstring(start, byte_count)) };
}

ErrorOr<String> String::substring_from_byte_offset_with_shared_superstring(size_t start) const
{
    VERIFY(start <= bytes_as_string_view().length());
    return substring_from_byte_offset_with_shared_superstring(start, bytes_as_string_view().length() - start);
}

bool String::operator==(char const* c_string) const
{
    return bytes_as_string_view() == c_string;
}

u32 String::ascii_case_insensitive_hash() const
{
    return case_insensitive_string_hash(reinterpret_cast<char const*>(bytes().data()), bytes().size());
}

Utf8View String::code_points() const&
{
    return Utf8View(bytes_as_string_view());
}

ErrorOr<void> Formatter<String>::format(FormatBuilder& builder, String const& utf8_string)
{
    return Formatter<StringView>::format(builder, utf8_string.bytes_as_string_view());
}

ErrorOr<String> String::replace(StringView needle, StringView replacement, ReplaceMode replace_mode) const
{
    return StringUtils::replace(*this, needle, replacement, replace_mode);
}

ErrorOr<String> String::reverse() const
{
    // FIXME: This handles multi-byte code points, but not e.g. grapheme clusters.
    // FIXME: We could avoid allocating a temporary vector if Utf8View supports reverse iteration.
    auto code_point_length = code_points().length();

    Vector<u32> code_points;
    TRY(code_points.try_ensure_capacity(code_point_length));

    for (auto code_point : this->code_points())
        code_points.unchecked_append(code_point);

    auto builder = TRY(StringBuilder::create(code_point_length * sizeof(u32)));
    while (!code_points.is_empty())
        TRY(builder.try_append_code_point(code_points.take_last()));

    return builder.to_string();
}

ErrorOr<String> String::trim(Utf8View const& code_points_to_trim, TrimMode mode) const
{
    auto trimmed = code_points().trim(code_points_to_trim, mode);
    return String::from_utf8(trimmed.as_string());
}

ErrorOr<String> String::trim(StringView code_points_to_trim, TrimMode mode) const
{
    return trim(Utf8View { code_points_to_trim }, mode);
}

ErrorOr<String> String::trim_ascii_whitespace(TrimMode mode) const
{
    return trim(" \n\t\v\f\r"sv, mode);
}

bool String::contains(StringView needle, CaseSensitivity case_sensitivity) const
{
    return StringUtils::contains(bytes_as_string_view(), needle, case_sensitivity);
}

bool String::contains(u32 needle, CaseSensitivity case_sensitivity) const
{
    auto needle_as_string = String::from_code_point(needle);
    return contains(needle_as_string.bytes_as_string_view(), case_sensitivity);
}

bool String::starts_with(u32 code_point) const
{
    if (is_empty())
        return false;

    return *code_points().begin() == code_point;
}

bool String::starts_with_bytes(StringView bytes, CaseSensitivity case_sensitivity) const
{
    return bytes_as_string_view().starts_with(bytes, case_sensitivity);
}

bool String::ends_with(u32 code_point) const
{
    if (is_empty())
        return false;

    u32 last_code_point = 0;
    for (auto it = code_points().begin(); it != code_points().end(); ++it)
        last_code_point = *it;

    return last_code_point == code_point;
}

bool String::ends_with_bytes(StringView bytes, CaseSensitivity case_sensitivity) const
{
    return bytes_as_string_view().ends_with(bytes, case_sensitivity);
}

unsigned Traits<String>::hash(String const& string)
{
    return string.hash();
}

ByteString String::to_byte_string() const
{
    return ByteString(bytes_as_string_view());
}

ErrorOr<String> String::from_byte_string(ByteString const& byte_string)
{
    return String::from_utf8(byte_string.view());
}

bool String::equals_ignoring_ascii_case(StringView other) const
{
    return StringUtils::equals_ignoring_ascii_case(bytes_as_string_view(), other);
}

ErrorOr<String> String::repeated(String const& input, size_t count)
{
    if (Checked<u32>::multiplication_would_overflow(count, input.bytes().size()))
        return Error::from_errno(EOVERFLOW);

    String result;
    size_t input_size = input.bytes().size();
    TRY(result.replace_with_new_string(count * input_size, [&](Bytes buffer) {
        if (input_size == 1) {
            buffer.fill(input.bytes().first());
        } else {
            for (size_t i = 0; i < count; ++i)
                input.bytes().copy_to(buffer.slice(i * input_size, input_size));
        }
        return ErrorOr<void> {};
    }));

    return result;
}

}
