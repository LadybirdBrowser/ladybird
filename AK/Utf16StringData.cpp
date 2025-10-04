/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Stream.h>
#include <AK/TypedTransfer.h>
#include <AK/Utf16StringData.h>
#include <AK/Utf32View.h>
#include <AK/Utf8View.h>

#include <simdutf.h>

namespace AK::Detail {

// Due to internal optimizations, we have an explicit maximum string length of 2**63 - 1.
#define VERIFY_UTF16_LENGTH(length) VERIFY(length >> Detail::UTF16_FLAG == 0);

NonnullRefPtr<Utf16StringData> Utf16StringData::create_uninitialized(StorageType storage_type, size_t code_unit_length)
{
    auto allocation_size = storage_type == Utf16StringData::StorageType::ASCII
        ? sizeof(Utf16StringData) + (sizeof(char) * code_unit_length)
        : sizeof(Utf16StringData) + (sizeof(char16_t) * code_unit_length);

    void* slot = malloc(allocation_size);
    VERIFY(slot);

    return adopt_ref(*new (slot) Utf16StringData(storage_type, code_unit_length));
}

template<typename ViewType>
NonnullRefPtr<Utf16StringData> Utf16StringData::create_from_code_point_iterable(ViewType const& view)
{
    size_t code_unit_length = 0;
    size_t code_point_length = 0;

    for (auto code_point : view) {
        code_unit_length += UnicodeUtils::code_unit_length_for_code_point(code_point);
        ++code_point_length;
    }

    VERIFY_UTF16_LENGTH(code_unit_length);

    auto string = create_uninitialized(StorageType::UTF16, code_unit_length);
    string->m_length_in_code_points = code_point_length;

    size_t code_unit_index = 0;

    for (auto code_point : view) {
        (void)UnicodeUtils::code_point_to_utf16(code_point, [&](auto code_unit) {
            string->m_utf16_data[code_unit_index++] = code_unit;
        });
    }

    return string;
}

NonnullRefPtr<Utf16StringData> Utf16StringData::from_ascii(ReadonlyBytes ascii_string)
{
    VERIFY_UTF16_LENGTH(ascii_string.size());
    auto string = create_uninitialized(StorageType::ASCII, ascii_string.size());
    TypedTransfer<char>::copy(string->m_ascii_data, reinterpret_cast<char const*>(ascii_string.data()), ascii_string.size());
    return string;
}

NonnullRefPtr<Utf16StringData> Utf16StringData::from_utf8(StringView utf8_string, AllowASCIIStorage allow_ascii_storage)
{
    RefPtr<Utf16StringData> string;

    if (allow_ascii_storage == AllowASCIIStorage::Yes && utf8_string.is_ascii()) {
        VERIFY_UTF16_LENGTH(utf8_string.length());

        string = create_uninitialized(StorageType::ASCII, utf8_string.length());
        TypedTransfer<char>::copy(string->m_ascii_data, utf8_string.characters_without_null_termination(), utf8_string.length());
    } else if (Utf8View view { utf8_string }; view.validate(AllowLonelySurrogates::No)) {
        auto code_unit_length = simdutf::utf16_length_from_utf8(utf8_string.characters_without_null_termination(), utf8_string.length());
        VERIFY_UTF16_LENGTH(code_unit_length);

        string = create_uninitialized(StorageType::UTF16, code_unit_length);

        auto result = simdutf::convert_utf8_to_utf16(utf8_string.characters_without_null_termination(), utf8_string.length(), string->m_utf16_data);
        VERIFY(result == code_unit_length);
    } else {
        string = create_from_code_point_iterable(view);
    }

    return string.release_nonnull();
}

NonnullRefPtr<Utf16StringData> Utf16StringData::from_utf16(Utf16View const& utf16_string)
{
    VERIFY_UTF16_LENGTH(utf16_string.length_in_code_units());
    RefPtr<Utf16StringData> string;

    if (utf16_string.has_ascii_storage()) {
        string = create_uninitialized(StorageType::ASCII, utf16_string.length_in_code_units());
        TypedTransfer<char>::copy(string->m_ascii_data, utf16_string.ascii_span().data(), utf16_string.length_in_code_units());
    } else if (utf16_string.is_ascii()) {
        string = create_uninitialized(StorageType::ASCII, utf16_string.length_in_code_units());

        auto result = simdutf::convert_utf16_to_utf8(utf16_string.utf16_span().data(), utf16_string.length_in_code_units(), string->m_ascii_data);
        VERIFY(result == utf16_string.length_in_code_units());
    } else {
        string = create_uninitialized(StorageType::UTF16, utf16_string.length_in_code_units());
        TypedTransfer<char16_t>::copy(string->m_utf16_data, utf16_string.utf16_span().data(), utf16_string.length_in_code_units());

        string->m_length_in_code_points = utf16_string.m_length_in_code_points;
    }

    return string.release_nonnull();
}

NonnullRefPtr<Utf16StringData> Utf16StringData::from_utf32(Utf32View const& utf32_string)
{
    RefPtr<Utf16StringData> string;

    auto const* utf32_data = reinterpret_cast<char32_t const*>(utf32_string.code_points());
    auto utf32_length = utf32_string.length();

    if (utf32_string.is_ascii()) {
        VERIFY_UTF16_LENGTH(utf32_length);

        string = create_uninitialized(StorageType::ASCII, utf32_length);

        auto result = simdutf::convert_utf32_to_utf8(utf32_data, utf32_length, string->m_ascii_data);
        VERIFY(result == utf32_length);
    } else if (simdutf::validate_utf32(utf32_data, utf32_length)) {
        auto code_unit_length = simdutf::utf16_length_from_utf32(utf32_data, utf32_length);
        VERIFY_UTF16_LENGTH(code_unit_length);

        string = create_uninitialized(StorageType::UTF16, code_unit_length);
        string->m_length_in_code_points = utf32_length;

        auto result = simdutf::convert_utf32_to_utf16(utf32_data, utf32_length, string->m_utf16_data);
        VERIFY(result == code_unit_length);
    } else {
        string = create_from_code_point_iterable(utf32_string);
    }

    return string.release_nonnull();
}

NonnullRefPtr<Utf16StringData> Utf16StringData::from_string_builder(StringBuilder& builder)
{
    auto view = builder.utf16_string_view();

    auto code_unit_length = view.length_in_code_units();
    VERIFY_UTF16_LENGTH(code_unit_length);

    RefPtr<Utf16StringData> string;

    if (auto buffer = builder.leak_buffer_for_string_construction(Badge<Utf16StringData> {}); buffer.has_value()) {
        auto storage_type = view.has_ascii_storage() ? StorageType::ASCII : StorageType::UTF16;
        string = adopt_ref(*new (buffer->buffer.data()) Utf16StringData { storage_type, code_unit_length });
    } else {
        if (view.has_ascii_storage()) {
            string = create_uninitialized(StorageType::ASCII, code_unit_length);
            TypedTransfer<char>::copy(string->m_ascii_data, view.ascii_span().data(), code_unit_length);
        } else {
            string = create_uninitialized(StorageType::UTF16, code_unit_length);
            TypedTransfer<char16_t>::copy(string->m_utf16_data, view.utf16_span().data(), code_unit_length);

            string->m_length_in_code_points = view.m_length_in_code_points;
        }
    }

    return string.release_nonnull();
}

ErrorOr<NonnullRefPtr<Utf16StringData>> Utf16StringData::from_ipc_stream(Stream& stream, size_t length_in_code_units, bool is_ascii)
{
    RefPtr<Utf16StringData> string;

    if (is_ascii) {
        string = create_uninitialized(StorageType::ASCII, length_in_code_units);

        Bytes bytes { string->m_ascii_data, length_in_code_units };
        TRY(stream.read_until_filled(bytes));

        if (!string->ascii_view().is_ascii())
            return Error::from_string_literal("Stream contains invalid ASCII data");
    } else {
        string = create_uninitialized(StorageType::UTF16, length_in_code_units);

        Bytes bytes { reinterpret_cast<u8*>(string->m_utf16_data), length_in_code_units * sizeof(char16_t) };
        TRY(stream.read_until_filled(bytes));
    }

    return string.release_nonnull();
}

NonnullRefPtr<Utf16StringData> Utf16StringData::to_well_formed(Utf16View const& utf16_string)
{
    VERIFY(!utf16_string.has_ascii_storage());

    auto string = create_uninitialized(StorageType::UTF16, utf16_string.length_in_code_units());
    simdutf::to_well_formed_utf16(utf16_string.utf16_span().data(), utf16_string.length_in_code_units(), string->m_utf16_data);

    return string;
}

size_t Utf16StringData::calculate_code_point_length() const
{
    ASSERT(!has_ascii_storage());

    if (simdutf::validate_utf16(m_utf16_data, length_in_code_units()))
        return simdutf::count_utf16(m_utf16_data, length_in_code_units());

    size_t code_points = 0;
    for ([[maybe_unused]] auto code_point : utf16_view())
        ++code_points;
    return code_points;
}

}
