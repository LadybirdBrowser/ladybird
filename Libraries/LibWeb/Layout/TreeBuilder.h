/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>

namespace Web::Layout {

class TreeBuilder {
public:
    TreeBuilder();

    GC::Ptr<Layout::Node> build(DOM::Node&);

private:
    struct Context {
        bool has_svg_root = false;
        bool layout_top_layer = false;
        bool layout_svg_mask_or_clip_path = false;
    };

    i32 calculate_list_item_index(DOM::Node&);

    void update_layout_tree_before_children(DOM::Node&, GC::Ref<Layout::Node>, Context&, bool element_has_content_visibility_hidden);
    void update_layout_tree_after_children(DOM::Node&, GC::Ref<Layout::Node>, Context&, bool element_has_content_visibility_hidden);
    void wrap_in_button_layout_tree_if_needed(DOM::Node&, GC::Ref<Layout::Node>);
    enum class MustCreateSubtree {
        No,
        Yes,
    };
    void update_layout_tree(DOM::Node&, Context&, MustCreateSubtree);

    void push_parent(Layout::NodeWithStyle& node) { m_ancestor_stack.append(node); }
    void pop_parent() { m_ancestor_stack.take_last(); }

    template<CSS::DisplayInternal, typename Callback>
    void for_each_in_tree_with_internal_display(NodeWithStyle& root, Callback);

    template<CSS::DisplayInside, typename Callback>
    void for_each_in_tree_with_inside_display(NodeWithStyle& root, Callback);

    void fixup_tables(NodeWithStyle& root);
    void remove_irrelevant_boxes(NodeWithStyle& root);
    void generate_missing_child_wrappers(NodeWithStyle& root);
    Vector<GC::Root<Box>> generate_missing_parents(NodeWithStyle& root);
    void missing_cells_fixup(Vector<GC::Root<Box>> const&);

    enum class AppendOrPrepend {
        Append,
        Prepend,
    };
    void insert_node_into_inline_or_block_ancestor(Layout::Node&, CSS::Display, AppendOrPrepend);
    void create_pseudo_element_if_needed(DOM::Element&, CSS::PseudoElement, AppendOrPrepend);
    void restructure_block_node_in_inline_parent(NodeWithStyleAndBoxModelMetrics&);

    GC::Ptr<Layout::Node> m_layout_root;
    Vector<GC::Ref<Layout::NodeWithStyle>> m_ancestor_stack;

    u32 m_quote_nesting_level { 0 };
};

}
