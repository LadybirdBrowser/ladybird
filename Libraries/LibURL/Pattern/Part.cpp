/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Pattern/Component.h>
#include <LibURL/Pattern/Part.h>

namespace URL::Pattern {

Part::Part(Type type, String value, Modifier modifier)
    : type(type)
    , value(move(value))
    , modifier(modifier)
{
}

Part::Part(Type type, String value, Modifier modifier, String name, String prefix, String suffix)
    : type(type)
    , value(move(value))
    , modifier(modifier)
    , name(move(name))
    , prefix(move(prefix))
    , suffix(move(suffix))
{
}

StringView Part::type_to_string(Part::Type type)
{
    switch (type) {
    case Type::FixedText:
        return "FixedText"sv;
    case Type::Regexp:
        return "Regexp"sv;
    case Type::SegmentWildcard:
        return "SegmentWildcard"sv;
    case Type::FullWildcard:
        return "FullWildcard"sv;
    }

    VERIFY_NOT_REACHED();
}

// https://urlpattern.spec.whatwg.org/#convert-a-modifier-to-a-string
StringView Part::convert_modifier_to_string(Part::Modifier modifier)
{
    // 1. If modifier is "zero-or-more", then return "*".
    if (modifier == Modifier::ZeroOrMore)
        return "*"sv;

    // 2. If modifier is "optional", then return "?".
    if (modifier == Modifier::Optional)
        return "?"sv;

    // 3. If modifier is "one-or-more", then return "+".
    if (modifier == Modifier::OneOrMore)
        return "+"sv;

    // 4. Return the empty string.
    return ""sv;
}

}
