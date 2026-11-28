/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/IterationDecision.h>
#include <AK/TypeCasts.h>
#include <AK/Vector.h>
#include <LibGC/Member.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWeb/TraversalDecision.h>

namespace Web {

template<typename T, typename Callback>
TraversalDecision traverse_preorder(T root, Callback callback)
{
    T current = root;
    while (current != nullptr) {
        TraversalDecision decision = callback(*current);
        if (decision == TraversalDecision::Break)
            return TraversalDecision::Break;

        if (decision != TraversalDecision::SkipChildrenAndContinue && current->first_child() != nullptr) {
            current = current->first_child();
            continue;
        }
        if (current == root)
            break;

        if (current->next_sibling() != nullptr) {
            current = current->next_sibling();
            continue;
        }

        while (current != root && current->next_sibling() == nullptr)
            current = current->parent();
        if (current == root)
            break;

        current = current->next_sibling();
    }
    return TraversalDecision::Continue;
}

template<typename T>
class WEB_API TreeNode {
public:
    T* parent() { return links().parent.ptr(); }
    T const* parent() const { return links().parent.ptr(); }

    bool has_children() const { return !!links().first_child; }
    T* next_sibling() { return links().next_sibling.ptr(); }
    T* previous_sibling() { return links().previous_sibling.ptr(); }
    T* first_child() { return links().first_child.ptr(); }
    T* last_child() { return links().last_child.ptr(); }
    T const* next_sibling() const { return links().next_sibling.ptr(); }
    T const* previous_sibling() const { return links().previous_sibling.ptr(); }
    T const* first_child() const { return links().first_child.ptr(); }
    T const* last_child() const { return links().last_child.ptr(); }

    // https://dom.spec.whatwg.org/#concept-tree-index
    size_t index() const
    {
        // The index of an object is its number of preceding siblings, or 0 if it has none.
        size_t index = 0;
        for (auto* node = previous_sibling(); node; node = node->previous_sibling())
            ++index;
        return index;
    }

    // https://dom.spec.whatwg.org/#concept-tree-root
    T& root()
    {
        // The root of an object is itself, if its parent is null, or else it is the root of its parent.
        // The root of a tree is any object participating in that tree whose parent is null.
        T* root = static_cast<T*>(this);
        while (root->parent())
            root = root->parent();
        return *root;
    }
    T const& root() const { return const_cast<TreeNode*>(this)->root(); }

    bool is_ancestor_of(TreeNode const&) const;
    bool is_inclusive_ancestor_of(TreeNode const&) const;
    bool contains(GC::Ptr<T>) const;
    bool is_descendant_of(TreeNode const&) const;
    bool is_inclusive_descendant_of(TreeNode const&) const;

    bool is_following(TreeNode const&) const;
    bool is_before(TreeNode const&) const;

    void append_child(GC::Ref<T> node);

    size_t child_count() const
    {
        size_t count = 0;
        for (auto* child = first_child(); child; child = child->next_sibling())
            ++count;
        return count;
    }

    T* child_at_index(size_t index)
    {
        size_t count = 0;
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (count == index)
                return child;
            ++count;
        }
        return nullptr;
    }

    T const* child_at_index(size_t index) const
    {
        return const_cast<TreeNode*>(this)->child_at_index(index);
    }

    T* next_in_pre_order()
    {
        if (first_child())
            return first_child();
        T* node;
        if (!(node = next_sibling())) {
            node = parent();
            while (node && !node->next_sibling())
                node = node->parent();
            if (node)
                node = node->next_sibling();
        }
        return node;
    }

    T* next_in_pre_order(T const* stay_within)
    {
        if (first_child())
            return first_child();

        T* node = static_cast<T*>(this);
        while (node) {
            if (node == stay_within)
                return nullptr;
            if (T* next = node->next_sibling())
                return next;
            node = node->parent();
        }
        return nullptr;
    }

    T const* next_in_pre_order() const
    {
        return const_cast<TreeNode*>(this)->next_in_pre_order();
    }

    T const* next_in_pre_order(T const* stay_within) const
    {
        return const_cast<TreeNode*>(this)->next_in_pre_order(stay_within);
    }

    T* previous_in_pre_order()
    {
        if (auto* node = previous_sibling()) {
            while (node->last_child())
                node = node->last_child();

            return node;
        }

        return parent();
    }

    T const* previous_in_pre_order() const
    {
        return const_cast<TreeNode*>(this)->previous_in_pre_order();
    }

    template<typename Callback>
    TraversalDecision for_each_in_inclusive_subtree(Callback callback) const
    {
        return traverse_preorder(static_cast<T const*>(this), callback);
    }

