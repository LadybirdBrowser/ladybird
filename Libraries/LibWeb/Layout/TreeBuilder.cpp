/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/TemporaryChange.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/Layout/FieldSetBox.h>
#include <LibWeb/Layout/ListItemBox.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/SVGClipBox.h>
#include <LibWeb/Layout/SVGMaskBox.h>
#include <LibWeb/Layout/TableGrid.h>
#include <LibWeb/Layout/TableWrapper.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/TreeBuilder.h>
#include <LibWeb/Layout/Viewport.h>

namespace Web::Layout {

TreeBuilder::TreeBuilder() = default;

static bool has_inline_or_in_flow_block_children(Layout::Node const& layout_node)
{
    for (auto child = layout_node.first_child(); child; child = child->next_sibling()) {
        if (child->is_inline() || child->is_in_flow())
            return true;
    }
    return false;
}

static bool has_in_flow_block_children(Layout::Node const& layout_node)
{
    if (layout_node.children_are_inline())
        return false;
    for (auto child = layout_node.first_child(); child; child = child->next_sibling()) {
        if (child->is_inline())
            continue;
        if (child->is_in_flow())
            return true;
    }
    return false;
}

// The insertion_parent_for_*() functions maintain the invariant that the in-flow children of
// block-level boxes must be either all block-level or all inline-level.

static Layout::Node& insertion_parent_for_inline_node(Layout::NodeWithStyle& layout_parent)
{
    auto last_child_creating_anonymous_wrapper_if_needed = [](auto& layout_parent) -> Layout::Node& {
        if (!layout_parent.last_child()
            || !layout_parent.last_child()->is_anonymous()
            || !layout_parent.last_child()->children_are_inline()
            || layout_parent.last_child()->is_generated()) {
            layout_parent.append_child(layout_parent.create_anonymous_wrapper());
        }
        return *layout_parent.last_child();
    };

    if (is<FieldSetBox>(layout_parent))
        return last_child_creating_anonymous_wrapper_if_needed(layout_parent);

    if (layout_parent.display().is_inline_outside() && layout_parent.display().is_flow_inside())
        return layout_parent;

    if (layout_parent.display().is_flex_inside()
        || layout_parent.display().is_grid_inside()
        || layout_parent.display().is_table_cell()) {
        return last_child_creating_anonymous_wrapper_if_needed(layout_parent);
    }

    if (!has_in_flow_block_children(layout_parent) || layout_parent.children_are_inline())
        return layout_parent;

    // Parent has block-level children, insert into an anonymous wrapper block (and create it first if needed)
    return last_child_creating_anonymous_wrapper_if_needed(layout_parent);
}

static Layout::Node& insertion_parent_for_block_node(Layout::NodeWithStyle& layout_parent, Layout::Node& layout_node)
{
    // Inline is fine for in-flow block children; we'll maintain the (non-)inline invariant after insertion.
    if (layout_parent.is_inline() && layout_parent.display().is_flow_inside() && !layout_node.is_out_of_flow())
        return layout_parent;

    if (!has_inline_or_in_flow_block_children(layout_parent)) {
        // Parent block has no children, insert this block into parent.
        return layout_parent;
    }

    if (layout_node.is_out_of_flow()
        && !layout_parent.display().is_flex_inside()
        && !layout_parent.display().is_grid_inside()
        && !layout_parent.last_child()->is_generated()
        && layout_parent.last_child()->is_anonymous()
        && layout_parent.last_child()->children_are_inline()) {
        // Block is out-of-flow & previous sibling was wrapped in an anonymous block.
        // Join the previous sibling inside the anonymous block.
        return *layout_parent.last_child();
    }

    if (!layout_parent.children_are_inline()) {
        // Parent block has block-level children, insert this block into parent.
        return layout_parent;
    }

    if (layout_node.is_out_of_flow()) {
        // Block is out-of-flow, it can have inline siblings if necessary.
        return layout_parent;
    }

    // Parent block has inline-level children (our siblings); wrap these siblings into an anonymous wrapper block.
    Vector<GC::Ref<Node>> children;
    for (GC::Ptr<Node> child = layout_parent.first_child(); child; child = child->next_sibling()) {
        // NOTE: We let out-of-flow children stay in the parent, to preserve tree structure.
        if (child->is_out_of_flow())
            continue;
        children.append(*child);
    }

    auto wrapper = layout_parent.create_anonymous_wrapper();
    wrapper->set_children_are_inline(true);
    for (auto child : children) {
        layout_parent.remove_child(child);
        wrapper->append_child(child);
    }

    layout_parent.set_children_are_inline(false);
    layout_parent.append_child(wrapper);

    // Then it's safe to insert this block into parent.
    return layout_parent;
}

void TreeBuilder::insert_node_into_inline_or_block_ancestor(Layout::Node& node, CSS::Display display, AppendOrPrepend mode)
{
    if (node.display().is_contents())
        return;

    // Find the nearest ancestor that can host the node.
    auto& nearest_insertion_ancestor = [&]() -> NodeWithStyle& {
        for (auto& ancestor : m_ancestor_stack.in_reverse()) {
            auto const& ancestor_display = ancestor->display();

            // Out-of-flow nodes cannot be hosted in inline flow nodes.
            if (node.is_out_of_flow() && ancestor_display.is_inline_outside() && ancestor_display.is_flow_inside())
                continue;

            if (!ancestor_display.is_contents())
                return ancestor;
        }
        VERIFY_NOT_REACHED();
    }();

    auto& insertion_point = display.is_inline_outside() ? insertion_parent_for_inline_node(nearest_insertion_ancestor)
                                                        : insertion_parent_for_block_node(nearest_insertion_ancestor, node);

    if (mode == AppendOrPrepend::Prepend)
        insertion_point.prepend_child(node);
    else
        insertion_point.append_child(node);

    if (display.is_inline_outside()) {
        // After inserting an inline-level box into a parent, mark the parent as having inline children.
        insertion_point.set_children_are_inline(true);
    } else if (node.is_in_flow()) {
        // After inserting an in-flow block-level box into a parent, mark the parent as having non-inline children.
        insertion_point.set_children_are_inline(false);
    }
}

void TreeBuilder::create_pseudo_element_if_needed(DOM::Element& element, CSS::PseudoElement pseudo_element, AppendOrPrepend mode)
{
    auto& document = element.document();

    auto pseudo_element_style = element.pseudo_element_computed_properties(pseudo_element);
    if (!pseudo_element_style)
        return;

    auto initial_quote_nesting_level = m_quote_nesting_level;
    auto [pseudo_element_content, final_quote_nesting_level] = pseudo_element_style->content(element, initial_quote_nesting_level);
    m_quote_nesting_level = final_quote_nesting_level;
    auto pseudo_element_display = pseudo_element_style->display();
    // ::before and ::after only exist if they have content. `content: normal` computes to `none` for them.
    // We also don't create them if they are `display: none`.
    if (pseudo_element_display.is_none()
        || pseudo_element_content.type == CSS::ContentData::Type::Normal
        || pseudo_element_content.type == CSS::ContentData::Type::None)
        return;

    auto pseudo_element_node = DOM::Element::create_layout_node_for_display_type(document, pseudo_element_display, *pseudo_element_style, nullptr);
    if (!pseudo_element_node)
        return;

    auto& style_computer = document.style_computer();

    // FIXME: This code actually computes style for element::marker, and shouldn't for element::pseudo::marker
    if (is<ListItemBox>(*pseudo_element_node)) {
        auto marker_style = style_computer.compute_style(element, CSS::PseudoElement::Marker);
        auto list_item_marker = document.heap().allocate<ListItemMarkerBox>(
            document,
            pseudo_element_node->computed_values().list_style_type(),
            pseudo_element_node->computed_values().list_style_position(),
            element,
            marker_style);
        static_cast<ListItemBox&>(*pseudo_element_node).set_marker(list_item_marker);
        element.set_pseudo_element_node({}, CSS::PseudoElement::Marker, list_item_marker);
        pseudo_element_node->prepend_child(*list_item_marker);
    }

    auto generated_for = CSS::to_generated_pseudo_element(pseudo_element).release_value();
    pseudo_element_node->set_generated_for(generated_for, element);
    pseudo_element_node->set_initial_quote_nesting_level(initial_quote_nesting_level);

    // FIXME: Handle images, and multiple values
    if (pseudo_element_content.type == CSS::ContentData::Type::String) {
        auto text = document.realm().create<DOM::Text>(document, pseudo_element_content.data);
        auto text_node = document.heap().allocate<Layout::TextNode>(document, *text);
        text_node->set_generated_for(generated_for, element);

        push_parent(*pseudo_element_node);
        insert_node_into_inline_or_block_ancestor(*text_node, text_node->display(), AppendOrPrepend::Append);
        pop_parent();
    } else {
        TODO();
    }

    element.set_pseudo_element_node({}, pseudo_element, pseudo_element_node);
    insert_node_into_inline_or_block_ancestor(*pseudo_element_node, pseudo_element_display, mode);
    pseudo_element_node->mutable_computed_values().set_content(pseudo_element_content);
}

// Block nodes inside inline nodes are allowed, but to maintain the invariant that either all layout children are
// inline or non-inline, we need to rearrange the tree a bit. All inline ancestors up to the node we've inserted are
// wrapped in an anonymous block, which is inserted into the nearest non-inline ancestor. We then recreate the inline
// ancestors in another anonymous block inserted after the node so we can continue adding children.
//
// Effectively, we try to turn this:
//
//     InlineNode 1
//       TextNode 1
//       InlineNode N
//         TextNode N
//         BlockContainer (node)
//
// Into this:
//
//     BlockContainer (anonymous "before")
//       InlineNode 1
//         TextNode 1
//         InlineNode N
//           TextNode N
//     BlockContainer (anonymous "middle") continuation
//       BlockContainer (node)
//     BlockContainer (anonymous "after")
//       InlineNode 1 continuation
//         InlineNode N
//
// To be able to reconstruct their relation after restructuring, layout nodes keep track of their continuation. The
// top-most inline node of the "after" wrapper points to the "middle" wrapper, which points to the top-most inline node
// of the "before" wrapper. All other inline nodes in the "after" wrapper point to their counterparts in the "before"
// wrapper, to make it easier to create the right paintables since a DOM::Node only has a single Layout::Node.
//
// Appending then continues in the "after" tree. If a new block node is then inserted, we can reuse the "middle" wrapper
// if no inline siblings exist for node or its ancestors, and leave the existing "after" wrapper alone. Otherwise, we
// create new wrappers and extend the continuation chain.
//
// Inspired by: https://webkit.org/blog/115/webcore-rendering-ii-blocks-and-inlines/
void TreeBuilder::restructure_block_node_in_inline_parent(NodeWithStyleAndBoxModelMetrics& node)
{
    // Mark parent as inline again
    auto& parent = *node.parent();
    VERIFY(!parent.children_are_inline());
    parent.set_children_are_inline(true);

    // Find nearest non-inline, content supporting ancestor that is not an anonymous block.
    auto& nearest_block_ancestor = [&] -> NodeWithStyle& {
        for (auto* ancestor = parent.parent(); ancestor; ancestor = ancestor->parent()) {
            if (!ancestor->is_inline() && !ancestor->display().is_contents() && !ancestor->is_anonymous())
                return *ancestor;
        }
        VERIFY_NOT_REACHED();
    }();
    nearest_block_ancestor.set_children_are_inline(false);

    // Find the topmost inline ancestor.
    GC::Ptr<NodeWithStyleAndBoxModelMetrics> topmost_inline_ancestor;
    for (auto* ancestor = &parent; ancestor; ancestor = ancestor->parent()) {
        if (ancestor == &nearest_block_ancestor)
            break;
        if (ancestor->is_inline())
            topmost_inline_ancestor = static_cast<NodeWithStyleAndBoxModelMetrics*>(ancestor);
    }
    VERIFY(topmost_inline_ancestor);

    // We need to host the topmost inline ancestor and its previous siblings in an anonymous "before" wrapper. If an
    // inline wrapper does not already exist, we create a new one and add it to the nearest block ancestor.
    GC::Ptr<Node> before_wrapper;
    if (auto* last_child = nearest_block_ancestor.last_child(); last_child->is_anonymous() && last_child->children_are_inline()) {
        before_wrapper = last_child;
    } else {
        before_wrapper = nearest_block_ancestor.create_anonymous_wrapper();
        before_wrapper->set_children_are_inline(true);
        nearest_block_ancestor.append_child(*before_wrapper);
    }
    if (topmost_inline_ancestor->parent() != before_wrapper.ptr()) {
        GC::Ptr<Node> inline_to_move = topmost_inline_ancestor;
        while (inline_to_move) {
            auto* next = inline_to_move->previous_sibling();
            inline_to_move->remove();
            before_wrapper->insert_before(*inline_to_move, before_wrapper->first_child());
            inline_to_move = next;
        }
    }

    // If we are part of an existing continuation and all inclusive ancestors have no previous siblings, we can reuse
    // the existing middle wrapper. Otherwiser, we create a new middle wrapper to contain the block node and add it to
    // the nearest block ancestor.
    bool needs_new_continuation = true;
    GC::Ptr<NodeWithStyleAndBoxModelMetrics> middle_wrapper;
    if (topmost_inline_ancestor->continuation_of_node()) {
        needs_new_continuation = false;
        for (GC::Ptr<Node> ancestor = node; ancestor != topmost_inline_ancestor; ancestor = ancestor->parent()) {
            if (ancestor->previous_sibling()) {
                needs_new_continuation = true;
                break;
            }
        }
        if (!needs_new_continuation)
            middle_wrapper = topmost_inline_ancestor->continuation_of_node();
    }
    if (!middle_wrapper) {
        middle_wrapper = static_cast<NodeWithStyleAndBoxModelMetrics&>(*nearest_block_ancestor.create_anonymous_wrapper());
        nearest_block_ancestor.append_child(*middle_wrapper);
        middle_wrapper->set_continuation_of_node({}, topmost_inline_ancestor);
    }

    // Move the block node to the middle wrapper.
    node.remove();
    middle_wrapper->append_child(node);

    // If we need a new continuation, recreate inline ancestors in another anonymous block so we can continue adding new
    // nodes. We don't need to do this if we are within an existing continuation and there were no previous siblings in
    // any inclusive ancestor of node in the after wrapper.
    if (needs_new_continuation) {
        auto after_wrapper = nearest_block_ancestor.create_anonymous_wrapper();
        GC::Ptr<Node> current_parent = after_wrapper;
        for (GC::Ptr<Node> inline_node = topmost_inline_ancestor;
            inline_node && is<DOM::Element>(inline_node->dom_node()); inline_node = inline_node->last_child()) {
            auto& element = static_cast<DOM::Element&>(*inline_node->dom_node());

            auto style = element.computed_properties();
            auto& new_inline_node = static_cast<NodeWithStyleAndBoxModelMetrics&>(*element.create_layout_node(*style));
            if (inline_node == topmost_inline_ancestor) {
                // The topmost inline ancestor points to the middle wrapper, which in turns points to the original node.
                new_inline_node.set_continuation_of_node({}, middle_wrapper);
                topmost_inline_ancestor = new_inline_node;
            } else {
                // We need all other inline nodes to point to their original node so we can walk the continuation chain
                // in LayoutState and create the right paintables.
                new_inline_node.set_continuation_of_node({}, static_cast<NodeWithStyleAndBoxModelMetrics&>(*inline_node));
            }

            current_parent->append_child(new_inline_node);
            current_parent = new_inline_node;

            // Replace the node in the ancestor stack with the new node.
            auto& node_with_style = static_cast<NodeWithStyle&>(*inline_node);
            if (auto stack_index = m_ancestor_stack.find_first_index(node_with_style); stack_index.has_value())
                m_ancestor_stack[stack_index.release_value()] = new_inline_node;

            // Stop recreating nodes when we've reached node's parent.
            if (inline_node == &parent)
                break;
        }

        after_wrapper->set_children_are_inline(true);
        nearest_block_ancestor.append_child(after_wrapper);
    }
}

static bool is_ignorable_whitespace(Layout::Node const& node)
{
    if (node.is_text_node() && static_cast<TextNode const&>(node).text_for_rendering().bytes_as_string_view().is_whitespace())
        return true;

    if (node.is_anonymous() && node.is_block_container() && static_cast<BlockContainer const&>(node).children_are_inline()) {
        bool contains_only_white_space = true;
        node.for_each_in_inclusive_subtree_of_type<TextNode>([&contains_only_white_space](auto& text_node) {
            if (!text_node.text_for_rendering().bytes_as_string_view().is_whitespace()) {
                contains_only_white_space = false;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        if (contains_only_white_space)
            return true;
    }

    return false;
}

void TreeBuilder::update_layout_tree(DOM::Node& dom_node, TreeBuilder::Context& context, MustCreateSubtree must_create_subtree)
{
    bool should_create_layout_node = must_create_subtree == MustCreateSubtree::Yes
        || dom_node.needs_layout_tree_update()
        || dom_node.document().needs_full_layout_tree_update()
        || (dom_node.is_document() && !dom_node.layout_node());

    if (dom_node.is_element()) {
        auto& element = static_cast<DOM::Element&>(dom_node);
        if (element.rendered_in_top_layer() && !context.layout_top_layer)
            return;
    }
    if (dom_node.is_element())
        dom_node.document().style_computer().push_ancestor(static_cast<DOM::Element const&>(dom_node));

    ScopeGuard pop_ancestor_guard = [&] {
        if (dom_node.is_element())
            dom_node.document().style_computer().pop_ancestor(static_cast<DOM::Element const&>(dom_node));
    };

    GC::Ptr<Layout::Node> old_layout_node = dom_node.layout_node();
    GC::Ptr<Layout::Node> layout_node;
    Optional<TemporaryChange<bool>> has_svg_root_change;

    ScopeGuard remove_stale_layout_node_guard = [&] {
        // If we didn't create a layout node for this DOM node,
        // go through the DOM tree and remove any old layout & paint nodes since they are now all stale.
        if (!layout_node) {
            dom_node.for_each_in_inclusive_subtree([&](auto& node) {
                node.set_needs_layout_tree_update(false);
                node.set_child_needs_layout_tree_update(false);
                node.detach_layout_node({});
                node.clear_paintable();
                if (is<DOM::Element>(node))
                    static_cast<DOM::Element&>(node).clear_pseudo_element_nodes({});
                return TraversalDecision::Continue;
            });
        }
    };

    if (dom_node.is_svg_container()) {
        has_svg_root_change.emplace(context.has_svg_root, true);
    } else if (dom_node.requires_svg_container() && !context.has_svg_root) {
        return;
    }

    auto& document = dom_node.document();
    auto& style_computer = document.style_computer();
    GC::Ptr<CSS::ComputedProperties> style;
    CSS::Display display;

    if (!should_create_layout_node) {
        if (is<DOM::Element>(dom_node)) {
            auto& element = static_cast<DOM::Element&>(dom_node);
            style = element.computed_properties();
            display = style->display();
        }
        layout_node = dom_node.layout_node();
    } else {
        if (is<DOM::Element>(dom_node)) {
            auto& element = static_cast<DOM::Element&>(dom_node);
            element.clear_pseudo_element_nodes({});
            VERIFY(!element.needs_style_update());
            style = element.computed_properties();
            element.resolve_counters(*style);
            display = style->display();
            if (display.is_none())
                return;
            // TODO: Implement changing element contents with the `content` property.
            if (context.layout_svg_mask_or_clip_path) {
                if (is<SVG::SVGMaskElement>(dom_node))
                    layout_node = document.heap().allocate<Layout::SVGMaskBox>(document, static_cast<SVG::SVGMaskElement&>(dom_node), *style);
                else if (is<SVG::SVGClipPathElement>(dom_node))
                    layout_node = document.heap().allocate<Layout::SVGClipBox>(document, static_cast<SVG::SVGClipPathElement&>(dom_node), *style);
                else
                    VERIFY_NOT_REACHED();
                // Only layout direct uses of SVG masks/clipPaths.
                context.layout_svg_mask_or_clip_path = false;
            } else {
                layout_node = element.create_layout_node(*style);
            }
        } else if (is<DOM::Document>(dom_node)) {
            style = style_computer.create_document_style();
            display = style->display();
            layout_node = document.heap().allocate<Layout::Viewport>(static_cast<DOM::Document&>(dom_node), *style);
        } else if (is<DOM::Text>(dom_node)) {
            layout_node = document.heap().allocate<Layout::TextNode>(document, static_cast<DOM::Text&>(dom_node));
            display = CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow);
        }
    }

    if (!layout_node)
        return;

    if (dom_node.is_document()) {
        m_layout_root = layout_node;
    } else if (should_create_layout_node) {
        // Decide whether to replace an existing node (partial tree update) or insert a new one appropriately.
        bool const may_replace_existing_layout_node = must_create_subtree == MustCreateSubtree::No
            && old_layout_node
            && old_layout_node->parent()
            && old_layout_node != layout_node;
        if (may_replace_existing_layout_node) {
            old_layout_node->parent()->replace_child(*layout_node, *old_layout_node);
        } else if (layout_node->is_svg_box()) {
            m_ancestor_stack.last()->append_child(*layout_node);
        } else {
            insert_node_into_inline_or_block_ancestor(*layout_node, display, AppendOrPrepend::Append);
        }
    }

    auto shadow_root = is<DOM::Element>(dom_node) ? as<DOM::Element>(dom_node).shadow_root() : nullptr;

    auto element_has_content_visibility_hidden = [&dom_node]() {
        if (is<DOM::Element>(dom_node)) {
            auto& element = static_cast<DOM::Element&>(dom_node);
            return element.computed_properties()->content_visibility() == CSS::ContentVisibility::Hidden;
        }
        return false;
    }();

    auto prior_quote_nesting_level = m_quote_nesting_level;

    if (should_create_layout_node)
        update_layout_tree_before_children(dom_node, *layout_node, context, element_has_content_visibility_hidden);

    if (should_create_layout_node || dom_node.child_needs_layout_tree_update()) {
        if ((dom_node.has_children() || shadow_root) && layout_node->can_have_children() && !element_has_content_visibility_hidden) {
            push_parent(as<NodeWithStyle>(*layout_node));
            if (shadow_root) {
                for (auto* node = shadow_root->first_child(); node; node = node->next_sibling()) {
                    update_layout_tree(*node, context, should_create_layout_node ? MustCreateSubtree::Yes : MustCreateSubtree::No);
                }
                shadow_root->set_child_needs_layout_tree_update(false);
                shadow_root->set_needs_layout_tree_update(false);
            } else {
                // This is the same as as<DOM::ParentNode>(dom_node).for_each_child
                for (auto* node = as<DOM::ParentNode>(dom_node).first_child(); node; node = node->next_sibling())
                    update_layout_tree(*node, context, should_create_layout_node ? MustCreateSubtree::Yes : MustCreateSubtree::No);
            }

            if (dom_node.is_document()) {
                // Elements in the top layer do not lay out normally based on their position in the document; instead they
                // generate boxes as if they were siblings of the root element.
                TemporaryChange<bool> layout_mask(context.layout_top_layer, true);
                for (auto const& top_layer_element : document.top_layer_elements()) {
                    if (top_layer_element->rendered_in_top_layer())
                        update_layout_tree(top_layer_element, context, should_create_layout_node ? MustCreateSubtree::Yes : MustCreateSubtree::No);
                }
            }
            pop_parent();
        }
    }

    if (is<HTML::HTMLSlotElement>(dom_node)) {
        auto& slot_element = static_cast<HTML::HTMLSlotElement&>(dom_node);

        if (slot_element.computed_properties()->content_visibility() != CSS::ContentVisibility::Hidden) {
            auto slottables = slot_element.assigned_nodes_internal();
            push_parent(as<NodeWithStyle>(*layout_node));

            MustCreateSubtree must_create_subtree_for_slottable = must_create_subtree;
            if (slot_element.needs_layout_tree_update())
                must_create_subtree_for_slottable = MustCreateSubtree::Yes;

            for (auto const& slottable : slottables) {
                slottable.visit([&](auto& node) { update_layout_tree(node, context, must_create_subtree_for_slottable); });
            }

            pop_parent();
        }
    }

    if (should_create_layout_node) {
        update_layout_tree_after_children(dom_node, *layout_node, context, element_has_content_visibility_hidden);
        wrap_in_button_layout_tree_if_needed(dom_node, *layout_node);

        // If we completely finished inserting a block level element into an inline parent, we need to fix up the tree so
        // that we can maintain the invariant that all children are either inline or non-inline. We can't do this earlier,
        // because the restructuring adds new children after this node that become part of the ancestor stack.
        if (auto node_with_metrics = as_if<NodeWithStyleAndBoxModelMetrics>(*layout_node);
            node_with_metrics && node_with_metrics->should_create_inline_continuation())
            restructure_block_node_in_inline_parent(*node_with_metrics);
    }

    // https://www.w3.org/TR/css-contain-2/#containment-style
    // Giving an element style containment has the following effects:
    // 2. The effects of the 'content' property’s 'open-quote', 'close-quote', 'no-open-quote' and 'no-close-quote' must
    //    be scoped to the element’s sub-tree.
    if (dom_node.is_element() && (static_cast<DOM::Element&>(dom_node)).has_style_containment()) {
        m_quote_nesting_level = prior_quote_nesting_level;
    }

    dom_node.set_needs_layout_tree_update(false);
    dom_node.set_child_needs_layout_tree_update(false);
}

void TreeBuilder::wrap_in_button_layout_tree_if_needed(DOM::Node& dom_node, GC::Ref<Layout::Node> layout_node)
{
    auto is_button_layout = [&] {
        if (dom_node.is_html_button_element())
            return true;
        if (!dom_node.is_html_input_element())
            return false;
        // https://html.spec.whatwg.org/multipage/rendering.html#the-input-element-as-a-button
        // An input element whose type attribute is in the Submit Button, Reset Button, or Button state, when it generates a CSS box, is expected to depict a button and use button layout
        auto const& input_element = static_cast<HTML::HTMLInputElement const&>(dom_node);
        if (input_element.is_button())
            return true;
        return false;
    }();

    if (!is_button_layout)
        return;

    auto display = layout_node->display();

    // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
    // If the computed value of 'inline-size' is 'auto', then the used value is the fit-content inline size.
    if (is_button_layout && dom_node.layout_node()->computed_values().width().is_auto()) {
        auto& computed_values = as<NodeWithStyle>(*dom_node.layout_node()).mutable_computed_values();
        computed_values.set_width(CSS::Size::make_fit_content());
    }

    // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
    // If the element is an input element, or if it is a button element and its computed value for
    // 'display' is not 'inline-grid', 'grid', 'inline-flex', or 'flex', then the element's box has
    // a child anonymous button content box with the following behaviors:
    if (is_button_layout && !display.is_grid_inside() && !display.is_flex_inside()) {
        auto& parent = *layout_node;

        // If the box does not overflow in the vertical axis, then it is centered vertically.
        // FIXME: Only apply alignment when box overflows
        auto flex_computed_values = parent.computed_values().clone_inherited_values();
        auto& mutable_flex_computed_values = static_cast<CSS::MutableComputedValues&>(*flex_computed_values);
        mutable_flex_computed_values.set_display(CSS::Display { CSS::DisplayOutside::Block, CSS::DisplayInside::Flex });
        mutable_flex_computed_values.set_justify_content(CSS::JustifyContent::Center);
        mutable_flex_computed_values.set_flex_direction(CSS::FlexDirection::Column);
        mutable_flex_computed_values.set_height(CSS::Size::make_percentage(CSS::Percentage(100)));
        mutable_flex_computed_values.set_min_height(parent.computed_values().min_height());
        auto flex_wrapper = parent.heap().template allocate<BlockContainer>(parent.document(), nullptr, move(flex_computed_values));

        auto content_box_computed_values = parent.computed_values().clone_inherited_values();
        auto content_box_wrapper = parent.heap().template allocate<BlockContainer>(parent.document(), nullptr, move(content_box_computed_values));
        content_box_wrapper->set_children_are_inline(parent.children_are_inline());

        Vector<GC::Root<Node>> sequence;
        for (auto child = parent.first_child(); child; child = child->next_sibling()) {
            sequence.append(*child);
        }

        for (auto& node : sequence) {
            parent.remove_child(*node);
            content_box_wrapper->append_child(*node);
        }

        flex_wrapper->append_child(*content_box_wrapper);

        parent.append_child(*flex_wrapper);
        parent.set_children_are_inline(false);
    }
}

void TreeBuilder::update_layout_tree_before_children(DOM::Node& dom_node, GC::Ref<Layout::Node> layout_node, TreeBuilder::Context&, bool element_has_content_visibility_hidden)
{
    // Add node for the ::before pseudo-element.
    if (is<DOM::Element>(dom_node) && layout_node->can_have_children() && !element_has_content_visibility_hidden) {
        auto& element = static_cast<DOM::Element&>(dom_node);
        push_parent(as<NodeWithStyle>(*layout_node));
        create_pseudo_element_if_needed(element, CSS::PseudoElement::Before, AppendOrPrepend::Prepend);
        pop_parent();
    }
}

void TreeBuilder::update_layout_tree_after_children(DOM::Node& dom_node, GC::Ref<Layout::Node> layout_node, TreeBuilder::Context& context, bool element_has_content_visibility_hidden)
{
    auto& document = dom_node.document();
    auto& style_computer = document.style_computer();

    if (is<ListItemBox>(*layout_node)) {
        auto& element = static_cast<DOM::Element&>(dom_node);
        auto marker_style = style_computer.compute_style(element, CSS::PseudoElement::Marker);
        auto list_item_marker = document.heap().allocate<ListItemMarkerBox>(document, layout_node->computed_values().list_style_type(), layout_node->computed_values().list_style_position(), element, marker_style);
        static_cast<ListItemBox&>(*layout_node).set_marker(list_item_marker);
        element.set_pseudo_element_node({}, CSS::PseudoElement::Marker, list_item_marker);
        layout_node->prepend_child(*list_item_marker);
    }

    if (is<SVG::SVGGraphicsElement>(dom_node)) {
        auto& graphics_element = static_cast<SVG::SVGGraphicsElement&>(dom_node);
        // Create the layout tree for the SVG mask/clip paths as a child of the masked element.
        // Note: This will create a new subtree for each use of the mask (so there's  not a 1-to-1 mapping
        // from DOM node to mask layout node). Each use of a mask may be laid out differently so this
        // duplication is necessary.
        auto layout_mask_or_clip_path = [&](GC::Ptr<SVG::SVGElement const> mask_or_clip_path) {
            TemporaryChange<bool> layout_mask(context.layout_svg_mask_or_clip_path, true);
            push_parent(as<NodeWithStyle>(*layout_node));
            update_layout_tree(const_cast<SVG::SVGElement&>(*mask_or_clip_path), context, MustCreateSubtree::Yes);
            pop_parent();
        };
        if (auto mask = graphics_element.mask())
            layout_mask_or_clip_path(mask);
        if (auto clip_path = graphics_element.clip_path())
            layout_mask_or_clip_path(clip_path);
    }

    // Add nodes for the ::after pseudo-element.
    if (is<DOM::Element>(dom_node) && layout_node->can_have_children() && !element_has_content_visibility_hidden) {
        auto& element = static_cast<DOM::Element&>(dom_node);
        push_parent(as<NodeWithStyle>(*layout_node));
        create_pseudo_element_if_needed(element, CSS::PseudoElement::After, AppendOrPrepend::Append);
        pop_parent();
    }
}

GC::Ptr<Layout::Node> TreeBuilder::build(DOM::Node& dom_node)
{
    VERIFY(dom_node.is_document());

    dom_node.document().style_computer().reset_ancestor_filter();

    Context context;
    m_quote_nesting_level = 0;
    update_layout_tree(dom_node, context, MustCreateSubtree::No);

    if (auto* root = dom_node.document().layout_node())
        fixup_tables(*root);

    return m_layout_root;
}

template<CSS::DisplayInternal internal, typename Callback>
void TreeBuilder::for_each_in_tree_with_internal_display(NodeWithStyle& root, Callback callback)
{
    root.for_each_in_inclusive_subtree_of_type<Box>([&](auto& box) {
        auto const display = box.display();
        if (display.is_internal() && display.internal() == internal)
            callback(box);
        return TraversalDecision::Continue;
    });
}

template<CSS::DisplayInside inside, typename Callback>
void TreeBuilder::for_each_in_tree_with_inside_display(NodeWithStyle& root, Callback callback)
{
    root.for_each_in_inclusive_subtree_of_type<Box>([&](auto& box) {
        auto const display = box.display();
        if (display.is_outside_and_inside() && display.inside() == inside)
            callback(box);
        return TraversalDecision::Continue;
    });
}

void TreeBuilder::fixup_tables(NodeWithStyle& root)
{
    remove_irrelevant_boxes(root);
    generate_missing_child_wrappers(root);
    auto table_root_boxes = generate_missing_parents(root);
    missing_cells_fixup(table_root_boxes);
}

void TreeBuilder::remove_irrelevant_boxes(NodeWithStyle& root)
{
    // The following boxes are discarded as if they were display:none:

    Vector<GC::Root<Node>> to_remove;

    // Children of a table-column.
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableColumn>(root, [&](Box& table_column) {
        table_column.for_each_child([&](auto& child) {
            to_remove.append(child);
            return IterationDecision::Continue;
        });
    });

    // Children of a table-column-group which are not a table-column.
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableColumnGroup>(root, [&](Box& table_column_group) {
        table_column_group.for_each_child([&](auto& child) {
            if (!child.display().is_table_column())
                to_remove.append(child);
            return IterationDecision::Continue;
        });
    });

    // FIXME:
    // Anonymous inline boxes which contain only white space and are between two immediate siblings each of which is a table-non-root box.
    // Anonymous inline boxes which meet all of the following criteria:
    // - they contain only white space
    // - they are the first and/or last child of a tabular container
    // - whose immediate sibling, if any, is a table-non-root box

    for (auto& box : to_remove)
        box->parent()->remove_child(*box);
}

static bool is_table_track(CSS::Display display)
{
    return display.is_table_row() || display.is_table_column();
}

static bool is_table_track_group(CSS::Display display)
{
    // Unless explicitly mentioned otherwise, mentions of table-row-groups in this spec also encompass the specialized
    // table-header-groups and table-footer-groups.
    return display.is_table_row_group()
        || display.is_table_header_group()
        || display.is_table_footer_group()
        || display.is_table_column_group();
}

static bool is_proper_table_child(Node const& node)
{
    auto const display = node.display();
    return is_table_track_group(display) || is_table_track(display) || display.is_table_caption();
}

static bool is_not_proper_table_child(Node const& node)
{
    if (!node.has_style())
        return true;
    return !is_proper_table_child(node);
}

static bool is_table_row(Node const& node)
{
    return node.display().is_table_row();
}

static bool is_not_table_row(Node const& node)
{
    if (!node.has_style())
        return true;
    return !is_table_row(node);
}

static bool is_table_cell(Node const& node)
{
    return node.display().is_table_cell();
}

static bool is_not_table_cell(Node const& node)
{
    if (!node.has_style())
        return true;
    return !is_table_cell(node);
}

template<typename Matcher, typename Callback>
static void for_each_sequence_of_consecutive_children_matching(NodeWithStyle& parent, Matcher matcher, Callback callback)
{
    Vector<GC::Root<Node>> sequence;

    auto sequence_is_all_ignorable_whitespace = [&]() -> bool {
        for (auto& node : sequence) {
            if (!is_ignorable_whitespace(*node))
                return false;
        }
        return true;
    };

    for (auto child = parent.first_child(); child; child = child->next_sibling()) {
        if (matcher(*child) || (!sequence.is_empty() && is_ignorable_whitespace(*child))) {
            sequence.append(*child);
        } else {
            if (!sequence.is_empty()) {
                if (!sequence_is_all_ignorable_whitespace())
                    callback(sequence, child);
                sequence.clear();
            }
        }
    }
    if (!sequence.is_empty() && !sequence_is_all_ignorable_whitespace())
        callback(sequence, nullptr);
}

template<typename WrapperBoxType>
static void wrap_in_anonymous(Vector<GC::Root<Node>>& sequence, Node* nearest_sibling, CSS::Display display)
{
    VERIFY(!sequence.is_empty());
    auto& parent = *sequence.first()->parent();
    auto computed_values = parent.computed_values().clone_inherited_values();
    static_cast<CSS::MutableComputedValues&>(*computed_values).set_display(display);
    auto wrapper = parent.heap().template allocate<WrapperBoxType>(parent.document(), nullptr, move(computed_values));
    for (auto& child : sequence) {
        parent.remove_child(*child);
        wrapper->append_child(*child);
    }
    wrapper->set_children_are_inline(parent.children_are_inline());
    if (nearest_sibling)
        parent.insert_before(*wrapper, *nearest_sibling);
    else
        parent.append_child(*wrapper);
}

void TreeBuilder::generate_missing_child_wrappers(NodeWithStyle& root)
{
    // An anonymous table-row box must be generated around each sequence of consecutive children of a table-root box which are not proper table child boxes.
    for_each_in_tree_with_inside_display<CSS::DisplayInside::Table>(root, [&](auto& parent) {
        for_each_sequence_of_consecutive_children_matching(parent, is_not_proper_table_child, [&](auto sequence, auto nearest_sibling) {
            wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableRow });
        });
    });

    // An anonymous table-row box must be generated around each sequence of consecutive children of a table-row-group box which are not table-row boxes.
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableRowGroup>(root, [&](auto& parent) {
        for_each_sequence_of_consecutive_children_matching(parent, is_not_table_row, [&](auto& sequence, auto nearest_sibling) {
            wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableRow });
        });
    });
    // Unless explicitly mentioned otherwise, mentions of table-row-groups in this spec also encompass the specialized
    // table-header-groups and table-footer-groups.
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableHeaderGroup>(root, [&](auto& parent) {
        for_each_sequence_of_consecutive_children_matching(parent, is_not_table_row, [&](auto& sequence, auto nearest_sibling) {
            wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableRow });
        });
    });
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableFooterGroup>(root, [&](auto& parent) {
        for_each_sequence_of_consecutive_children_matching(parent, is_not_table_row, [&](auto& sequence, auto nearest_sibling) {
            wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableRow });
        });
    });

    // An anonymous table-cell box must be generated around each sequence of consecutive children of a table-row box which are not table-cell boxes. !Testcase
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableRow>(root, [&](auto& parent) {
        for_each_sequence_of_consecutive_children_matching(parent, is_not_table_cell, [&](auto& sequence, auto nearest_sibling) {
            wrap_in_anonymous<BlockContainer>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableCell });
        });
    });
}

