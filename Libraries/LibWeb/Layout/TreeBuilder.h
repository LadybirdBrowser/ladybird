/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibWeb/Forward.h>

namespace Web::Layout {

class TreeBuilder {
public:
    TreeBuilder();

    RefPtr<Layout::Node> build(DOM::Node&);

    enum class AppendOrPrepend {
        Append,
        Prepend,
    };

    // Confinement report for the build: the roots of the subtrees that were rebuilt in place,
    // and whether anything mutated the tree outside those subtrees. A caller that wants to
    // re-lay out only the rebuilt subtrees must check that nothing escaped them.
    Vector<Layout::Node*> const& rebuilt_subtree_roots() const { return m_rebuilt_subtree_roots; }
    bool layout_tree_update_escaped_rebuild_roots() const { return m_layout_tree_update_escaped_rebuild_roots; }

    void note_tree_restructuring_at(Layout::Node const&);

    static void detach_top_layer_element_layout_subtree(DOM::Element&);

private:
    struct Context {
        bool has_svg_root = false;
        bool layout_top_layer = false;
        bool layout_svg_mask_or_clip_path = false;
        bool layout_svg_pattern = false;
    };

    i32 calculate_list_item_index(DOM::Node&);

    void update_layout_tree_before_children(DOM::Node&, Layout::Node&, Context&, bool element_has_content_visibility_hidden);
    void update_layout_tree_after_children(DOM::Node&, Layout::Node&, Context&, bool element_has_content_visibility_hidden);
    void wrap_in_button_layout_tree_if_needed(DOM::Node&, Layout::Node&);
    enum class MustCreateSubtree {
        No,
        Yes,
    };
    void update_layout_tree(DOM::Node&, Context&, MustCreateSubtree);
    void update_layout_tree_for_display_contents(DOM::Element&, Context&, MustCreateSubtree, bool should_create_layout_node);
    void update_layout_tree_for_svg_switch_children(SVG::SVGSwitchElement&, Context&, MustCreateSubtree);
    static TraversalDecision clear_stale_layout_and_paint_node(DOM::Node&, DOM::Node const* cleared_subtree_root = nullptr);

    void push_parent(Layout::NodeWithStyle& node) { m_ancestor_stack.append(&node); }
    void pop_parent() { m_ancestor_stack.take_last(); }

    template<CSS::DisplayInternal, typename Callback>
    void for_each_in_tree_with_internal_display(NodeWithStyle& root, Callback);

    template<CSS::DisplayInside, typename Callback>
    void for_each_in_tree_with_inside_display(NodeWithStyle& root, Callback);

    void fixup_tables(NodeWithStyle& root);
    void remove_irrelevant_boxes(NodeWithStyle& root);
    void generate_missing_child_wrappers(NodeWithStyle& root);
    Vector<NonnullRefPtr<Box>> generate_missing_parents(NodeWithStyle& root);
    void missing_cells_fixup(Vector<NonnullRefPtr<Box>> const&);

    void insert_node_into_inline_or_block_ancestor(Layout::Node&, CSS::Display, AppendOrPrepend);
    RefPtr<NodeWithStyle> create_pseudo_element_if_needed(DOM::Element&, CSS::PseudoElement, Optional<AppendOrPrepend>);
    RefPtr<NodeWithStyle> create_content_replacement_if_needed(DOM::Element&, NonnullRefPtr<CSS::ComputedValues const>) const;
    static void create_first_letter_wrapper_if_needed(DOM::Element&, Layout::BlockContainer&);

    RefPtr<Layout::Node> m_layout_root;
    Vector<Layout::NodeWithStyle*> m_ancestor_stack;

    // The root of the in-place subtree replacement currently being built, if any.
    Layout::Node* m_current_rebuild_root { nullptr };
    Vector<Layout::Node*> m_rebuilt_subtree_roots;
    bool m_layout_tree_update_escaped_rebuild_roots { false };

    u32 m_quote_nesting_level { 0 };
};

}
