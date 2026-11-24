/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/ScriptAndModuleABIConversions.h>
#include <LibJS/SourceCode.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(sizeof(Utf16String) == sizeof(JSOwnedUtf16String));
static_assert(alignof(Utf16String) == alignof(JSOwnedUtf16String));

// The source code's string, which the runtime keeps in an AK::Utf16String of its own for as long as the source code
// lives.
static Utf16String const& string_in_place_from_abi(JSOwnedUtf16String const* string)
{
    return *reinterpret_cast<Utf16String const*>(string);
}

NonnullRefPtr<SourceCode const> SourceCode::create(Utf16String filename, Utf16String code)
{
    return adopt_source_code_from_abi(js_source_code_create(owned_utf16_string_to_abi(move(filename)), owned_utf16_string_to_abi(move(code))));
}

NonnullRefPtr<SourceCode const> SourceCode::create_sharing_code(Utf16String filename, SourceCode const& other)
{
    return adopt_source_code_from_abi(js_source_code_create_sharing_code(owned_utf16_string_to_abi(move(filename)), source_code_to_abi(other)));
}

Utf16String const& SourceCode::filename() const
{
    return string_in_place_from_abi(js_source_code_filename_address(source_code_to_abi(*this)));
}

Utf16String SourceCode::code() const
{
    return owned_utf16_string_from_abi(js_source_code_code(source_code_to_abi(*this)));
}

size_t SourceCode::length_in_code_units() const
{
    return js_source_code_length_in_code_units(source_code_to_abi(*this));
}

void SourceCode::ref() const
{
    js_source_code_retain(source_code_to_abi(*this));
}

void SourceCode::unref() const
{
    js_source_code_release(source_code_to_abi(*this));
}

static JSWeakSourceCode* weak_source_code_to_abi(FFI::WeakSourceCode* weak)
{
    return reinterpret_cast<JSWeakSourceCode*>(weak);
}

WeakSourceCode::WeakSourceCode(SourceCode const& source_code)
    : m_weak(reinterpret_cast<FFI::WeakSourceCode*>(js_source_code_downgrade(source_code_to_abi(source_code))))
{
}

WeakSourceCode::WeakSourceCode(WeakSourceCode&& other)
    : m_weak(exchange(other.m_weak, nullptr))
{
}

WeakSourceCode& WeakSourceCode::operator=(WeakSourceCode&& other)
{
    if (this != &other) {
        js_weak_source_code_destroy(weak_source_code_to_abi(m_weak));
        m_weak = exchange(other.m_weak, nullptr);
    }
    return *this;
}

WeakSourceCode::~WeakSourceCode()
{
    js_weak_source_code_destroy(weak_source_code_to_abi(m_weak));
}

bool WeakSourceCode::is_alive() const
{
    return m_weak && js_weak_source_code_is_alive(weak_source_code_to_abi(m_weak));
}

RefPtr<SourceCode const> WeakSourceCode::strong_ref() const
{
    if (!m_weak)
        return nullptr;
    auto const* source_code = js_weak_source_code_upgrade(weak_source_code_to_abi(m_weak));
    if (!source_code)
        return nullptr;
    return adopt_source_code_from_abi(source_code);
}

}