Vector<GC::Root<Box>> TreeBuilder::generate_missing_parents(NodeWithStyle& root)
{
    Vector<GC::Root<Box>> table_roots_to_wrap;
    root.for_each_in_inclusive_subtree_of_type<Box>([&](auto& parent) {
        // An anonymous table-row box must be generated around each sequence of consecutive table-cell boxes whose parent is not a table-row.
        if (is_not_table_row(parent)) {
            for_each_sequence_of_consecutive_children_matching(parent, is_table_cell, [&](auto& sequence, auto nearest_sibling) {
                wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableRow });
            });
        }

        // A table-row is misparented if its parent is neither a table-row-group nor a table-root box.
        if (!parent.display().is_table_inside() && !is_proper_table_child(parent)) {
            for_each_sequence_of_consecutive_children_matching(parent, is_table_row, [&](auto& sequence, auto nearest_sibling) {
                wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display::from_short(parent.display().is_inline_outside() ? CSS::Display::Short::InlineTable : CSS::Display::Short::Table));
            });
        }

        // A table-row-group, table-column-group, or table-caption box is misparented if its parent is not a table-root box.
        if (!parent.display().is_table_inside() && !is_proper_table_child(parent)) {
            for_each_sequence_of_consecutive_children_matching(parent, is_proper_table_child, [&](auto& sequence, auto nearest_sibling) {
                wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display::from_short(parent.display().is_inline_outside() ? CSS::Display::Short::InlineTable : CSS::Display::Short::Table));
            });
        }

        // An anonymous table-wrapper box must be generated around each table-root.
        if (parent.display().is_table_inside()) {
            if (parent.has_been_wrapped_in_table_wrapper()) {
                VERIFY(parent.parent());
                VERIFY(parent.parent()->is_table_wrapper());
                return TraversalDecision::Continue;
            }
            table_roots_to_wrap.append(parent);
        }

        return TraversalDecision::Continue;
    });

    for (auto& table_box : table_roots_to_wrap) {
        auto* nearest_sibling = table_box->next_sibling();
        auto& parent = *table_box->parent();

        auto wrapper_computed_values = table_box->computed_values().clone_inherited_values();
        table_box->transfer_table_box_computed_values_to_wrapper_computed_values(*wrapper_computed_values);

        if (parent.is_table_wrapper()) {
            auto& existing_wrapper = static_cast<TableWrapper&>(parent);
            existing_wrapper.set_computed_values(move(wrapper_computed_values));
            continue;
        }

        auto wrapper = parent.heap().allocate<TableWrapper>(parent.document(), nullptr, move(wrapper_computed_values));

        parent.remove_child(*table_box);
        wrapper->append_child(*table_box);

        if (nearest_sibling)
            parent.insert_before(*wrapper, *nearest_sibling);
        else
            parent.append_child(*wrapper);

        table_box->set_has_been_wrapped_in_table_wrapper(true);
    }

    return table_roots_to_wrap;
}

