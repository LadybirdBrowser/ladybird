/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/BigInt.h>

namespace JS {

using namespace EmbeddingABI;

// Magnitudes cross the ABI as 32-bit words, least significant first, which is the order of the words of
// Crypto::UnsignedBigInteger.
GC::Ref<BigInt> BigInt::create(VM& vm, Crypto::SignedBigInteger big_integer)
{
    auto magnitude_words = big_integer.unsigned_value().words();
    auto* bigint = js_bigint_create_from_magnitude(vm_to_abi(vm), big_integer.is_negative(), magnitude_words.data(), magnitude_words.size());
    VERIFY(bigint);
    return cell_ref_from_abi<BigInt>(bigint);
}

Crypto::SignedBigInteger BigInt::big_integer() const
{
    auto* bigint = bigint_to_abi(*this);
    Vector<u32> magnitude_words;
    magnitude_words.resize(js_bigint_magnitude_word_count(bigint));
    js_bigint_copy_magnitude_words(bigint, magnitude_words.data(), magnitude_words.size());
    return Crypto::SignedBigInteger { Crypto::UnsignedBigInteger { magnitude_words }, js_bigint_is_negative(bigint) };
}

// The decimal digits with the "n" of a BigInt literal, as the C++ runtime writes them.
Utf16String BigInt::to_utf16_string() const
{
    auto digits = owned_utf16_string_from_abi(js_bigint_to_string(bigint_to_abi(*this), 10));
    return Utf16String::formatted("{}n", digits);
}

}
