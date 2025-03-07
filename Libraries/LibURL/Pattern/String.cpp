/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibURL/Pattern/String.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#escape-a-pattern-string
String escape_a_pattern_string(String const& input)
{
    // 1. Assert: input is an ASCII string.
    VERIFY(all_of(input.code_points(), is_ascii));

    // 2. Let result be the empty string.
    StringBuilder result;

    // 3. Let index be 0.
    // 4. While index is less than input’s length:
    for (auto c : input.bytes_as_string_view()) {
        // 1. Let c be input[index].
        // 2. Increment index by 1.

        // 3. If c is one of:
        //     * U+002B (+);
        //     * U+002A (*);
        //     * U+003F (?);
        //     * U+003A (:);
        //     * U+007B ({);
        //     * U+007D (});
        //     * U+0028 (();
        //     * U+0029 ()); or
        //     * U+005C (\),
        //    then append U+005C (\) to the end of result.
        if ("+*?:{}()\\"sv.contains(c))
            result.append('\\');

        // 4. Append c to the end of result.
        result.append(c);
    }

    // 5. Return result.
    return result.to_string_without_validation();
}

}
