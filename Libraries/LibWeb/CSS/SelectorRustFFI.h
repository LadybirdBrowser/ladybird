/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::CSS::SelectorFFI {

struct RustSelector;

enum class SimpleSelectorType : u8 {
    Universal,
    TagName,
    Id,
    Class,
    Attribute,
    PseudoClass,
    PseudoElement,
    Nesting,
    Invalid,
};

enum class Combinator : u8 {
    None,
    ImmediateChild,
    Descendant,
    NextSibling,
    SubsequentSibling,
    Column,
    PseudoElement,
};

enum class NamespaceType : u8 {
    Default,
    None,
    Any,
    Named,
};

enum class AttributeMatchType : u8 {
    HasAttribute,
    ExactValue,
    ContainsWord,
    ContainsString,
    StartsWithSegment,
    StartsWithString,
    EndsWithString,
};

enum class AttributeCaseType : u8 {
    Default,
    Sensitive,
    Insensitive,
};

enum class Direction : u8 {
    None,
    LeftToRight,
    RightToLeft,
    Other,
};

enum class PseudoElementValueType : u8 {
    None,
    CompoundSelector,
    Identifiers,
    TransitionName,
};

struct StringView {
    u16 const* data;
    size_t length;
};

struct SimpleSelector {
    SimpleSelectorType type;

    NamespaceType namespace_type;
    StringView namespace_;
    StringView name;
    StringView lowercase_name;

    AttributeMatchType attribute_match_type;
    AttributeCaseType attribute_case_type;
    StringView attribute_value;

    u8 pseudo_class;
    i32 an_plus_b_step_size;
    i32 an_plus_b_offset;
    RustSelector const* const* argument_selectors;
    size_t argument_selector_count;
    StringView const* languages;
    size_t language_count;
    Direction direction;
    StringView identifier;
    i64 const* levels;
    size_t level_count;

    u8 pseudo_element;
    PseudoElementValueType pseudo_element_value_type;
    RustSelector const* pseudo_element_selector;
    StringView const* pseudo_element_identifiers;
    size_t pseudo_element_identifier_count;
    bool transition_name_is_universal;
    StringView transition_name;
};

struct CompoundSelector {
    Combinator combinator;
    bool is_implicit_universal_anchor;
    SimpleSelector const* simple_selectors;
    size_t simple_selector_count;
};

struct Selector {
    CompoundSelector const* compound_selectors;
    size_t compound_selector_count;
};

extern "C" RustSelector* rust_selector_create(Selector const*);
extern "C" void rust_selector_destroy(RustSelector*);

}
