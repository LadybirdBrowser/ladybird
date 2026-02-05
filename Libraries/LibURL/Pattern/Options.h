/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#options
struct Options {
    // https://urlpattern.spec.whatwg.org/#options-delimiter-code-point
    Optional<char> delimiter_code_point;

    // https://urlpattern.spec.whatwg.org/#options-prefix-code-point
    Optional<char> prefix_code_point;

    // https://urlpattern.spec.whatwg.org/#options-ignore-case
    bool ignore_case { false };

    static Options default_();
    static Options hostname();
    static Options pathname();
};
;

}
