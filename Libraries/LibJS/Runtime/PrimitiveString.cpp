/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/PrimitiveString.h>

namespace JS {

using namespace EmbeddingABI;

static GC::Ref<PrimitiveString> primitive_string_from_abi(JSPrimitiveString* string)
{
    VERIFY(string);
    return cell_ref_from_abi<PrimitiveString>(string);
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, Utf16String const& string)
{
    return primitive_string_from_abi(js_string_create_from_owned_utf16_string(vm_to_abi(vm), owned_utf16_string_to_abi(string)));
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, Utf16View const& string)
{
    return primitive_string_from_abi(js_string_create_from_utf16_view(vm_to_abi(vm), utf16_view_to_abi(string)));
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, Utf16FlyString const& string)
{
    return primitive_string_from_abi(js_string_create_from_utf16_fly_string(vm_to_abi(vm), string.raw_identity()));
}

GC::Ref<PrimitiveString> PrimitiveString::create_from_unsigned_integer(VM& vm, u64 number)
{
    return primitive_string_from_abi(js_string_create_from_unsigned_integer(vm_to_abi(vm), number));
}

Utf16String PrimitiveString::utf16_string() const
{
    return owned_utf16_string_from_abi(js_string_utf16_string(primitive_string_to_abi(*this)));
}

Utf16View PrimitiveString::utf16_string_view() const
{
    return utf16_view_from_abi(js_string_utf16_view(primitive_string_to_abi(*this)));
}

size_t PrimitiveString::length_in_utf16_code_units() const
{
    return js_string_length_in_code_units(primitive_string_to_abi(*this));
}

bool PrimitiveString::operator==(PrimitiveString const& other) const
{
    return js_string_equals(primitive_string_to_abi(*this), primitive_string_to_abi(other));
}

}
