/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Pattern/Options.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#default-options
Options Options::default_()
{
    // The default options is an options struct with delimiter code point set to the empty string and prefix code point set to the empty string.
    return {
        .delimiter_code_point = {},
        .prefix_code_point = {},
    };
}

// https://urlpattern.spec.whatwg.org/#hostname-options
Options Options::hostname()
{
    // The hostname options is an options struct with delimiter code point set "." and prefix code point set to the empty string.
    return {
        .delimiter_code_point = '.',
        .prefix_code_point = {},
    };
}

// https://urlpattern.spec.whatwg.org/#pathname-options
Options Options::pathname()
{
    // The pathname options is an options struct with delimiter code point set "/" and prefix code point set to "/".
    return {
        .delimiter_code_point = '/',
        .prefix_code_point = '/',
    };
}

}
