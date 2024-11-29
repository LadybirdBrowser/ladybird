/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/DOM/Node.h>

namespace Web::Editing {

// https://w3c.github.io/editing/docs/execCommand/#record-the-values
struct RecordedNodeValue {
    GC::Ref<DOM::Node> node;
    FlyString const& command;
    Optional<String> specified_command_value;
};

// Below algorithms are specified here:
// https://w3c.github.io/editing/docs/execCommand/#assorted-common-algorithms

String canonical_space_sequence(u32 length, bool non_breaking_start, bool non_breaking_end);
void canonicalize_whitespace(GC::Ref<DOM::Node>, u32 offset, bool fix_collapsed_space = true);
void delete_the_selection(Selection::Selection const&);
GC::Ptr<DOM::Node> editing_host_of_node(GC::Ref<DOM::Node>);
void fix_disallowed_ancestors_of_node(GC::Ptr<DOM::Node>);
bool follows_a_line_break(GC::Ref<DOM::Node>);
bool is_allowed_child_of_node(Variant<GC::Ref<DOM::Node>, FlyString> child, Variant<GC::Ref<DOM::Node>, FlyString> parent);
bool is_block_boundary_point(GC::Ref<DOM::Node>, u32 offset);
bool is_block_end_point(GC::Ref<DOM::Node>, u32 offset);
bool is_block_node(GC::Ref<DOM::Node>);
bool is_block_start_point(GC::Ref<DOM::Node>, u32 offset);
bool is_collapsed_whitespace_node(GC::Ref<DOM::Node>);
bool is_editing_host(GC::Ref<DOM::Node>);
bool is_element_with_inline_contents(GC::Ref<DOM::Node>);
bool is_extraneous_line_break(GC::Ref<DOM::Node>);
bool is_in_same_editing_host(GC::Ref<DOM::Node>, GC::Ref<DOM::Node>);
bool is_inline_node(GC::Ref<DOM::Node>);
bool is_invisible_node(GC::Ref<DOM::Node>);
bool is_name_of_an_element_with_inline_contents(FlyString const&);
bool is_prohibited_paragraph_child(GC::Ref<DOM::Node>);
bool is_prohibited_paragraph_child_name(FlyString const&);
bool is_visible_node(GC::Ref<DOM::Node>);
bool is_whitespace_node(GC::Ref<DOM::Node>);
void move_node_preserving_ranges(GC::Ref<DOM::Node>, GC::Ref<DOM::Node> new_parent, u32 new_index);
void normalize_sublists_in_node(GC::Ref<DOM::Element>);
bool precedes_a_line_break(GC::Ref<DOM::Node>);
Vector<RecordedNodeValue> record_the_values_of_nodes(Vector<GC::Ref<DOM::Node>> const&);
void remove_extraneous_line_breaks_at_the_end_of_node(GC::Ref<DOM::Node>);
void remove_extraneous_line_breaks_before_node(GC::Ref<DOM::Node>);
void remove_extraneous_line_breaks_from_a_node(GC::Ref<DOM::Node>);
void remove_node_preserving_its_descendants(GC::Ref<DOM::Node>);
void restore_the_values_of_nodes(Vector<RecordedNodeValue> const&);
GC::Ref<DOM::Element> set_the_tag_name(GC::Ref<DOM::Element>, FlyString const&);
Optional<String> specified_command_value(GC::Ref<DOM::Element>, FlyString const& command);
void split_the_parent_of_nodes(Vector<GC::Ref<DOM::Node>> const&);

}
