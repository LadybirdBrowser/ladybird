/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibWeb/Infra/ByteSequences.h>

namespace Web::Infra {

// https://infra.spec.whatwg.org/#byte-lowercase
void byte_lowercase(ByteBuffer& bytes)
{
    // To byte-lowercase a byte sequence, increase each byte it contains, in the range 0x41 (A) to 0x5A (Z), inclusive, by 0x20.
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = to_ascii_lowercase(bytes[i]);
}

// https://infra.spec.whatwg.org/#byte-uppercase
void byte_uppercase(ByteBuffer& bytes)
{
    // To byte-uppercase a byte sequence, subtract each byte it contains, in the range 0x61 (a) to 0x7A (z), inclusive, by 0x20.
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = to_ascii_uppercase(bytes[i]);
}

// https://infra.spec.whatwg.org/#byte-sequence-starts-with
bool is_prefix_of(ReadonlyBytes potential_prefix, ReadonlyBytes input)
{
    // "input starts with potentialPrefix" can be used as a synonym for "potentialPrefix is a prefix of input".
    return input.starts_with(potential_prefix);
}

// https://infra.spec.whatwg.org/#byte-less-than
bool is_byte_less_than(ReadonlyBytes a, ReadonlyBytes b)
{
    // 1. If b is a prefix of a, then return false.
    if (is_prefix_of(b, a))
        return false;

    // 2. If a is a prefix of b, then return true.
    if (is_prefix_of(a, b))
        return true;

    // 3. Let n be the smallest index such that the nth byte of a is different from the nth byte of b.
    //    (There has to be such an index, since neither byte sequence is a prefix of the other.)
    // 4. If the nth byte of a is less than the nth byte of b, then return true.
    // 5. Return false.
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i])
            return a[i] < b[i];
    }
    VERIFY_NOT_REACHED();
}

}
