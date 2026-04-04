/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// https://urlpattern.spec.whatwg.org/#part
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Part {
    // https://urlpattern.spec.whatwg.org/#part-type
    // A part has an associated type, a string, which must be set upon creation.
    pub r#type: Type,

    // https://urlpattern.spec.whatwg.org/#part-value
    // A part has an associated value, a string, which must be set upon creation.
    pub value: String,

    // https://urlpattern.spec.whatwg.org/#part-modifier
    // A part has an associated modifier a string, which must be set upon creation.
    pub modifier: Modifier,

    // https://urlpattern.spec.whatwg.org/#part-name
    // A part has an associated name, a string, initially the empty string.
    pub name: String,

    // https://urlpattern.spec.whatwg.org/#part-prefix
    // A part has an associated prefix, a string, initially the empty string.
    pub prefix: String,

    // https://urlpattern.spec.whatwg.org/#part-suffix
    // A part has an associated suffix, a string, initially the empty string.
    pub suffix: String,
}

// https://urlpattern.spec.whatwg.org/#part-type
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Type {
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
}

// https://urlpattern.spec.whatwg.org/#part-modifier
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Modifier {
    // The part does not have a modifier.
    None,

    // The part has an optional modifier indicated by the U+003F (?) code point.
    Optional,

    // The part has a "zero or more" modifier indicated by the U+002A (*) code point.
    ZeroOrMore,

    // The part has a "one or more" modifier indicated by the U+002B (+) code point.
    OneOrMore,
}

impl Part {
    pub fn new(r#type: Type, value: String, modifier: Modifier) -> Self {
        Self {
            r#type,
            value,
            modifier,
            name: String::new(),
            prefix: String::new(),
            suffix: String::new(),
        }
    }

    pub fn new_with_name(
        r#type: Type,
        value: String,
        modifier: Modifier,
        name: String,
        prefix: String,
        suffix: String,
    ) -> Self {
        Self {
            r#type,
            value,
            modifier,
            name,
            prefix,
            suffix,
        }
    }

    pub fn type_to_string(r#type: Type) -> &'static str {
        match r#type {
            Type::FixedText => "FixedText",
            Type::Regexp => "Regexp",
            Type::SegmentWildcard => "SegmentWildcard",
            Type::FullWildcard => "FullWildcard",
        }
    }

    // https://urlpattern.spec.whatwg.org/#convert-a-modifier-to-a-string
    pub fn convert_modifier_to_string(modifier: Modifier) -> &'static str {
        // 1. If modifier is "zero-or-more", then return "*".
        if modifier == Modifier::ZeroOrMore {
            return "*";
        }

        // 2. If modifier is "optional", then return "?".
        if modifier == Modifier::Optional {
            return "?";
        }

        // 3. If modifier is "one-or-more", then return "+".
        if modifier == Modifier::OneOrMore {
            return "+";
        }

        // 4. Return the empty string.
        ""
    }
}
