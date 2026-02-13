/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Debug.h"

#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibRequests/RequestClient.h>

namespace TestWeb {

ByteBuffer strip_sgr_sequences(StringView input)
{
    // Regex equivalent: /\x1b\[[0-9;]*m/
    auto const* bytes = reinterpret_cast<unsigned char const*>(input.characters_without_null_termination());
    size_t length = input.length();

    ByteBuffer output;
    output.ensure_capacity(length);

    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] == 0x1b && (i + 1) < length && bytes[i + 1] == '[') {
            size_t j = i + 2;
            while (j < length && (is_ascii_digit(bytes[j]) || bytes[j] == ';'))
                ++j;
            if (j < length && bytes[j] == 'm') {
                i = j;
                continue;
            }
        }

        output.append(static_cast<u8>(bytes[i]));
    }

    return output;
}

}