    template<typename Callback>
    TraversalDecision for_each_in_inclusive_subtree(Callback callback)
    {
        return traverse_preorder(static_cast<T*>(this), callback);
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_inclusive_subtree_of_type(Callback callback)
    {
        return for_each_in_inclusive_subtree([callback = move(callback)](T& node) {
            if (auto* maybe_node_of_type = as_if<U>(node))
                return callback(*maybe_node_of_type);
            return TraversalDecision::Continue;
        });
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_inclusive_subtree_of_type(Callback callback) const
    {
        return for_each_in_inclusive_subtree([callback = move(callback)](T const& node) {
            if (auto* maybe_node_of_type = as_if<U>(node))
                return callback(*maybe_node_of_type);
            return TraversalDecision::Continue;
        });
    }

    template<typename Callback>
    TraversalDecision for_each_in_subtree(Callback callback) const
    {
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (child->for_each_in_inclusive_subtree(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename Callback>
    TraversalDecision for_each_in_subtree(Callback callback)
    {
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (child->for_each_in_inclusive_subtree(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_subtree_of_type(Callback callback)
    {
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (child->template for_each_in_inclusive_subtree_of_type<U>(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_subtree_of_type(Callback callback) const
    {
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (child->template for_each_in_inclusive_subtree_of_type<U>(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename Callback>
    void for_each_child(Callback callback) const
    {
        return const_cast<TreeNode*>(this)->for_each_child(move(callback));
    }

    template<typename Callback>
    void for_each_child(Callback callback)
    {
        for (auto* node = first_child(); node; node = node->next_sibling()) {
            if (callback(*node) == IterationDecision::Break)
                return;
        }
    }

    template<typename U, typename Callback>
    void for_each_child_of_type(Callback callback)
    {
        for (auto* node = first_child(); node; node = node->next_sibling()) {
            if (auto* node_of_type = as_if<U>(node)) {
                if (callback(*node_of_type) == IterationDecision::Break)
                    return;
            }
        }
    }

    template<typename U, typename Callback>
    void for_each_child_of_type(Callback callback) const
    {
        return const_cast<TreeNode*>(this)->template for_each_child_of_type<U>(move(callback));
    }

    template<typename U>
    U const* next_sibling_of_type() const
    {
        return const_cast<TreeNode*>(this)->template next_sibling_of_type<U>();
    }

    template<typename U>
    inline U* next_sibling_of_type()
    {
        for (auto* sibling = next_sibling(); sibling; sibling = sibling->next_sibling()) {
            if (auto* sibling_of_type = as_if<U>(*sibling))
                return sibling_of_type;
        }
        return nullptr;
    }

    template<typename U>
    U const* previous_sibling_of_type() const
    {
        return const_cast<TreeNode*>(this)->template previous_sibling_of_type<U>();
    }

    template<typename U>
    U* previous_sibling_of_type()
    {
        for (auto* sibling = previous_sibling(); sibling; sibling = sibling->previous_sibling()) {
            if (auto* sibling_of_type = as_if<U>(*sibling))
                return sibling_of_type;
        }
        return nullptr;
    }

    template<typename U>
    bool has_child_of_type() const
    {
        return first_child_of_type<U>() != nullptr;
    }

    template<typename U>
    U const* first_child_of_type() const
    {
        return const_cast<TreeNode*>(this)->template first_child_of_type<U>();
    }

    template<typename U>
    U const* last_child_of_type() const
    {
        return const_cast<TreeNode*>(this)->template last_child_of_type<U>();
    }

    template<typename U>
    U* first_child_of_type()
    {
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (auto* child_of_type = as_if<U>(*child))
                return child_of_type;
        }
        return nullptr;
    }

    template<typename U>
    U* last_child_of_type()
    {
        for (auto* child = last_child(); child; child = child->previous_sibling()) {
            if (auto* child_of_type = as_if<U>(*child))
                return child_of_type;
        }
        return nullptr;
    }

    template<typename U>
    U const* first_ancestor_of_type() const
    {
        return const_cast<TreeNode*>(this)->template first_ancestor_of_type<U>();
    }

    template<typename U>
    U* first_ancestor_of_type()
    {
        for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
            if (auto* ancestor_of_type = as_if<U>(*ancestor))
                return ancestor_of_type;
        }
        return nullptr;
    }

    template<typename Callback>
    void for_each_ancestor(Callback callback) const
    {
        return const_cast<TreeNode*>(this)->for_each_ancestor(move(callback));
    }

    template<typename Callback>
    void for_each_ancestor(Callback callback)
    {
        for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
            if (callback(static_cast<T&>(*ancestor)) == IterationDecision::Break)
                break;
        }
    }

    template<typename Callback>
    void for_each_inclusive_ancestor(Callback callback) const
    {
        return const_cast<TreeNode*>(this)->for_each_inclusive_ancestor(move(callback));
    }

    template<typename Callback>
    void for_each_inclusive_ancestor(Callback callback)
    {
        for (auto* ancestor = this; ancestor; ancestor = ancestor->parent()) {
            if (callback(static_cast<T&>(*ancestor)) == IterationDecision::Break)
                break;
        }
    }

    template<typename U, typename Callback>
    void for_each_inclusive_ancestor_of_type(Callback callback) const
    {
        return const_cast<TreeNode*>(this)->for_each_inclusive_ancestor_of_type<U>(move(callback));
    }

    template<typename U, typename Callback>
    void for_each_inclusive_ancestor_of_type(Callback callback)
    {
        for (auto* ancestor = static_cast<T*>(this); ancestor; ancestor = ancestor->parent()) {
            if (auto* ancestor_of_type = as_if<U>(*ancestor)) {
                if (callback(*ancestor_of_type) == IterationDecision::Break)
                    return;
            }
        }
    }

    ~TreeNode() = default;

protected:
    TreeNode() = default;

    void visit_edges(JS::Cell::Visitor& visitor)
    {
        visitor.visit(links().parent);
        visitor.visit(links().first_child);
        visitor.visit(links().last_child);
        visitor.visit(links().next_sibling);
        visitor.visit(links().previous_sibling);
    }

private:
    // NB: T stores the links, as GC::Members named parent, first_child, last_child, next_sibling and previous_sibling
    //     of what its tree_node_links() returns, so that a T can embed them in state laid out elsewhere.
    auto& links() { return static_cast<T*>(this)->tree_node_links(); }
    auto const& links() const { return static_cast<T const*>(this)->tree_node_links(); }
};

// The links of a T that keeps them in a field of their own.
template<typename T>
struct TreeNodeLinks {
    GC::Member<T> parent;
    GC::Member<T> first_child;
    GC::Member<T> last_child;
    GC::Member<T> next_sibling;
    GC::Member<T> previous_sibling;
};

template<typename T>
inline void TreeNode<T>::append_child(GC::Ref<T> node)
{
    VERIFY(!node->links().parent);

    if (links().last_child)
        links().last_child->links().next_sibling = node.ptr();
    node->links().previous_sibling = links().last_child;
    node->links().parent = static_cast<T*>(this);
    links().last_child = node.ptr();
    if (!links().first_child)
        links().first_child = links().last_child;
}

template<typename T>
inline bool TreeNode<T>::is_ancestor_of(TreeNode<T> const& other) const
{
    // OPTIMIZATION: Each link is tested for null while it is still compressed, and then decoded without a null check,
    //               and the ancestors are compared as Ts, so each step up is one load, one test, one decode and one
    //               compare.
    auto const* target = static_cast<T const*>(this);
    for (auto const* node = static_cast<T const*>(&other); node->links().parent;) {
        node = &*node->links().parent;
        if (node == target)
            return true;
    }
    return false;
}

template<typename T>
inline bool TreeNode<T>::is_inclusive_ancestor_of(TreeNode<T> const& other) const
{
    return &other == this || is_ancestor_of(other);
}

// https://dom.spec.whatwg.org/#dom-node-contains
template<typename T>
inline bool TreeNode<T>::contains(GC::Ptr<T> other) const
{
    // The contains(other) method steps are to return true if other is an inclusive descendant of this; otherwise false (including when other is null).
    return other && other->is_inclusive_descendant_of(*this);
}

template<typename T>
inline bool TreeNode<T>::is_descendant_of(TreeNode<T> const& other) const
{
    return other.is_ancestor_of(*this);
}

template<typename T>
inline bool TreeNode<T>::is_inclusive_descendant_of(TreeNode<T> const& other) const
{
    return other.is_inclusive_ancestor_of(*this);
}

// https://dom.spec.whatwg.org/#concept-tree-following
template<typename T>
inline bool TreeNode<T>::is_following(TreeNode const& other) const
{
    // An object A is following an object B if A and B are in the same tree, and A comes after B in tree order. That's
    // exactly the order is_before() decides. So, ask is_before() — rather than scan the preorder all the way back to
    // the start of the tree: The ancestor chains is_before() walks are depth-bounded — while the scan's document-sized.
    return other.is_before(*this);
}

template<typename T>
inline bool TreeNode<T>::is_before(TreeNode const& other) const
{
    if (this == &other)
        return false;
    // Compare tree order through the ancestor chains rather than scanning the preorder between the
    // two nodes: the chains are depth-bounded, while the preorder distance is document-sized and a
    // negative answer used to scan all the way to the end of the tree.
    Vector<TreeNode const*, 32> my_chain;
    for (auto* node = this; node; node = node->parent())
        my_chain.append(node);
    Vector<TreeNode const*, 32> other_chain;
    for (auto* node = &other; node; node = node->parent())
        other_chain.append(node);
    // Nodes in different trees have no order, exactly as the preorder scan answered.
    if (my_chain.last() != other_chain.last())
        return false;
    size_t my_depth = my_chain.size();
    size_t other_depth = other_chain.size();
    while (my_depth > 0 && other_depth > 0 && my_chain[my_depth - 1] == other_chain[other_depth - 1]) {
        --my_depth;
        --other_depth;
    }
    // An inclusive ancestor precedes its descendants; otherwise the divergent children of the
    // deepest common ancestor decide by their sibling order.
    if (my_depth == 0)
        return true;
    if (other_depth == 0)
        return false;
    for (auto* sibling = my_chain[my_depth - 1]->next_sibling(); sibling; sibling = sibling->next_sibling()) {
        if (sibling == other_chain[other_depth - 1])
            return true;
    }
    return false;
}

}
