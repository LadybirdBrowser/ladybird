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
    SimpleSelector const* simple_selectors;
    size_t simple_selector_count;
};

struct Selector {
    void const* cxx_selector;
    CompoundSelector const* compound_selectors;
    size_t compound_selector_count;
};

struct ElementAndShadowHost {
    void const* element;
    void const* shadow_host;
};

extern "C" RustSelector* rust_selector_create(Selector const*);
extern "C" void rust_selector_destroy(RustSelector*);
extern "C" bool rust_selector_matches(RustSelector const*, void const* element, u8 pseudo_element, void const* shadow_host, void* context, void const* scope);
extern "C" bool rust_selector_matches_originating_element(RustSelector const*, u8 pseudo_element, void const* element, void const* shadow_host, void* context, void const* scope);

extern "C" bool selector_ffi_matches_universal(void* context, void const* element, NamespaceType, StringView namespace_);
extern "C" bool selector_ffi_matches_tag_name(void* context, void const* element, NamespaceType, StringView namespace_, StringView name, StringView lowercase_name, u8 matching_mode);
extern "C" bool selector_ffi_matches_id(void const* element, StringView);
extern "C" bool selector_ffi_matches_class(void const* element, StringView);
extern "C" bool selector_ffi_matches_attribute(void* context, void const* element, NamespaceType, StringView namespace_, StringView name, StringView lowercase_name, AttributeMatchType, AttributeCaseType, StringView value);
extern "C" bool selector_ffi_matches_pseudo_class(void const* element, u8 pseudo_class);
extern "C" bool selector_ffi_matches_language(void const* element, StringView language);
extern "C" bool selector_ffi_matches_direction(void const* element, Direction);
extern "C" bool selector_ffi_matches_state(void const* element, StringView identifier);
extern "C" bool selector_ffi_matches_heading(void const* element, i64 const* levels, size_t level_count);

extern "C" void const* selector_ffi_parent_element(void const* element, void const* shadow_host);
extern "C" void const* selector_ffi_parent_element_in_light_tree(void const* element);
extern "C" void const* selector_ffi_previous_element_sibling(void const* element);
extern "C" void const* selector_ffi_next_element_sibling(void const* element);
extern "C" void const* selector_ffi_first_element_child(void const* element);
extern "C" void const* selector_ffi_last_element_child(void const* element);
extern "C" void const* selector_ffi_first_element_descendant(void const* element);
extern "C" void const* selector_ffi_next_element_descendant(void const* element, void const* root);
extern "C" bool selector_ffi_has_no_element_or_nonempty_text_children(void const* element);
extern "C" bool selector_ffi_has_same_type(void const* first, void const* second);
extern "C" bool selector_ffi_is_document_root(void const* element);

extern "C" ElementAndShadowHost selector_ffi_slotted_parent(void* context, void const* element);
extern "C" ElementAndShadowHost selector_ffi_part_parent(void* context, void const* element, StringView const* identifiers, size_t identifier_count, bool allow_same_shadow_root_scope, void const* shadow_host);

extern "C" void selector_ffi_note_structural_pseudo_class(void* context, void const* element, u8 pseudo_class);
extern "C" void selector_ffi_note_has_pseudo_class(void* context, void const* element);
extern "C" void selector_ffi_note_sibling_combinator(void* context, void const* element, Combinator, size_t sibling_invalidation_distance);
extern "C" void selector_ffi_note_has_sibling_combinator_anchor(void* context, void const* anchor);
extern "C" void selector_ffi_note_has_sibling_combinator_element(void* context, void const* element);
extern "C" void selector_ffi_note_has_scope_element(void* context, void const* element);
extern "C" bool selector_ffi_collects_selector_involvement_metadata(void* context);
extern "C" bool selector_ffi_inside_has_argument(void* context);
extern "C" void selector_ffi_set_inside_has_argument(void* context, bool value);
extern "C" u8 selector_ffi_has_cache_get(void* context, u64 selector_id, void const* anchor);
extern "C" void selector_ffi_has_cache_set(void* context, u64 selector_id, void const* anchor, bool result);
extern "C" bool selector_ffi_should_reject_has_argument(void* context, void const* selector, void const* anchor);

}
