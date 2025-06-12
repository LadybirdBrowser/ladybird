/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveDetails.h>
#include <AK/RedBlackTree.h>

namespace AK::Detail {

template<Integral K, typename V, typename Container = RawPtr<V>>
class IntrusiveRedBlackTreeNode;

struct ExtractIntrusiveRedBlackTreeTypes {
    template<typename K, typename V, typename Container, typename T>
    static K key(IntrusiveRedBlackTreeNode<K, V, Container> T::* x);
    template<typename K, typename V, typename Container, typename T>
    static V value(IntrusiveRedBlackTreeNode<K, V, Container> T::* x);
    template<typename K, typename V, typename Container, typename T>
    static Container container(IntrusiveRedBlackTreeNode<K, V, Container> T::* x);
};

template<Integral K, typename V, typename Container = RawPtr<V>>
using SubstitutedIntrusiveRedBlackTreeNode = IntrusiveRedBlackTreeNode<K, V, typename SubstituteIntrusiveContainerType<V, Container>::Type>;

template<Integral K, typename V, typename Container, SubstitutedIntrusiveRedBlackTreeNode<K, V, Container> V::* member>
class IntrusiveRedBlackTree : public BaseRedBlackTree<K> {

public:
    IntrusiveRedBlackTree() = default;
    virtual ~IntrusiveRedBlackTree() override
    {
        clear();
    }

    using BaseTree = BaseRedBlackTree<K>;
    using TreeNode = SubstitutedIntrusiveRedBlackTreeNode<K, V, Container>;

    Container find(K key)
    {
        auto* node = static_cast<TreeNode*>(BaseTree::find(this->m_root, key));
        if (!node)
            return nullptr;
        return node_to_value(*node);
    }

    Container find_largest_not_above(K key)
    {
        auto* node = static_cast<TreeNode*>(BaseTree::find_largest_not_above(this->m_root, key));
        if (!node)
            return nullptr;
        return node_to_value(*node);
    }

    Container find_smallest_not_below(K key)
    {
        auto* node = static_cast<TreeNode*>(BaseTree::find_smallest_not_below(this->m_root, key));
        if (!node)
            return nullptr;
        return node_to_value(*node);
    }

    void insert(K key, V& value)
    {
        auto& node = value.*member;
        VERIFY(!node.m_in_tree);
        static_cast<typename BaseTree::Node&>(node).key = key;
        BaseTree::insert(&node);
        if constexpr (!TreeNode::IsRaw)
            node.m_self.reference = &value; // Note: Self-reference ensures that the object will keep a ref to itself when the Container is a smart pointer.
        node.m_in_tree = true;
    }

    template<typename ElementType>
    class BaseIterator {
    public:
        BaseIterator() = default;
        bool operator!=(BaseIterator const& other) const { return m_node != other.m_node; }
        BaseIterator& operator++()
        {
            if (!m_node)
                return *this;
            m_prev = m_node;
            // the complexity is O(logn) for each successor call, but the total complexity for all elements comes out to O(n), meaning the amortized cost for a single call is O(1)
            m_node = static_cast<TreeNode*>(BaseTree::successor(m_node));
            return *this;
        }
        BaseIterator& operator--()
        {
            if (!m_prev)
                return *this;
            m_node = m_prev;
            m_prev = static_cast<TreeNode*>(BaseTree::predecessor(m_prev));
            return *this;
        }
        ElementType& operator*()
        {
            VERIFY(m_node);
            return *node_to_value(*m_node);
        }
        auto operator->()
        {
            VERIFY(m_node);
            return node_to_value(*m_node);
        }
        [[nodiscard]] bool is_end() const { return !m_node; }
        [[nodiscard]] bool is_begin() const { return !m_prev; }
        [[nodiscard]] auto key() const { return m_node->key; }

    private:
        friend class IntrusiveRedBlackTree;
        explicit BaseIterator(TreeNode* node, TreeNode* prev = nullptr)
            : m_node(node)
            , m_prev(prev)
        {
        }
        TreeNode* m_node { nullptr };
        TreeNode* m_prev { nullptr };
    };

    using Iterator = BaseIterator<V>;
    Iterator begin() { return Iterator(static_cast<TreeNode*>(this->m_minimum)); }
    Iterator end() { return {}; }
    Iterator begin_from(K key) { return Iterator(static_cast<TreeNode*>(BaseTree::find(this->m_root, key))); }
    Iterator begin_from(V& value) { return Iterator(&(value.*member)); }

    using ConstIterator = BaseIterator<V const>;
    ConstIterator begin() const { return ConstIterator(static_cast<TreeNode*>(this->m_minimum)); }
    ConstIterator end() const { return {}; }
    ConstIterator begin_from(K key) const { return ConstIterator(static_cast<TreeNode*>(BaseTree::find(this->m_rootF, key))); }
    ConstIterator begin_from(V const& value) const { return Iterator(&(value.*member)); }

