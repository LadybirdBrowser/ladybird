/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibURL/Pattern/Init.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#typedefdef-urlpatterninput
using Input = Variant<String, Init>;

// https://urlpattern.spec.whatwg.org/#dictdef-urlpatternoptions
struct Options {
    bool ignore_case { false };
};

}
