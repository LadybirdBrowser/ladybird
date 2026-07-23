/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>

namespace Web::Layout {

struct LayoutTreeBuildResult {
    RefPtr<Layout::Node> root;
    Vector<Layout::Node*> rebuilt_subtree_roots;
    bool layout_tree_update_escaped_rebuild_roots { false };
};

LayoutTreeBuildResult build_layout_tree(DOM::Node&);
void detach_top_layer_element_layout_subtree(DOM::Element&);

class LayoutTreeBuilderAccess {
    friend class LayoutTreeBuildBridge;

private:
    static void clear_synthetic_pseudo_element_layout_nodes(DOM::Element&);
    static void detach_layout_node(DOM::Node&);
    static void register_svg_resource_reference(SVG::SVGElement&, DOM::Element&);
    static void set_synthetic_pseudo_element_node(DOM::Element&, CSS::PseudoElement, Layout::NodeWithStyle*);
};

}
