/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <AK/StringBuilder.h>
#include <AK/Types.h>
#include <AK/Vector.h>

namespace AK {

ErrorOr<ByteBuffer> decode_hex(StringView input)
{
    if ((input.length() % 2) != 0)
        return Error::from_string_literal("Hex string was not an even length");

    auto output = TRY(ByteBuffer::create_zeroed(input.length() / 2));

    for (size_t i = 0; i < input.length() / 2; ++i) {
        auto const c1 = decode_hex_digit(input[i * 2]);
        if (c1 >= 16)
            return Error::from_string_literal("Hex string contains invalid digit");

        auto const c2 = decode_hex_digit(input[i * 2 + 1]);
        if (c2 >= 16)
            return Error::from_string_literal("Hex string contains invalid digit");

        output[i] = (c1 << 4) + c2;
    }

    return { move(output) };
}

ByteString encode_hex(ReadonlyBytes const input)
{
    StringBuilder output(input.size() * 2);

    for (auto ch : input)
        output.appendff("{:02x}", ch);

    return output.to_byte_string();
}

}
