/*
 * Copyright (c) 2024-2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Editing {

// https://w3c.github.io/editing/docs/execCommand/#record-the-values
struct RecordedNodeValue {
    GC::Ref<DOM::Node> node;
    FlyString command;
    Optional<String> specified_command_value;
};

// https://w3c.github.io/editing/docs/execCommand/#record-current-states-and-values
struct RecordedOverride {
    FlyString command;
    Variant<String, bool> value;
};

// https://w3c.github.io/editing/docs/execCommand/#selection's-list-state
enum class SelectionsListState : u8 {
    Ol,
    Ul,
    Mixed,
    MixedOl,
    MixedUl,
    None,
};

// https://w3c.github.io/editing/docs/execCommand/#justify-the-selection
enum class JustifyAlignment : u8 {
    Center,
    Justify,
    Left,
    Right,
};

using Selection::Selection;

// Below algorithms are specified here:
// https://w3c.github.io/editing/docs/execCommand/#assorted-common-algorithms

GC::Ptr<DOM::Range> active_range(DOM::Document const&);
JustifyAlignment alignment_value_of_node(GC::Ptr<DOM::Node>);
void autolink(DOM::BoundaryPoint);
GC::Ref<DOM::Range> block_extend_a_range(GC::Ref<DOM::Range>);
GC::Ptr<DOM::Node> block_node_of_node(GC::Ref<DOM::Node>);
String canonical_space_sequence(u32 length, bool non_breaking_start, bool non_breaking_end);
void canonicalize_whitespace(DOM::BoundaryPoint, bool fix_collapsed_space = true);
Vector<GC::Ref<DOM::Node>> clear_the_value(FlyString const&, GC::Ref<DOM::Element>);
void delete_the_selection(Selection&, bool block_merging = true, bool strip_wrappers = true,
    Selection::Direction direction = Selection::Direction::Forwards);
GC::Ptr<DOM::Node> editing_host_of_node(GC::Ref<DOM::Node>);
Optional<String> effective_command_value(GC::Ptr<DOM::Node>, FlyString const& command);
DOM::BoundaryPoint first_equivalent_point(DOM::BoundaryPoint);
void fix_disallowed_ancestors_of_node(GC::Ref<DOM::Node>);
bool follows_a_line_break(GC::Ref<DOM::Node>);
void force_the_value(GC::Ref<DOM::Node>, FlyString const&, Optional<String>);
void indent(Vector<GC::Ref<DOM::Node>>);
bool is_allowed_child_of_node(Variant<GC::Ref<DOM::Node>, FlyString> child, Variant<GC::Ref<DOM::Node>, FlyString> parent);
bool is_block_boundary_point(DOM::BoundaryPoint);
bool is_block_end_point(DOM::BoundaryPoint);
bool is_block_node(GC::Ref<DOM::Node>);
bool is_block_start_point(DOM::BoundaryPoint);
bool is_collapsed_block_prop(GC::Ref<DOM::Node>);
bool is_collapsed_line_break(GC::Ref<DOM::Node>);
bool is_collapsed_whitespace_node(GC::Ref<DOM::Node>);
bool is_effectively_contained_in_range(GC::Ref<DOM::Node>, GC::Ref<DOM::Range>);
bool is_element_with_inline_contents(GC::Ref<DOM::Node>);
bool is_extraneous_line_break(GC::Ref<DOM::Node>);
bool is_formattable_block_name(FlyString const&);
bool is_formattable_node(GC::Ref<DOM::Node>);
bool is_in_same_editing_host(GC::Ref<DOM::Node>, GC::Ref<DOM::Node>);
bool is_indentation_element(GC::Ref<DOM::Node>);
bool is_inline_node(GC::Ref<DOM::Node>);
bool is_invisible_node(GC::Ref<DOM::Node>);
bool is_modifiable_element(GC::Ref<DOM::Node>);
bool is_name_of_an_element_with_inline_contents(FlyString const&);
bool is_non_list_single_line_container(GC::Ref<DOM::Node>);
bool is_prohibited_paragraph_child(GC::Ref<DOM::Node>);
bool is_prohibited_paragraph_child_name(FlyString const&);
bool is_remove_format_candidate(GC::Ref<DOM::Node>);
bool is_simple_indentation_element(GC::Ref<DOM::Node>);
bool is_simple_modifiable_element(GC::Ref<DOM::Node>);
bool is_single_line_container(GC::Ref<DOM::Node>);
bool is_visible_node(GC::Ref<DOM::Node>);
bool is_whitespace_node(GC::Ref<DOM::Node>);
void justify_the_selection(DOM::Document&, JustifyAlignment);
DOM::BoundaryPoint last_equivalent_point(DOM::BoundaryPoint);
String legacy_font_size(int);
void move_node_preserving_ranges(GC::Ref<DOM::Node>, GC::Ref<DOM::Node> new_parent, u32 new_index);
Optional<DOM::BoundaryPoint> next_equivalent_point(DOM::BoundaryPoint);
void normalize_sublists_in_node(GC::Ref<DOM::Node>);
void outdent(GC::Ref<DOM::Node>);
bool precedes_a_line_break(GC::Ref<DOM::Node>);
Optional<DOM::BoundaryPoint> previous_equivalent_point(DOM::BoundaryPoint);
void push_down_values(FlyString const&, GC::Ref<DOM::Node>, Optional<String>);
Vector<RecordedOverride> record_current_overrides(DOM::Document const&);
Vector<RecordedOverride> record_current_states_and_values(DOM::Document const&);
Vector<RecordedNodeValue> record_the_values_of_nodes(Vector<GC::Ref<DOM::Node>> const&);
void remove_extraneous_line_breaks_at_the_end_of_node(GC::Ref<DOM::Node>);
void remove_extraneous_line_breaks_before_node(GC::Ref<DOM::Node>);
void remove_extraneous_line_breaks_from_a_node(GC::Ref<DOM::Node>);
void remove_node_preserving_its_descendants(GC::Ref<DOM::Node>);
void reorder_modifiable_descendants(GC::Ref<DOM::Node>, FlyString const&, Optional<String>);
void restore_states_and_values(DOM::Document&, Vector<RecordedOverride> const&);
void restore_the_values_of_nodes(Vector<RecordedNodeValue> const&);
SelectionsListState selections_list_state(DOM::Document const&);
void set_the_selections_value(DOM::Document&, FlyString const&, Optional<String>);
GC::Ref<DOM::Element> set_the_tag_name(GC::Ref<DOM::Element>, FlyString const&);
Optional<String> specified_command_value(GC::Ref<DOM::Element>, FlyString const& command);
void split_the_parent_of_nodes(Vector<GC::Ref<DOM::Node>> const&);
void toggle_lists(DOM::Document&, FlyString const&);
bool values_are_equivalent(FlyString const&, Optional<String>, Optional<String>);
bool values_are_loosely_equivalent(FlyString const&, Optional<String>, Optional<String>);
GC::Ptr<DOM::Node> wrap(Vector<GC::Ref<DOM::Node>>, Function<bool(GC::Ref<DOM::Node>)> sibling_criteria, Function<GC::Ptr<DOM::Node>()> new_parent_instructions);

// Utility methods:

GC::Ptr<DOM::Node> first_formattable_node_effectively_contained(GC::Ptr<DOM::Range>);
CSSPixels font_size_to_pixel_size(StringView);
void for_each_node_effectively_contained_in_range(GC::Ptr<DOM::Range>, Function<TraversalDecision(GC::Ref<DOM::Node>)>);
bool has_visible_children(GC::Ref<DOM::Node>);
bool is_heading(FlyString const&);
String justify_alignment_to_string(JustifyAlignment);
Array<StringView, 7> named_font_sizes();
Optional<NonnullRefPtr<CSS::CSSStyleValue const>> property_in_style_attribute(GC::Ref<DOM::Element>, CSS::PropertyID);
Optional<CSS::Display> resolved_display(GC::Ref<DOM::Node>);
Optional<CSS::Keyword> resolved_keyword(GC::Ref<DOM::Node>, CSS::PropertyID);
Optional<NonnullRefPtr<CSS::CSSStyleValue const>> resolved_value(GC::Ref<DOM::Node>, CSS::PropertyID);
void take_the_action_for_command(DOM::Document&, FlyString const&, String const&);
bool value_contains_keyword(CSS::CSSStyleValue const&, CSS::Keyword);

}
