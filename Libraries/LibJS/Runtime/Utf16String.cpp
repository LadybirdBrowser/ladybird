/*
 * Copyright (c) 2021-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibJS/Runtime/Utf16String.h>
#include <LibJS/Runtime/VM.h>

namespace JS {
namespace Detail {

static NonnullRefPtr<Utf16StringImpl> the_empty_utf16_string()
{
    static NonnullRefPtr<Utf16StringImpl> empty_string = Utf16StringImpl::create();
    return empty_string;
}

Utf16StringImpl::Utf16StringImpl(Utf16Data string)
    : m_string(move(string))
{
}

NonnullRefPtr<Utf16StringImpl> Utf16StringImpl::create()
{
    return adopt_ref(*new Utf16StringImpl);
}

NonnullRefPtr<Utf16StringImpl> Utf16StringImpl::create(Utf16Data string)
{
    return adopt_ref(*new Utf16StringImpl(move(string)));
}

NonnullRefPtr<Utf16StringImpl> Utf16StringImpl::create(StringView string)
{
    return create(MUST(utf8_to_utf16(string)));
}

NonnullRefPtr<Utf16StringImpl> Utf16StringImpl::create(Utf16View const& view)
{
    Utf16Data string;
    string.ensure_capacity(view.length_in_code_units());
    string.unchecked_append(view.data(), view.length_in_code_units());
    return create(move(string));
}

Utf16Data const& Utf16StringImpl::string() const
{
    return m_string;
}

Utf16View Utf16StringImpl::view() const
{
    return Utf16View { m_string };
}

u32 Utf16StringImpl::compute_hash() const
{
    if (m_string.is_empty())
        return 0;
    return string_hash((char const*)m_string.data(), m_string.size() * sizeof(u16));
}

}

Utf16String Utf16String::create()
{
    return Utf16String { Detail::the_empty_utf16_string() };
}

Utf16String Utf16String::create(Utf16Data string)
{
    return Utf16String { Detail::Utf16StringImpl::create(move(string)) };
}

Utf16String Utf16String::create(StringView string)
{
    return Utf16String { Detail::Utf16StringImpl::create(string) };
}

Utf16String Utf16String::create(Utf16View const& string)
{
    return Utf16String { Detail::Utf16StringImpl::create(string) };
}

Utf16String::Utf16String(NonnullRefPtr<Detail::Utf16StringImpl> string)
    : m_string(move(string))
{
}

Utf16Data const& Utf16String::string() const
{
    return m_string->string();
}

Utf16View Utf16String::view() const
{
    return m_string->view();
}

Utf16View Utf16String::substring_view(size_t code_unit_offset, size_t code_unit_length) const
{
    return view().substring_view(code_unit_offset, code_unit_length);
}

Utf16View Utf16String::substring_view(size_t code_unit_offset) const
{
    return view().substring_view(code_unit_offset);
}

String Utf16String::to_utf8() const
{
    return MUST(view().to_utf8(Utf16View::AllowInvalidCodeUnits::Yes));
}

ByteString Utf16String::to_byte_string() const
{
    return MUST(view().to_byte_string(Utf16View::AllowInvalidCodeUnits::Yes));
}

u16 Utf16String::code_unit_at(size_t index) const
{
    return view().code_unit_at(index);
}

size_t Utf16String::length_in_code_units() const
{
    return view().length_in_code_units();
}

bool Utf16String::is_empty() const
{
    return view().is_empty();
}

}
