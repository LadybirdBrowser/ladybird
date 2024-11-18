/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8String.h>

namespace AK {

ErrorOr<Utf8String> Utf8String::from_utf8(Utf8View view)
{
    Utf8String result;
    auto bytes = view.underlying_bytes();
    TRY(result.replace_with_new_string(bytes.size(), [&](Bytes buffer) {
        bytes.copy_to(buffer);
        return ErrorOr<void> {};
    }));
    return result;
}

}
