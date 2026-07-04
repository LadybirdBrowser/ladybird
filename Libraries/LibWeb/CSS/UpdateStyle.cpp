/*
 * Copyright (c) 2018-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/RootVector.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Invalidation/HasMutationInvalidator.h>
#include <LibWeb/CSS/Invalidation/SlotInvalidator.h>
#include <LibWeb/CSS/Invalidation/StyleInvalidator.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/UpdateStyle.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>

namespace Web::CSS {

static void apply_element_style_invalidation_after_style_change(DOM::Element& element, RequiredInvalidationAfterStyleChange const& invalidation, bool invalidate_assigned_slottables, bool invalidate_descendant_slots)
{
    if (invalidate_assigned_slottables)
        Invalidation::invalidate_assigned_slottables_after_slot_style_change(element);
    if (invalidate_descendant_slots && (invalidation.inherited_style_changed || invalidation.rebuild_layout_tree))
        Invalidation::invalidate_assigned_slottables_for_descendant_slots_after_inherited_style_change(element);

    if (invalidation.relayout)
        element.set_needs_layout_update(DOM::SetNeedsLayoutReason::StyleChange);
    if (invalidation.rebuild_layout_tree) {
        // Mark the parent to handle display changes to/from contents correctly.
        if (auto parent_element = element.parent_element())
            parent_element->set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::StyleChange);
        else
            element.set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::StyleChange);
    }

    element.set_needs_style_update(false);
}

static void apply_document_style_invalidation_after_style_change(DOM::Document& document, RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.repaint
        || invalidation.rebuild_stacking_context_tree
        || invalidation.relayout
        || invalidation.rebuild_layout_tree)
        document.set_needs_to_record_display_list();
    if (invalidation.rebuild_accumulated_visual_contexts)
        document.set_needs_accumulated_visual_contexts_update(true);
    if (invalidation.rebuild_stacking_context_tree)
        document.invalidate_stacking_context_tree();
}

enum class StyleUpdateTraversal : u8 {
    Normal,
    TraverseDisplayNoneSubtrees,
};

enum class StyleInvalidationBehavior : u8 {
    Apply,
    Suppress,
};

class ScopedStyleComputerAncestorChain {
public:
    ScopedStyleComputerAncestorChain(StyleComputer& style_computer, DOM::Element& element)
        : m_style_computer(style_computer)
    {
        for (auto* cursor = element.parent_or_shadow_host_element(); cursor; cursor = cursor->parent_or_shadow_host_element())
            m_ancestors.append(*cursor);

        for (size_t i = m_ancestors.size(); i > 0; --i)
            m_style_computer->push_ancestor(m_ancestors[i - 1]);
    }

    ~ScopedStyleComputerAncestorChain()
    {
        for (auto& ancestor : m_ancestors)
            m_style_computer->pop_ancestor(ancestor);
    }

    ScopedStyleComputerAncestorChain(ScopedStyleComputerAncestorChain const&) = delete;
    ScopedStyleComputerAncestorChain& operator=(ScopedStyleComputerAncestorChain const&) = delete;

private:
    GC::Ref<StyleComputer> m_style_computer;
    GC::RootVector<GC::Ref<DOM::Element>> m_ancestors;
};

struct StyleUpdateFrame {
    StyleUpdateFrame(
        DOM::Node& node,
        bool needs_inherited_style_update,
        bool recompute_elements_depending_on_custom_properties,
        bool parent_display_changed,
        bool ancestor_needs_descendant_style_recompute,
        StyleUpdateTraversal traversal,
        StyleInvalidationBehavior invalidation_behavior)
        : node(node)
        , needs_inherited_style_update(needs_inherited_style_update)
        , recompute_elements_depending_on_custom_properties(recompute_elements_depending_on_custom_properties)
        , parent_display_changed(parent_display_changed)
        , ancestor_needs_descendant_style_recompute(ancestor_needs_descendant_style_recompute)
        , traversal(traversal)
        , invalidation_behavior(invalidation_behavior)
    {
    }

    GC::Ref<DOM::Node> node;
    GC::Ptr<DOM::Node> next_child;
    RequiredInvalidationAfterStyleChange invalidation;
    RequiredInvalidationAfterStyleChange node_invalidation;
    bool needs_inherited_style_update { false };
    bool recompute_elements_depending_on_custom_properties { false };
    bool parent_display_changed { false };
    bool ancestor_needs_descendant_style_recompute { false };
    bool needs_full_style_update { false };
    bool is_display_none { false };
    bool children_need_inherited_style_update { false };
    bool children_need_full_style_recompute { false };
    bool descendant_style_recompute_needed { false };
    bool skip_display_none_descent { false };
    bool should_descend { false };
    bool did_enter { false };
    bool did_visit_shadow_root { false };
    StyleUpdateTraversal traversal { StyleUpdateTraversal::Normal };
    StyleInvalidationBehavior invalidation_behavior { StyleInvalidationBehavior::Apply };
};

static void enter_style_update_frame(StyleUpdateFrame& frame, StyleComputer& style_computer)
{
    auto& node = *frame.node;
    frame.needs_full_style_update = node.document().needs_full_style_update();

    if (node.is_element())
        style_computer.push_ancestor(static_cast<DOM::Element const&>(node));

    // NOTE: If the current node has `display:none`, we can disregard all invalidation
    //       caused by its children, as they will not be rendered anyway.
    //       We will still recompute style for the children, though.
    bool did_change_custom_properties = false;
    if (is<DOM::Element>(node)) {
        auto& element = static_cast<DOM::Element&>(node);

        // FIXME: We can avoid some unnecessary re-computations by, skipping if a) the contained if() functions don't
        //        include media conditions, or b) the data used to resolve media queries hasn't changed.
        bool const needs_style_update_due_to_if_media = element.style_uses_if_css_function();

        if (frame.needs_full_style_update
            || node.needs_style_update()
            || (frame.traversal == StyleUpdateTraversal::TraverseDisplayNoneSubtrees && !element.computed_properties())
            || frame.parent_display_changed
            || frame.ancestor_needs_descendant_style_recompute
            || (frame.recompute_elements_depending_on_custom_properties && (element.style_uses_var_css_function() || element.style_uses_inherit_css_function()))
            || needs_style_update_due_to_if_media) {
            frame.node_invalidation = element.recompute_style(did_change_custom_properties);
        } else {
            if (frame.needs_inherited_style_update)
                frame.node_invalidation = element.recompute_inherited_style(DOM::ScheduleAnimationUpdate::Yes);
            if (frame.recompute_elements_depending_on_custom_properties && element.refresh_inherited_custom_property_data())
                did_change_custom_properties = true;
        }
        frame.is_display_none = element.computed_properties()->display().is_none();

        if (frame.invalidation_behavior == StyleInvalidationBehavior::Suppress)
            element.set_needs_style_update(false);
        else
            apply_element_style_invalidation_after_style_change(element, frame.node_invalidation, !frame.node_invalidation.is_none(), !frame.needs_inherited_style_update);
    }

    if (!node.is_element())
        node.set_needs_style_update(false);
    frame.invalidation |= frame.node_invalidation;

    if (did_change_custom_properties)
        frame.recompute_elements_depending_on_custom_properties = true;

    frame.children_need_inherited_style_update = !frame.invalidation.is_none();
    if (!node.is_element())
        frame.children_need_inherited_style_update |= frame.needs_inherited_style_update;
    // NB: When display changes to/from flex/grid/contents, children may need to be blockified or un-blockified.
    //     This requires a full style recompute, not just inherited style update.
    frame.children_need_full_style_recompute = frame.node_invalidation.rebuild_layout_tree;
    frame.descendant_style_recompute_needed = frame.ancestor_needs_descendant_style_recompute || frame.node_invalidation.recompute_descendant_styles;

    // OPTIMIZATION: Descendants of a display:none element are not rendered and their computed style is not observable
    //               except through on-demand reads, which call Document::update_style_for_element to refresh the path
    //               lazily. We can therefore skip the descent entirely. The exception is when display itself just
    //               changed or the document needs a full style update. In those cases descendants must be re-cascaded
    //               eagerly.
    frame.skip_display_none_descent = frame.traversal == StyleUpdateTraversal::Normal
        && frame.is_display_none
        && !frame.needs_full_style_update
        && !frame.children_need_full_style_recompute;
    if (frame.skip_display_none_descent
        && (did_change_custom_properties
            || frame.node_invalidation.inherited_style_changed
            || frame.node_invalidation.recompute_descendant_styles))
        node.set_child_needs_style_update(true);

    frame.should_descend = !frame.skip_display_none_descent
        && (frame.traversal == StyleUpdateTraversal::TraverseDisplayNoneSubtrees
            || frame.needs_full_style_update
            || node.child_needs_style_update()
            || frame.children_need_inherited_style_update
            || frame.recompute_elements_depending_on_custom_properties
            || frame.children_need_full_style_recompute
            || frame.descendant_style_recompute_needed);
    frame.next_child = node.first_child();
    frame.did_enter = true;
}

static bool should_update_style_for_child(StyleUpdateFrame const& frame, DOM::Node const& child, bool child_needs_inherited_style_update)
{
    if (frame.traversal == StyleUpdateTraversal::TraverseDisplayNoneSubtrees) {
        if (auto const* element = as_if<DOM::Element>(child); element && !element->computed_properties())
            return true;
    }

    return frame.needs_full_style_update
        || child.needs_style_update()
        || child_needs_inherited_style_update
        || child.child_needs_style_update()
        || frame.recompute_elements_depending_on_custom_properties
        || frame.children_need_full_style_recompute
        || frame.descendant_style_recompute_needed;
}

static void finish_style_update_frame(StyleUpdateFrame& frame, StyleComputer& style_computer)
{
    auto& node = *frame.node;
    if (!frame.skip_display_none_descent)
        node.set_child_needs_style_update(false);

    if (node.is_element())
        style_computer.pop_ancestor(static_cast<DOM::Element const&>(node));
}

static void push_style_update_frame_for_child(GC::ConservativeVector<StyleUpdateFrame, 32>& stack, StyleUpdateFrame const& parent_frame, DOM::Node& child, bool child_needs_inherited_style_update)
{
    auto recompute_elements_depending_on_custom_properties = parent_frame.recompute_elements_depending_on_custom_properties;
    auto children_need_full_style_recompute = parent_frame.children_need_full_style_recompute;
    auto descendant_style_recompute_needed = parent_frame.descendant_style_recompute_needed;
    auto traversal = parent_frame.traversal;
    auto invalidation_behavior = parent_frame.invalidation_behavior;

    stack.empend(
        child,
        child_needs_inherited_style_update,
        recompute_elements_depending_on_custom_properties,
        children_need_full_style_recompute,
        descendant_style_recompute_needed,
        traversal,
        invalidation_behavior);
}

[[nodiscard]] static RequiredInvalidationAfterStyleChange update_style_iteratively(
    DOM::Node& root,
    StyleComputer& style_computer,
    bool needs_inherited_style_update,
    bool recompute_elements_depending_on_custom_properties,
    bool parent_display_changed,
    bool ancestor_needs_descendant_style_recompute,
    StyleUpdateTraversal traversal = StyleUpdateTraversal::Normal,
    StyleInvalidationBehavior invalidation_behavior = StyleInvalidationBehavior::Apply)
{
    GC::ConservativeVector<StyleUpdateFrame, 32> stack;
    stack.empend(root, needs_inherited_style_update, recompute_elements_depending_on_custom_properties, parent_display_changed, ancestor_needs_descendant_style_recompute, traversal, invalidation_behavior);

    RequiredInvalidationAfterStyleChange root_invalidation;
    while (!stack.is_empty()) {
        auto& frame = stack.last();
        if (!frame.did_enter)
            enter_style_update_frame(frame, style_computer);

        if (frame.should_descend && !frame.did_visit_shadow_root) {
            frame.did_visit_shadow_root = true;

            if (is<DOM::Element>(*frame.node)) {
                if (auto shadow_root = static_cast<DOM::Element&>(*frame.node).shadow_root()) {
                    bool shadow_tree_children_need_inherited_style_update = frame.node_invalidation.inherited_style_changed
                        || (!frame.node_invalidation.is_none() && shadow_root->children_may_depend_on_non_inherited_property_inheritance());
                    if (should_update_style_for_child(frame, *shadow_root, shadow_tree_children_need_inherited_style_update)) {
                        push_style_update_frame_for_child(stack, frame, *shadow_root, shadow_tree_children_need_inherited_style_update);
                        continue;
                    }
                }
            }
        }

        bool did_push_child = false;
        while (frame.should_descend && frame.next_child) {
            auto child = frame.next_child;
            frame.next_child = child->next_sibling();

            if (!should_update_style_for_child(frame, *child, frame.children_need_inherited_style_update))
                continue;

            push_style_update_frame_for_child(stack, frame, *child, frame.children_need_inherited_style_update);
            did_push_child = true;
            break;
        }
        if (did_push_child)
            continue;

        auto frame_invalidation = frame.invalidation;
        finish_style_update_frame(frame, style_computer);
        (void)stack.take_last();

        if (stack.is_empty()) {
            root_invalidation = frame_invalidation;
            break;
        }

        auto& parent_frame = stack.last();
        if (parent_frame.traversal == StyleUpdateTraversal::TraverseDisplayNoneSubtrees || !parent_frame.is_display_none)
            parent_frame.invalidation |= frame_invalidation;
    }

    return root_invalidation;
}

void update_style(DOM::Document& document)
{
    // NOTE: If our parent document needs a relayout, we must do that *first*. This is required as it may cause the
    // viewport to change which will can affect media query evaluation and the value of the `vw` unit.
    if (auto navigable = document.navigable(); navigable && navigable->container() && &navigable->container()->document() != &document)
        navigable->container()->document().update_layout(DOM::UpdateLayoutReason::ChildDocumentStyleUpdate);

    if (!document.browsing_context())
        return;

    // Fetch the viewport rect once, instead of repeatedly, during style computation.
    document.update_style_computer_viewport_rect();

    document.update_animated_style_if_needed();

    // Associated with each top-level browsing context is a current transition generation that is incremented on each
    // style change event. [CSS-Transitions-2]
    document.increment_transition_generation();

    if (document.consume_needs_invalidation_of_elements_affected_by_has())
        Invalidation::invalidate_style_for_pending_has_mutations(document);

    if (!document.style_invalidator().has_pending_invalidations() && !document.needs_full_style_update() && !document.needs_style_update() && !document.child_needs_style_update() && !document.needs_media_rule_evaluation())
        return;

    // NOTE: If this is a document hosting <template> contents, style update is unnecessary.
    if (document.created_for_appropriate_template_contents())
        return;

    if (document.needs_media_rule_evaluation())
        document.evaluate_media_rules_for_style_update();

    if (!document.style_invalidator().has_pending_invalidations() && !document.needs_full_style_update() && !document.needs_style_update() && !document.child_needs_style_update())
        return;

    document.style_invalidator().invalidate(document);

    document.build_registered_properties_cache_for_style_update();

    RequiredInvalidationAfterStyleChange invalidation;
    constexpr size_t max_style_update_passes = 8;
    for (size_t style_update_pass = 0; style_update_pass < max_style_update_passes; ++style_update_pass) {
        document.style_computer().reset_has_result_cache();
        document.style_computer().reset_ancestor_filter();

        invalidation |= update_style_iteratively(document, document.style_computer(), false, false, false, false);
        document.set_needs_full_style_update(false);

        if (!document.style_invalidator().has_pending_invalidations() && !document.needs_style_update() && !document.child_needs_style_update())
            break;

        document.style_invalidator().invalidate(document);
    }

    apply_document_style_invalidation_after_style_change(document, invalidation);
    document.update_animated_style_if_needed();
}

void update_style_if_needed_for_element(DOM::Document& document, DOM::AbstractElement const& abstract_element)
{
    if (element_needs_style_update(document, abstract_element))
        update_style(document);
}

static void mark_direct_children_for_style_update(DOM::Element& element)
{
    if (auto shadow_root = element.shadow_root()) {
        shadow_root->for_each_child([](auto& child) {
            child.set_needs_style_update(true);
            return IterationDecision::Continue;
        });
    }

    element.for_each_child([](auto& child) {
        child.set_needs_style_update(true);
        return IterationDecision::Continue;
    });
}

static void apply_targeted_style_invalidation(DOM::Element& element, RequiredInvalidationAfterStyleChange const& invalidation, bool did_change_custom_properties, bool descendant_style_recompute_needed)
{
    apply_element_style_invalidation_after_style_change(element, invalidation, !invalidation.is_none() || did_change_custom_properties, true);

    if (!invalidation.is_none() || did_change_custom_properties || descendant_style_recompute_needed || invalidation.recompute_descendant_styles)
        mark_direct_children_for_style_update(element);

    apply_document_style_invalidation_after_style_change(element.document(), invalidation);
}

static RequiredInvalidationAfterStyleChange recompute_style_for_targeted_style_update(DOM::Element& element, bool& did_change_custom_properties)
{
    if (element.parent())
        return element.recompute_style(did_change_custom_properties);

    auto new_computed_properties = element.document().style_computer().compute_style({ element }, did_change_custom_properties);
    element.set_computed_properties({}, move(new_computed_properties));
    element.set_needs_style_update(false);
    return {};
}

ComputedProperties const* update_style_for_element(DOM::Document& document, DOM::AbstractElement const& abstract_element, StyleUpdateMode mode)
{
    // Refresh computed properties for an abstract element without requiring every unrelated dirty element in the
    // document to be resolved. This walks the flat-tree inheritance chain and re-cascades from the rootmost stale
    // element on the path back down to the target. Normal mode also re-cascades the target path under display:none
    // ancestors, because the regular document traversal may leave those descendants stale.

    if (auto navigable = document.navigable(); navigable && navigable->container() && &navigable->container()->document() != &document)
        navigable->container()->document().update_layout(DOM::UpdateLayoutReason::ChildDocumentStyleUpdate);

    if (document.browsing_context()) {
        document.update_style_computer_viewport_rect();

        document.update_animated_style_if_needed();

        // Media query evaluation can enqueue normal style invalidations, so do it before deciding whether the full
        // style traversal needs to run.
        if (document.needs_media_rule_evaluation())
            document.evaluate_media_rules_for_style_update();

        if (!document.is_running_update_layout()
            && (document.needs_full_style_update()
                || document.needs_invalidation_of_elements_affected_by_has()
                || document.style_invalidator().has_pending_invalidations()
                || document.needs_style_update())) {
            update_style(document);
        }
    }

    // Single walk up the inheritance chain: collect each ancestor and remember the index of the topmost display:none
    // entry seen. Pseudo-element styles are refreshed when the originating element is recomputed, so don't put the pseudo
    // on the path.
    GC::RootVector<GC::Ref<DOM::Element>> inheritance_chain;
    if (!abstract_element.pseudo_element().has_value())
        inheritance_chain.append(const_cast<DOM::Element&>(abstract_element.element()));

    Optional<size_t> topmost_display_none_index;
    Optional<size_t> topmost_element_requiring_style;
    bool ancestor_needs_descendant_style_recompute = false;
    for (auto cursor = abstract_element.element_to_inherit_style_from(); cursor.has_value(); cursor = cursor->element_to_inherit_style_from()) {
        auto& ancestor = const_cast<DOM::Element&>(cursor->element());
        inheritance_chain.append(ancestor);
    }

    for (size_t i = inheritance_chain.size(); i > 0; --i) {
        auto& ancestor = inheritance_chain[i - 1];
        if (!topmost_element_requiring_style.has_value()
            && (ancestor_needs_descendant_style_recompute
                || ancestor->needs_style_update()
                || !ancestor->computed_properties())) {
            topmost_element_requiring_style = i - 1;
        }

        if (ancestor->entire_subtree_needs_style_update()) {
            if (!topmost_element_requiring_style.has_value())
                topmost_element_requiring_style = i - 1;
            ancestor_needs_descendant_style_recompute = true;
        }

        if (auto const properties = ancestor->computed_properties(); properties && properties->display().is_none()) {
            topmost_display_none_index = i - 1;
            if (mode == StyleUpdateMode::StopAtDisplayNone && !topmost_element_requiring_style.has_value())
                return nullptr;
        }
    }

    Optional<size_t> topmost_element_to_recompute = topmost_element_requiring_style;
    if (mode == StyleUpdateMode::Normal && topmost_display_none_index.has_value()) {
        if (!topmost_element_to_recompute.has_value() && *topmost_display_none_index > 0)
            topmost_element_to_recompute = *topmost_display_none_index - 1;
    }

    if (!topmost_element_to_recompute.has_value()) {
        if (mode == StyleUpdateMode::Normal && !inheritance_chain.is_empty())
            topmost_element_to_recompute = 0;
        else
            return abstract_element.computed_properties();
    }

    document.style_computer().reset_has_result_cache();

    // Re-cascading the inheritance chain requires the style computer's ancestor filter to reflect each recomputed
    // element's DOM ancestors, so that descendant-combinator selectors match correctly. The filter is empty at this
    // point because the normal top-down `update_style` traversal skipped the display:none subtree, so we have to seed
    // it ourselves.
    auto& style_computer = document.style_computer();
    ScopedStyleComputerAncestorChain scoped_ancestor_chain { style_computer, inheritance_chain[*topmost_element_to_recompute] };

    GC::RootVector<GC::Ref<DOM::Element>> pushed_path_ancestors;

    ScopeGuard pop_path_ancestors = [&] {
        for (auto& ancestor : pushed_path_ancestors)
            style_computer.pop_ancestor(ancestor);
    };

    bool descendant_style_recompute_needed = false;
    for (size_t i = *topmost_element_to_recompute + 1; i > 0; --i) {
        auto& element = inheritance_chain[i - 1];
        bool did_change_custom_properties = false;
        auto invalidation = recompute_style_for_targeted_style_update(element, did_change_custom_properties);
        apply_targeted_style_invalidation(element, invalidation, did_change_custom_properties, descendant_style_recompute_needed);

        descendant_style_recompute_needed |= invalidation.recompute_descendant_styles;

        if (element->computed_properties()->display().is_none()) {
            if (mode == StyleUpdateMode::StopAtDisplayNone)
                return nullptr;
            descendant_style_recompute_needed = false;
        }

        if (did_change_custom_properties || invalidation.rebuild_layout_tree)
            descendant_style_recompute_needed = true;

        if (i > 1) {
            style_computer.push_ancestor(element);
            pushed_path_ancestors.append(element);
        }
    }

    return abstract_element.computed_properties();
}

void update_style_for_subtree_including_display_none(DOM::Document& document, DOM::Element const& subtree_root)
{
    auto& root = const_cast<DOM::Element&>(subtree_root);

    auto* update_root = &root;
    for (auto* ancestor = &root; ancestor; ancestor = ancestor->parent_or_shadow_host_element()) {
        if (!ancestor->computed_properties())
            update_root = ancestor;
        if (auto const properties = ancestor->computed_properties(); properties && properties->display().is_none())
            update_root = ancestor;
    }

    auto force_descendant_style_recompute = update_root->child_needs_style_update();

    if (update_root->computed_properties() && !update_root->needs_style_update() && !force_descendant_style_recompute)
        return;

    VERIFY(&update_root->document() == &document);

    document.style_computer().reset_has_result_cache();

    auto& style_computer = document.style_computer();
    ScopedStyleComputerAncestorChain scoped_ancestor_chain { style_computer, *update_root };

    (void)update_style_iteratively(*update_root, style_computer, false, false, false, force_descendant_style_recompute, StyleUpdateTraversal::TraverseDisplayNoneSubtrees, StyleInvalidationBehavior::Suppress);
    document.update_animated_style_if_needed();
}

bool element_needs_style_update(DOM::Document const& document, DOM::AbstractElement const& abstract_element)
{
    // If there are document-level reasons to update style, we can't skip.
    if (document.needs_full_style_update())
        return true;
    if (document.needs_animated_style_update())
        return true;
    if (document.needs_invalidation_of_elements_affected_by_has())
        return true;
    if (document.needs_media_rule_evaluation())
        return true;
    if (document.style_invalidator().has_pending_invalidations())
        return true;

    // Check the element itself.
    if (abstract_element.element().needs_style_update())
        return true;
    if (abstract_element.element().entire_subtree_needs_style_update())
        return true;

    // Walk the inheritance ancestor chain. We use element_to_inherit_style_from()
    // because style inheritance follows the flat tree (slotted elements inherit
    // from their assigned slot, not their DOM parent). If any ancestor on the
    // path has its style marked dirty, the target element's computed style could
    // change via inherited properties, so we must update.
    for (auto ancestor = abstract_element.element_to_inherit_style_from(); ancestor.has_value(); ancestor = ancestor->element_to_inherit_style_from()) {
        if (ancestor->element().needs_style_update())
            return true;
        if (ancestor->element().entire_subtree_needs_style_update())
            return true;
    }

    // If the navigable has a container and that container needs a style update, then we need one as well, since the
    // container's style can affect this element (e.g. via media queries or viewport units).
    if (auto navigable = document.navigable()) {
        if (auto container = navigable->container()) {
            if (element_needs_style_update(container->document(), DOM::AbstractElement { *container }))
                return true;
        }
    }

    return false;
}

}

namespace Web::DOM {

void Document::update_style()
{
    CSS::update_style(*this);
}

void Document::update_style_if_needed_for_element(AbstractElement const& abstract_element)
{
    CSS::update_style_if_needed_for_element(*this, abstract_element);
}

CSS::ComputedProperties const* Document::update_style_for_element(AbstractElement const& abstract_element)
{
    return CSS::update_style_for_element(*this, abstract_element);
}

CSS::ComputedProperties const* Document::update_style_for_element(AbstractElement const& abstract_element, StyleUpdateMode mode)
{
    return CSS::update_style_for_element(*this, abstract_element, mode);
}

bool Document::element_needs_style_update(AbstractElement const& abstract_element) const
{
    return CSS::element_needs_style_update(*this, abstract_element);
}

}