template<typename Matcher, typename Callback>
static void for_each_child_box_matching(Box& parent, Matcher matcher, Callback callback)
{
    parent.for_each_child_of_type<Box>([&](Box& child_box) {
        if (matcher(child_box))
            callback(child_box);
        return IterationDecision::Continue;
    });
}

static void fixup_row(Box& row_box, TableGrid const& table_grid, size_t row_index)
{
    for (size_t column_index = 0; column_index < table_grid.column_count(); ++column_index) {
        if (table_grid.occupancy_grid().contains({ column_index, row_index }))
            continue;

        auto computed_values = row_box.computed_values().clone_inherited_values();
        auto& mutable_computed_values = static_cast<CSS::MutableComputedValues&>(*computed_values);
        mutable_computed_values.set_display(Web::CSS::Display { CSS::DisplayInternal::TableCell });
        // Ensure that the cell (with zero content height) will have the same height as the row by setting vertical-align to middle.
        mutable_computed_values.set_vertical_align(CSS::VerticalAlign::Middle);
        auto cell_box = row_box.heap().template allocate<BlockContainer>(row_box.document(), nullptr, move(computed_values));
        row_box.append_child(cell_box);
    }
}

void TreeBuilder::missing_cells_fixup(Vector<GC::Root<Box>> const& table_root_boxes)
{
    // Implements https://www.w3.org/TR/css-tables-3/#missing-cells-fixup.
    for (auto& table_box : table_root_boxes) {
        auto table_grid = TableGrid::calculate_row_column_grid(*table_box);
        size_t row_index = 0;
        for_each_child_box_matching(*table_box, TableGrid::is_table_row_group, [&](auto& row_group_box) {
            for_each_child_box_matching(row_group_box, is_table_row, [&](auto& row_box) {
                fixup_row(row_box, table_grid, row_index);
                ++row_index;
                return IterationDecision::Continue;
            });
        });

        for_each_child_box_matching(*table_box, is_table_row, [&](auto& row_box) {
            fixup_row(row_box, table_grid, row_index);
            ++row_index;
            return IterationDecision::Continue;
        });
    }
}
}
