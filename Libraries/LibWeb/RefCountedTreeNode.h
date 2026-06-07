/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/IterationDecision.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/TypeCasts.h>
#include <AK/WeakPtr.h>
#include <LibWeb/Export.h>
#include <LibWeb/TraversalDecision.h>

namespace Web {

template<typename T, typename Callback>
TraversalDecision traverse_ref_counted_preorder(RefPtr<T> root, Callback callback)
{
    auto current = root;
    while (current) {
        TraversalDecision decision = callback(*current);
        if (decision == TraversalDecision::Break)
            return TraversalDecision::Break;

        if (decision != TraversalDecision::SkipChildrenAndContinue) {
            if (auto first_child = current->first_child()) {
                current = move(first_child);
                continue;
            }
        }
        if (current == root)
            break;

        if (auto next_sibling = current->next_sibling()) {
            current = move(next_sibling);
            continue;
        }

        while (current != root && !current->next_sibling())
            current = current->parent();
        if (current == root)
            break;

        current = current->next_sibling();
    }
    return TraversalDecision::Continue;
}

template<typename T>
class WEB_API RefCountedTreeNode {
public:
    RefPtr<T> parent() { return m_parent.strong_ref(); }
    RefPtr<T const> parent() const { return m_parent.strong_ref(); }

    bool has_children() const { return m_first_child; }

    T* first_child_ptr() { return m_first_child.ptr(); }
    T const* first_child_ptr() const { return m_first_child.ptr(); }

    T* last_child_ptr() { return m_last_child.ptr(); }
    T const* last_child_ptr() const { return m_last_child.ptr(); }

    T* next_sibling_ptr() { return m_next_sibling.ptr(); }
    T const* next_sibling_ptr() const { return m_next_sibling.ptr(); }

    T* previous_sibling_ptr() { return m_previous_sibling.ptr(); }
    T const* previous_sibling_ptr() const { return m_previous_sibling.ptr(); }

    RefPtr<T> first_child()
    {
        return m_first_child;
    }
    RefPtr<T const> first_child() const
    {
        return const_cast<RefCountedTreeNode&>(*this).first_child();
    }

    RefPtr<T> last_child()
    {
        return m_last_child.strong_ref();
    }
    RefPtr<T const> last_child() const
    {
        return const_cast<RefCountedTreeNode&>(*this).last_child();
    }

    RefPtr<T> next_sibling()
    {
        return m_next_sibling;
    }
    RefPtr<T const> next_sibling() const
    {
        return const_cast<RefCountedTreeNode&>(*this).next_sibling();
    }

    RefPtr<T> previous_sibling()
    {
        return m_previous_sibling.strong_ref();
    }
    RefPtr<T const> previous_sibling() const
    {
        return const_cast<RefCountedTreeNode&>(*this).previous_sibling();
    }

    size_t index() const
    {
        size_t index = 0;
        for (auto node = previous_sibling(); node; node = node->previous_sibling())
            ++index;
        return index;
    }

    T& root()
    {
        auto root = RefPtr<T> { static_cast<T&>(*this) };
        while (auto parent = root->parent())
            root = parent;
        return *root;
    }
    T const& root() const { return const_cast<RefCountedTreeNode*>(this)->root(); }

    bool is_ancestor_of(RefCountedTreeNode const& other) const
    {
        for (auto ancestor = other.parent(); ancestor; ancestor = ancestor->parent()) {
            if (ancestor.ptr() == static_cast<T const*>(this))
                return true;
        }
        return false;
    }

    bool is_inclusive_ancestor_of(RefCountedTreeNode const& other) const
    {
        return &other == this || is_ancestor_of(other);
    }

    bool contains(T const* other) const
    {
        return other && other->is_inclusive_descendant_of(*this);
    }

    bool is_descendant_of(RefCountedTreeNode const& other) const
    {
        return other.is_ancestor_of(*this);
    }

    bool is_inclusive_descendant_of(RefCountedTreeNode const& other) const
    {
        return other.is_inclusive_ancestor_of(*this);
    }

