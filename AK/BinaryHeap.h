/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Vector.h>

namespace AK {

template<typename Node, typename Comparator, typename IndexSetter, size_t inline_capacity = 0>
class IntrusiveBinaryHeap {
    AK_MAKE_DEFAULT_COPYABLE(IntrusiveBinaryHeap);
    AK_MAKE_DEFAULT_MOVABLE(IntrusiveBinaryHeap);

public:
    IntrusiveBinaryHeap() = default;

    IntrusiveBinaryHeap(Vector<Node, inline_capacity>&& nodes)
        : m_nodes(move(nodes))
    {
        for (ssize_t i = m_nodes.size() / 2; i--;)
            heapify_down(i);
    }

    [[nodiscard]] size_t size() const { return m_nodes.size(); }
    [[nodiscard]] bool is_empty() const { return m_nodes.is_empty(); }

    void insert(Node const& node)
    {
        m_nodes.append(node);
        IndexSetter {}(m_nodes.last(), m_nodes.size() - 1);
        heapify_up(m_nodes.size() - 1);
    }

    void insert(Node&& node)
    {
        m_nodes.append(move(node));
        IndexSetter {}(m_nodes.last(), m_nodes.size() - 1);
        heapify_up(m_nodes.size() - 1);
    }

    Node pop(size_t i)
    {
        while (i != 0) {
            swap_indices(i, (i - 1) / 2);
            i = (i - 1) / 2;
        }
        swap_indices(0, m_nodes.size() - 1);
        Node node = m_nodes.take_last();
        heapify_down(0);
        return node;
    }

    Node pop_min()
    {
        return pop(0);
    }

    Node const& peek_min() const
    {
        return m_nodes[0];
    }

    void clear()
    {
        m_nodes.clear();
    }

    ReadonlySpan<Node> nodes_in_arbitrary_order() const
    {
        return m_nodes;
    }

private:
    void swap_indices(size_t i, size_t j)
    {
        swap(m_nodes[i], m_nodes[j]);
        IndexSetter {}(m_nodes[i], i);
        IndexSetter {}(m_nodes[j], j);
    }

    bool compare_indices(size_t i, size_t j)
    {
        return Comparator {}(m_nodes[i], m_nodes[j]);
    }

    void heapify_up(size_t i)
    {
        while (i != 0) {
            auto parent = (i - 1) / 2;
            if (compare_indices(parent, i))
                break;
            swap_indices(i, parent);
            i = parent;
        }
    }

    void heapify_down(size_t i)
    {
        while (i * 2 + 1 < size()) {
            size_t min_child = i * 2 + 1;
            size_t other_child = i * 2 + 2;
            if (other_child < size() && compare_indices(other_child, min_child))
                min_child = other_child;
            if (compare_indices(i, min_child))
                break;
            swap_indices(i, min_child);
            i = min_child;
        }
    }

    Vector<Node, inline_capacity> m_nodes;
};

template<typename K, typename V, size_t inline_capacity>
class BinaryHeap {
public:
    BinaryHeap() = default;
    ~BinaryHeap() = default;

    // This constructor allows for O(n) construction of the heap (instead of O(nlogn) for repeated insertions)
    BinaryHeap(K keys[], V values[], size_t size)
    {
        Vector<Node, inline_capacity> nodes;
        nodes.ensure_capacity(size);
        for (size_t i = 0; i < size; i++)
            nodes.unchecked_append({ keys[i], values[i] });
        m_heap = decltype(m_heap) { move(nodes) };
    }

    [[nodiscard]] size_t size() const { return m_heap.size(); }
    [[nodiscard]] bool is_empty() const { return m_heap.is_empty(); }

    void insert(K key, V value)
    {
        m_heap.insert({ key, value });
    }

    V pop_min()
    {
        return m_heap.pop_min().value;
    }

    [[nodiscard]] V const& peek_min() const
    {
        return m_heap.peek_min().value;
    }

    [[nodiscard]] K const& peek_min_key() const
    {
        return m_heap.peek_min().key;
    }

    void clear()
    {
        m_heap.clear();
    }

private:
    struct Node {
        K key;
        V value;
    };

    IntrusiveBinaryHeap<
        Node,
        decltype([](Node const& a, Node const& b) { return a.key < b.key; }),
        decltype([](Node&, size_t) {})>
        m_heap;
};

}

#if USING_AK_GLOBALLY
using AK::BinaryHeap;
using AK::IntrusiveBinaryHeap;
#endif
