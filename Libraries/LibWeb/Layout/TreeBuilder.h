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

namespace RustFFI {

struct FfiDomTreeBuilderCallbacks;
struct FfiPseudoTreeBuilderCallbacks;

}

struct LayoutTreeBuildResult {
    RefPtr<Layout::Node> root;
    Vector<Layout::Node*> rebuilt_subtree_roots;
    bool layout_tree_update_escaped_rebuild_roots { false };
};

LayoutTreeBuildResult build_layout_tree(DOM::Node&);
void detach_top_layer_element_layout_subtree(DOM::Element&);

class TreeBuilder {
public:
    TreeBuilder();
    ~TreeBuilder();

    TreeBuilder(TreeBuilder const&) = delete;
    TreeBuilder& operator=(TreeBuilder const&) = delete;

    LayoutTreeBuildResult build(DOM::Node&);

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

    enum class MustCreateSubtree {
        No,
        Yes,
    };
    void update_layout_tree(DOM::Node&, Context&, MustCreateSubtree);
    static void clear_stale_layout_nodes_for_assigned_slottables(HTML::HTMLSlotElement&);
    static TraversalDecision clear_stale_layout_and_paint_node(DOM::Node&, DOM::Node const* cleared_subtree_root = nullptr);

    void push_parent(Layout::NodeWithStyle&);
    void pop_parent();
    Layout::NodeWithStyle& current_parent() const;
    size_t ancestor_count() const;
    Layout::NodeWithStyle& ancestor_at(size_t) const;
    u32 quote_nesting_level() const;
    void set_quote_nesting_level(u32);
    RustFFI::FfiDomTreeBuilderCallbacks make_ffi_dom_tree_builder_callbacks();
    RustFFI::FfiPseudoTreeBuilderCallbacks make_ffi_pseudo_tree_builder_callbacks();

    void insert_node_into_inline_or_block_ancestor(Layout::Node&, CSS::Display, AppendOrPrepend);
    static NonnullRefPtr<ListItemMarkerBox> create_and_attach_list_item_marker(ListItemBox&, DOM::Element&, NonnullRefPtr<CSS::ComputedValues const> marker_style);
    RefPtr<NodeWithStyle> create_pseudo_element_if_needed(DOM::Element&, CSS::PseudoElement, Optional<AppendOrPrepend>);
    static void create_first_letter_wrapper_if_needed(DOM::Element&, Layout::BlockContainer&);

    RefPtr<Layout::Node> m_layout_root;
    void* m_rust_state { nullptr };

    // The root of the in-place subtree replacement currently being built, if any.
    Layout::Node* m_current_rebuild_root { nullptr };
    Vector<Layout::Node*> m_rebuilt_subtree_roots;
    bool m_layout_tree_update_escaped_rebuild_roots { false };
};

}
