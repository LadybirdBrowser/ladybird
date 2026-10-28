/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

struct ConsoleInputCompletion {
    // The names that complete the input, in the order a console lists them.
    Vector<String> matches;

    // The end of the input that each match replaces: the start of the name being typed.
    String match_prop;
};

// Completes the name that a console input ends with: a global of the realm, or a property of the object that a chain
// of names and periods before it leads to. An input that ends any other way, such as inside a string, has nothing.
WEB_API Optional<ConsoleInputCompletion> complete_console_input(JS::Realm&, StringView input);

}
