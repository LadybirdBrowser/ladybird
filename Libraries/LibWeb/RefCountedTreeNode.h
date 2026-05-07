/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/IterationDecision.h>
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

    void remove_child(T& node)
    {
        RefPtr<T> self = static_cast<T&>(*this);
        RefPtr<T> child_to_remove = node;
        VERIFY(node.parent() == self);

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
    RefPtr<U const> first_ancestor_of_type() const
    {
        return const_cast<RefCountedTreeNode&>(*this).template first_ancestor_of_type<U>();
    }

    template<typename U>
    RefPtr<U> first_ancestor_of_type()
    {
        for (auto ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
            if (auto* ancestor_of_type = as_if<U>(*ancestor))
                return *ancestor_of_type;
        }
        return nullptr;
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
