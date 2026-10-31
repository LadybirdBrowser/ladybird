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

}

#if USING_AK_GLOBALLY
using AK::IntrusiveBinaryHeap;
#endif
