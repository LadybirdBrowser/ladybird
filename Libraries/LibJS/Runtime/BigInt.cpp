/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/BigInt.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<BigInt> BigInt::create(VM& vm, bool is_negative, ReadonlySpan<u32> magnitude_words)
{
    auto* bigint = js_bigint_create_from_magnitude(vm_to_abi(vm), is_negative, magnitude_words.data(), magnitude_words.size());
    VERIFY(bigint);
    return cell_ref_from_abi<BigInt>(bigint);
}

GC::Ref<BigInt> BigInt::create_from_i64(VM& vm, i64 value)
{
    // NB: The magnitude of i64's minimum is 2^63, which fits in a u64.
    auto magnitude = value < 0 ? static_cast<u64>(0) - static_cast<u64>(value) : static_cast<u64>(value);
    u32 const words[] = { static_cast<u32>(magnitude), static_cast<u32>(magnitude >> 32) };
    return create(vm, value < 0, words);
}

GC::Ref<BigInt> BigInt::create_from_u64(VM& vm, u64 value)
{
    u32 const words[] = { static_cast<u32>(value), static_cast<u32>(value >> 32) };
    return create(vm, false, words);
}

GC::Ptr<BigInt> BigInt::create_from_string(VM& vm, Utf16View const& string)
{
    return cell_from_abi<BigInt>(js_bigint_create_from_string(vm_to_abi(vm), utf16_view_to_abi(string)));
}

bool BigInt::is_negative() const
{
    return js_bigint_is_negative(bigint_to_abi(*this));
}

Vector<u32> BigInt::magnitude_words() const
{
    auto* bigint = bigint_to_abi(*this);
    Vector<u32> magnitude_words;
    magnitude_words.resize(js_bigint_magnitude_word_count(bigint));
    js_bigint_copy_magnitude_words(bigint, magnitude_words.data(), magnitude_words.size());
    return magnitude_words;
}

String BigInt::to_string() const
{
    return owned_utf16_string_from_abi(js_bigint_to_string(bigint_to_abi(*this), 10)).to_utf8();
}

// The decimal digits with the "n" of a BigInt literal.
Utf16String BigInt::to_utf16_string() const
{
    auto digits = owned_utf16_string_from_abi(js_bigint_to_string(bigint_to_abi(*this), 10));
    return Utf16String::formatted("{}n", digits);
}

}