    bool is_following(RefCountedTreeNode const& other) const
    {
        for (auto* node = previous_in_pre_order(); node; node = node->previous_in_pre_order()) {
            if (node == &other)
                return true;
        }
        return false;
    }

    bool is_parent_of(RefCountedTreeNode const& other) const
    {
        for (auto child = first_child(); child; child = child->next_sibling()) {
            if (child.ptr() == static_cast<T const*>(&other))
                return true;
        }
        return false;
    }

    void append_child(NonnullRefPtr<T> node)
    {
        VERIFY(!node->parent());
        VERIFY(!node->next_sibling());
        VERIFY(!node->previous_sibling());

        auto previous_last_child = last_child();
        node->m_previous_sibling = previous_last_child;
        node->m_parent = static_cast<T&>(*this);
        m_last_child = node;

        if (previous_last_child)
            previous_last_child->m_next_sibling = move(node);
        else
            m_first_child = move(node);
    }

    void prepend_child(NonnullRefPtr<T> node)
    {
        VERIFY(!node->parent());
        VERIFY(!node->next_sibling());
        VERIFY(!node->previous_sibling());

        if (m_first_child)
            m_first_child->m_previous_sibling = node;
        node->m_next_sibling = m_first_child;
        node->m_parent = static_cast<T&>(*this);
        m_first_child = move(node);
        if (!m_last_child)
            m_last_child = m_first_child;
    }

    void insert_before(NonnullRefPtr<T> node, T* child)
    {
        if (!child)
            return append_child(move(node));

        VERIFY(!node->parent());
        VERIFY(!node->next_sibling());
        VERIFY(!node->previous_sibling());
        VERIFY(static_cast<RefCountedTreeNode<T>*>(child)->parent().ptr() == static_cast<T*>(this));

        auto previous_sibling = child->previous_sibling();
        node->m_previous_sibling = previous_sibling;
        node->m_next_sibling = *child;
        node->m_parent = static_cast<T&>(*this);

        if (previous_sibling)
            previous_sibling->m_next_sibling = node;
        else
            m_first_child = node;

        child->m_previous_sibling = move(node);
    }

    void insert_before(NonnullRefPtr<T> node, T& child)
    {
        insert_before(move(node), &child);
    }

    void remove_child(T& node)
    {
        RefPtr<T> self = static_cast<T&>(*this);
        RefPtr<T> child_to_remove = node;
        VERIFY(static_cast<RefCountedTreeNode<T>&>(node).parent() == self);

        auto previous_sibling = node.previous_sibling();
        auto next_sibling = node.next_sibling();
        if (previous_sibling) {
            VERIFY(previous_sibling->m_next_sibling == child_to_remove);
            previous_sibling->m_next_sibling = next_sibling;
        } else {
            VERIFY(m_first_child == child_to_remove);
            m_first_child = next_sibling;
        }

        if (next_sibling) {
            VERIFY(next_sibling->previous_sibling() == child_to_remove);
            next_sibling->m_previous_sibling = previous_sibling;
        } else {
            VERIFY(last_child() == child_to_remove);
            m_last_child = previous_sibling;
        }

        node.m_next_sibling.clear();
        node.m_previous_sibling.clear();
        node.m_parent.clear();
    }

    void replace_child(NonnullRefPtr<T> new_child, T& old_child)
    {
        VERIFY(&old_child != new_child.ptr());
        VERIFY(static_cast<RefCountedTreeNode<T>&>(old_child).parent().ptr() == static_cast<T*>(this));
        VERIFY(!new_child->parent());
        VERIFY(!new_child->next_sibling());
        VERIFY(!new_child->previous_sibling());

        auto previous_sibling = old_child.previous_sibling();
        auto next_sibling = old_child.next_sibling();
        RefPtr<T> old_child_ref = old_child;

        new_child->m_parent = static_cast<T&>(*this);
        new_child->m_previous_sibling = previous_sibling;
        new_child->m_next_sibling = next_sibling;

        if (previous_sibling)
            previous_sibling->m_next_sibling = new_child;
        else
            m_first_child = new_child;

        if (next_sibling)
            next_sibling->m_previous_sibling = new_child;
        else
            m_last_child = new_child;

        old_child_ref->m_next_sibling.clear();
        old_child_ref->m_previous_sibling.clear();
        old_child_ref->m_parent.clear();
    }

