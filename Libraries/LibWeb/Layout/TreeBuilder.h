/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
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
    TreeBuilder() = default;
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
    struct PrincipalNodeFrameStorage;
    static void clear_stale_layout_nodes_for_assigned_slottables(HTML::HTMLSlotElement&);
    static TraversalDecision clear_stale_layout_and_paint_node(DOM::Node&, DOM::Node const* cleared_subtree_root = nullptr);

    RustFFI::FfiDomTreeBuilderCallbacks make_ffi_dom_tree_builder_callbacks();
    RustFFI::FfiPseudoTreeBuilderCallbacks make_ffi_pseudo_tree_builder_callbacks();

    void insert_node_into_inline_or_block_ancestor(Layout::NodeWithStyle&, Layout::Node&, CSS::Display, AppendOrPrepend);
    static NonnullRefPtr<ListItemMarkerBox> create_and_attach_list_item_marker(ListItemBox&, DOM::Element&, NonnullRefPtr<CSS::ComputedValues const> marker_style);
    RefPtr<NodeWithStyle> create_pseudo_element_if_needed(void* rust_state, DOM::Element&, CSS::PseudoElement, Optional<AppendOrPrepend>);
    static void create_first_letter_wrapper_if_needed(DOM::Element&, Layout::BlockContainer&);

    RefPtr<Layout::Node> m_layout_root;
    OwnPtr<PrincipalNodeFrameStorage> m_principal_frames;

    // The root of the in-place subtree replacement currently being built, if any.
    Layout::Node* m_current_rebuild_root { nullptr };
    Vector<Layout::Node*> m_rebuilt_subtree_roots;
    bool m_layout_tree_update_escaped_rebuild_roots { false };
};

}
