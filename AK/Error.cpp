/*
 * Copyright (c) 2023, Liav A. <liavalb@hotmail.co.il>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024-2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/String.h>

#ifdef AK_OS_WINDOWS
#    include <AK/ByteString.h>
#    include <AK/HashMap.h>
#    include <windows.h>
// Comment to prevent clang-format from including windows.h too late
#    include <winbase.h>
#endif

namespace AK {

#ifdef AK_OS_WINDOWS
Error Error::from_windows_error(u32 windows_error)
{
    return Error(static_cast<int>(windows_error), Error::Kind::Windows);
}

// This can be used both for generic Windows errors and for winsock errors because WSAGetLastError is forwarded to GetLastError.
Error Error::from_windows_error()
{
    return from_windows_error(GetLastError());
}
#endif

template<typename R>
R Error::format_impl() const
{
    static_assert(IsSame<R, ErrorOr<AK::String>>);

    if (auto* ptr = m_data.get_pointer<ErrnoCode>())
        return AK::String::formatted("Errno {}: {}", ptr->code, strerror(ptr->code));
    if (auto* ptr = m_data.get_pointer<Syscall>())
        return AK::String::formatted("{} failed with errno {}: {}", ptr->name, ptr->code, strerror(ptr->code));
#ifdef AK_OS_WINDOWS
    if (auto* ptr = m_data.get_pointer<WindowsError>())
        return Formatter<Error>::format_windows_error(*this);
#endif
    if (auto* ptr = m_data.get_pointer<StringView>())
        return AK::String::from_utf8(*ptr);

    auto& format_data = m_data.get<FormattedString>();
    struct FormatParams {
        alignas(TypeErasedFormatParams) u8 format_params_bits[sizeof(TypeErasedFormatParams)];
        alignas(AK::TypeErasedParameter) u8 params_bits[sizeof(AK::TypeErasedParameter) * ((64 - sizeof(StringView)) / 2)];
    } data;
    auto* params = bit_cast<AK::TypeErasedParameter*>(&data.params_bits[0]);
    size_t count = 0;
    size_t offset = 0;

    u8 aligned_local_storage[1 * KiB] {};
    size_t aligned_local_storage_offset = 0;
    auto allocate_aligned_on_local_storage = [&]<typename T>(u8 const* p) {
        auto start_offset = align_up_to(aligned_local_storage_offset, alignof(T));
        VERIFY(array_size(aligned_local_storage) >= start_offset + sizeof(T));
        __builtin_memcpy(&aligned_local_storage[start_offset], p, sizeof(T));
        aligned_local_storage_offset = start_offset + sizeof(T);
        return bit_cast<T const*>(&aligned_local_storage[start_offset]);
    };

    for (; format_data.buffer[offset] != 0;) {
        auto type = static_cast<FormattedString::Type>(format_data.buffer[offset]);
        offset += 1;
        switch (type) {
        case FormattedString::Type::Nothing:
            VERIFY_NOT_REACHED();

#define CASE(Name, T)                                                                              \
    case FormattedString::Type::Name:                                                              \
        params[count++] = AK::TypeErasedParameter {                                                \
            allocate_aligned_on_local_storage.template operator()<T>(&format_data.buffer[offset]), \
        };                                                                                         \
        offset += sizeof(T);                                                                       \
        break

            CASE(ExplicitStaticString, StringView);
            CASE(U8, u8);
            CASE(U16, u16);
            CASE(U32, u32);
            CASE(U64, u64);
            CASE(I8, i8);
            CASE(I16, i16);
            CASE(I32, i32);
            CASE(I64, i64);
            CASE(Bool, bool);

#undef CASE
        }
    }

    auto* format_params = bit_cast<TypeErasedFormatParams*>(&data.format_params_bits[0]);
    return AK::String::vformatted(format_data.format_string, *format_params);
}

template ErrorOr<String> Error::format_impl<ErrorOr<String>>() const;

}