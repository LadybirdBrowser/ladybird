/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibURL/Pattern/Init.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#typedefdef-urlpatterninput
using Input = Variant<String, Init>;

// https://urlpattern.spec.whatwg.org/#dictdef-urlpatternoptions
struct Options {
    bool ignore_case { false };
};

// https://urlpattern.spec.whatwg.org/#dictdef-urlpatterncomponentresult
struct ComponentResult {
    String input;
    OrderedHashMap<String, Variant<String, Empty>> groups;
};

// https://urlpattern.spec.whatwg.org/#dictdef-urlpatternresult
struct Result {
    Vector<Input> inputs;

    ComponentResult protocol;
    ComponentResult username;
    ComponentResult password;
    ComponentResult hostname;
    ComponentResult port;
    ComponentResult pathname;
    ComponentResult search;
    ComponentResult hash;
};

}