    void remove()
    {
        auto parent = this->parent();
        VERIFY(parent);
        parent->remove_child(static_cast<T&>(*this));
    }

    template<typename Callback>
    TraversalDecision for_each_in_inclusive_subtree(Callback callback) const
    {
        return traverse_ref_counted_preorder(RefPtr<T const> { static_cast<T const&>(*this) }, callback);
    }

    template<typename Callback>
    TraversalDecision for_each_in_inclusive_subtree(Callback callback)
    {
        return traverse_ref_counted_preorder(RefPtr<T> { static_cast<T&>(*this) }, callback);
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_inclusive_subtree_of_type(Callback callback)
    {
        return for_each_in_inclusive_subtree([callback = move(callback)](T& node) {
            if (auto* node_of_type = as_if<U>(node))
                return callback(*node_of_type);
            return TraversalDecision::Continue;
        });
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_inclusive_subtree_of_type(Callback callback) const
    {
        return for_each_in_inclusive_subtree([callback = move(callback)](T const& node) {
            if (auto const* node_of_type = as_if<U>(node))
                return callback(*node_of_type);
            return TraversalDecision::Continue;
        });
    }

    template<typename Callback>
    TraversalDecision for_each_in_subtree(Callback callback) const
    {
        for (auto child = first_child(); child; child = child->next_sibling()) {
            if (child->for_each_in_inclusive_subtree(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename Callback>
    TraversalDecision for_each_in_subtree(Callback callback)
    {
        for (auto child = first_child(); child; child = child->next_sibling()) {
            if (child->for_each_in_inclusive_subtree(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_subtree_of_type(Callback callback)
    {
        for (auto child = first_child(); child; child = child->next_sibling()) {
            if (child->template for_each_in_inclusive_subtree_of_type<U>(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_subtree_of_type(Callback callback) const
    {
        for (auto child = first_child(); child; child = child->next_sibling()) {
            if (child->template for_each_in_inclusive_subtree_of_type<U>(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename Callback>
    void for_each_child(Callback callback) const
    {
        return const_cast<RefCountedTreeNode&>(*this).for_each_child(move(callback));
    }

    template<typename Callback>
    void for_each_child(Callback callback)
    {
        for (auto node = first_child(); node; node = node->next_sibling()) {
            if (callback(*node) == IterationDecision::Break)
                return;
        }
    }

    template<typename U, typename Callback>
    void for_each_child_of_type(Callback callback)
    {
        for (auto node = first_child(); node; node = node->next_sibling()) {
            auto* node_of_type = as_if<U>(*node);
            if (!node_of_type)
                continue;
            if (callback(*node_of_type) == IterationDecision::Break)
                return;
        }
    }

    template<typename U, typename Callback>
    void for_each_child_of_type(Callback callback) const
    {
        return const_cast<RefCountedTreeNode&>(*this).template for_each_child_of_type<U>(move(callback));
    }

    template<typename U>
    U const* next_sibling_of_type() const
    {
        return const_cast<RefCountedTreeNode*>(this)->template next_sibling_of_type<U>();
    }

    template<typename U>
    U* next_sibling_of_type()
    {
        for (auto sibling = next_sibling(); sibling; sibling = sibling->next_sibling()) {
            if (auto* sibling_of_type = as_if<U>(*sibling))
                return sibling_of_type;
        }
        return nullptr;
    }

    template<typename U>
    U const* previous_sibling_of_type() const
    {
        return const_cast<RefCountedTreeNode*>(this)->template previous_sibling_of_type<U>();
    }

    template<typename U>
    U* previous_sibling_of_type()
    {
        for (auto sibling = previous_sibling(); sibling; sibling = sibling->previous_sibling()) {
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
        return const_cast<RefCountedTreeNode*>(this)->template first_child_of_type<U>();
    }

    template<typename U>
    U const* last_child_of_type() const
    {
        return const_cast<RefCountedTreeNode*>(this)->template last_child_of_type<U>();
    }

    template<typename U>
    U* first_child_of_type()
    {
        for (auto child = first_child(); child; child = child->next_sibling()) {
            if (auto* child_of_type = as_if<U>(*child))
                return child_of_type;
        }
        return nullptr;
    }

    template<typename U>
    U* last_child_of_type()
    {
        for (auto child = last_child(); child; child = child->previous_sibling()) {
            if (auto* child_of_type = as_if<U>(*child))
                return child_of_type;
        }
        return nullptr;
    }

    template<typename U>
    U const* first_ancestor_of_type() const
    {
        return const_cast<RefCountedTreeNode&>(*this).template first_ancestor_of_type<U>();
    }

    template<typename U>
    U* first_ancestor_of_type()
    {
        for (auto ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
            if (auto* ancestor_of_type = as_if<U>(*ancestor))
                return ancestor_of_type;
        }
        return nullptr;
    }

    template<typename Callback>
    void for_each_ancestor(Callback callback) const
    {
        for (auto ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
            if (callback(*ancestor) == IterationDecision::Break)
                return;
        }
    }

    T* next_in_pre_order()
    {
        if (auto child = first_child())
            return child.ptr();

        auto* node = static_cast<T*>(this);
        while (node) {
            if (auto next = node->next_sibling())
                return next.ptr();
            auto parent = static_cast<RefCountedTreeNode<T>*>(node)->parent();
            node = parent.ptr();
        }
        return nullptr;
    }

    T* next_in_pre_order(T const* stay_within)
    {
        if (auto child = first_child())
            return child.ptr();

        auto* node = static_cast<T*>(this);
        while (node) {
            if (node == stay_within)
                return nullptr;
            if (auto next = node->next_sibling())
                return next.ptr();
            auto parent = static_cast<RefCountedTreeNode<T>*>(node)->parent();
            node = parent.ptr();
        }
        return nullptr;
    }

    T const* next_in_pre_order() const
    {
        return const_cast<RefCountedTreeNode*>(this)->next_in_pre_order();
    }

    T const* next_in_pre_order(T const* stay_within) const
    {
        return const_cast<RefCountedTreeNode*>(this)->next_in_pre_order(stay_within);
    }

    T* previous_in_pre_order()
    {
        if (auto previous = previous_sibling()) {
            auto* node = previous.ptr();
            while (auto last_child = node->last_child())
                node = last_child.ptr();
            return node;
        }

        return parent().ptr();
    }

    T const* previous_in_pre_order() const
    {
        return const_cast<RefCountedTreeNode*>(this)->previous_in_pre_order();
    }

    bool is_before(RefCountedTreeNode const& other) const
    {
        if (this == &other)
            return false;
        for (auto* node = static_cast<T const*>(this); node; node = node->next_in_pre_order()) {
            if (node == &other)
                return true;
        }
        return false;
    }

    ~RefCountedTreeNode()
    {
        if (auto parent = this->parent())
            parent->remove_child(static_cast<T&>(*this));

        while (m_first_child) {
            auto child = m_first_child;
            m_first_child = child->m_next_sibling;
            if (m_first_child)
                m_first_child->m_previous_sibling.clear();
            child->m_next_sibling.clear();
            child->m_previous_sibling.clear();
            child->m_parent.clear();
        }
        m_last_child.clear();
    }

protected:
    RefCountedTreeNode() = default;

private:
    WeakPtr<T> m_parent;
    RefPtr<T> m_first_child;
    WeakPtr<T> m_last_child;
    RefPtr<T> m_next_sibling;
    WeakPtr<T> m_previous_sibling;
};

}
