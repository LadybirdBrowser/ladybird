/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/parsing.html#parser-scripting-mode
enum class ParserScriptingMode : u8 {
    // Scripts are processed when inserted, respecting async and defer attributes and blocking the parser when encountering a classic script.
    Normal,

    // Scripts are disabled, and the noscript element can represent fallback content.
    Disabled,

    // Scripts are enabled, however they are marked as already started, essentially preventing them from executing.
    // This is the default mode of the HTML fragment parsing algorithm.
    Inert,

    // Scripts are executed as soon as they are inserted into the document as part of a the HTML fragment parsing
    // algorithm, ignoring async and defer attributes. This mode is used by createContextualFragment().
    Fragment,
};

}
