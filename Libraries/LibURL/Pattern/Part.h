/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/String.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#part
struct Part {
    // https://urlpattern.spec.whatwg.org/#part-type
    enum class Type {
        // The part represents a simple fixed text string.
        FixedText,

        // The part represents a matching group with a custom regular expression.
        Regexp,

        // The part represents a matching group that matches code points up to the next separator code point. This is
        // typically used for a named group like ":foo" that does not have a custom regular expression.
        SegmentWildcard,

        // The part represents a matching group that greedily matches all code points. This is typically used for
        // the "*" wildcard matching group.
        FullWildcard,
    };

    // https://urlpattern.spec.whatwg.org/#part-modifier
    enum class Modifier {
        // The part does not have a modifier.
        None,

        // The part has an optional modifier indicated by the U+003F (?) code point.
        Optional,

        // The part has a "zero or more" modifier indicated by the U+002A (*) code point.
        ZeroOrMore,

        // The part has a "one or more" modifier indicated by the U+002B (+) code point.
        OneOrMore,
    };

    static StringView convert_modifier_to_string(Modifier);
    static StringView type_to_string(Type);

    Part(Type, String value, Modifier);
    Part(Type, String value, Modifier, String name, String prefix, String suffix);

    // https://urlpattern.spec.whatwg.org/#part-type
    // A part has an associated type, a string, which must be set upon creation.
    Type type {};

    // https://urlpattern.spec.whatwg.org/#part-value
    // A part has an associated value, a string, which must be set upon creation.
    String value;

    // https://urlpattern.spec.whatwg.org/#part-modifier
    // A part has an associated modifier a string, which must be set upon creation.
    Modifier modifier;

    // https://urlpattern.spec.whatwg.org/#part-name
    // A part has an associated name, a string, initially the empty string.
    String name;

    // https://urlpattern.spec.whatwg.org/#part-prefix
    // A part has an associated prefix, a string, initially the empty string.
    String prefix;

    // https://urlpattern.spec.whatwg.org/#part-suffix
    // A part has an associated suffix, a string, initially the empty string.
    String suffix;
};

}