    bool remove(K key)
    {
        auto* node = static_cast<TreeNode*>(BaseTree::find(this->m_root, key));
        if (!node)
            return false;

        BaseTree::remove(node);

        node->right_child = nullptr;
        node->left_child = nullptr;
        node->m_in_tree = false;
        if constexpr (!TreeNode::IsRaw)
            node->m_self.reference = nullptr;

        return true;
    }

    void clear()
    {
        clear_nodes(static_cast<TreeNode*>(this->m_root));
        this->m_root = nullptr;
        this->m_minimum = nullptr;
        this->m_size = 0;
    }

private:
    static void clear_nodes(TreeNode* node)
    {
        if (!node)
            return;
        clear_nodes(static_cast<TreeNode*>(node->right_child));
        node->right_child = nullptr;
        clear_nodes(static_cast<TreeNode*>(node->left_child));
        node->left_child = nullptr;
        node->m_in_tree = false;
        if constexpr (!TreeNode::IsRaw)
            node->m_self.reference = nullptr;
    }

    static V* node_to_value(TreeNode& node)
    {
#ifdef AK_OS_WINDOWS
        // NOTE: https://learn.microsoft.com/en-us/cpp/build/reference/vmb-vmg-representation-method?view=msvc-170
        static_assert(sizeof(member) == 4);
        auto distance = bit_cast<u8*>(static_cast<FlatPtr>(bit_cast<u32>(member)));
        return bit_cast<V*>(bit_cast<u8*>(&node) - distance);
#else
        return bit_cast<V*>(bit_cast<u8*>(&node) - bit_cast<u8*>(member));
#endif
    }
};

template<Integral K, typename V, typename Container>
class IntrusiveRedBlackTreeNode : public BaseRedBlackTree<K>::Node {
public:
    ~IntrusiveRedBlackTreeNode()
    {
        VERIFY(!is_in_tree());
    }

    [[nodiscard]] bool is_in_tree() const
    {
        return m_in_tree;
    }

    [[nodiscard]] K key() const
    {
        return BaseRedBlackTree<K>::Node::key;
    }

    static constexpr bool IsRaw = IsPointer<Container>;

#if !defined(AK_COMPILER_CLANG)
private:
    template<Integral TK, typename TV, typename TContainer, SubstitutedIntrusiveRedBlackTreeNode<TK, TV, TContainer> TV::* member>
    friend class ::AK::Detail::IntrusiveRedBlackTree;
#endif

    bool m_in_tree { false };
    NO_UNIQUE_ADDRESS SelfReferenceIfNeeded<Container, IsRaw> m_self;
};

// Specialise IntrusiveRedBlackTree for NonnullRefPtr
// By default, red black trees cannot contain null entries anyway, so switch to RefPtr
// and just make the user-facing functions deref the pointers.
template<Integral K, typename V, SubstitutedIntrusiveRedBlackTreeNode<K, V, NonnullRefPtr<V>> V::* member>
class IntrusiveRedBlackTree<K, V, NonnullRefPtr<V>, member> : public IntrusiveRedBlackTree<K, V, RefPtr<V>, member> {
public:
    [[nodiscard]] NonnullRefPtr<V> find(K key) const { return IntrusiveRedBlackTree<K, V, RefPtr<V>, member>::find(key).release_nonnull(); }
    [[nodiscard]] NonnullRefPtr<V> find_largest_not_above(K key) const { return IntrusiveRedBlackTree<K, V, RefPtr<V>, member>::find_largest_not_above(key).release_nonnull(); }
    [[nodiscard]] NonnullRefPtr<V> find_smallest_not_below(K key) const { return IntrusiveRedBlackTree<K, V, RefPtr<V>, member>::find_smallest_not_below(key).release_nonnull(); }
};

}

namespace AK {

template<Integral K, typename V, typename Container = RawPtr<K>>
using IntrusiveRedBlackTreeNode = Detail::SubstitutedIntrusiveRedBlackTreeNode<K, V, Container>;

template<auto member>
using IntrusiveRedBlackTree = Detail::IntrusiveRedBlackTree<
    decltype(Detail::ExtractIntrusiveRedBlackTreeTypes::key(member)),
    decltype(Detail::ExtractIntrusiveRedBlackTreeTypes::value(member)),
    decltype(Detail::ExtractIntrusiveRedBlackTreeTypes::container(member)),
    member>;

}

#if USING_AK_GLOBALLY
using AK::IntrusiveRedBlackTree;
using AK::IntrusiveRedBlackTreeNode;
#endif
