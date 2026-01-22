/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/Find.h>
#include <AK/StdLibExtras.h>

namespace AK {

template<typename ListType, typename ElementType>
class DoublyLinkedListIterator {
public:
    bool operator!=(DoublyLinkedListIterator const& other) const { return m_node != other.m_node; }
    bool operator==(DoublyLinkedListIterator const& other) const { return m_node == other.m_node; }
    DoublyLinkedListIterator& operator++()
    {
        m_node = m_node->next;
        return *this;
    }
    ElementType& operator*() { return m_node->value(); }
    ElementType* operator->() { return &m_node->value(); }
    [[nodiscard]] bool is_end() const { return !m_node; }
    static DoublyLinkedListIterator universal_end() { return DoublyLinkedListIterator(nullptr); }

private:
    friend ListType;
    explicit DoublyLinkedListIterator(typename ListType::Node* node)
        : m_node(node)
    {
    }
    typename ListType::Node* m_node;
};

template<typename T, size_t node_cache_size>
class DoublyLinkedList {
private:
    struct Node {
        template<typename... Args>
        requires(IsConstructible<T, Args...>)
        explicit Node(Args&&... args)
        {
            new (m_value) T(forward<Args>(args)...);
        }

        T const& value() const { return *bit_cast<T const*>(&m_value); }
        T& value() { return *bit_cast<T*>(&m_value); }

        Node* next { nullptr };
        Node* prev { nullptr };

    private:
        alignas(T) u8 m_value[sizeof(T)];
    };

public:
    DoublyLinkedList() = default;
    ~DoublyLinkedList()
    {
        clear();
        if constexpr (node_cache_size > 0) {
            for (size_t i = 0; i < m_node_cache.used_count; ++i)
                delete m_node_cache.nodes[i];
            m_node_cache.used_count = 0;
        }
    }

    [[nodiscard]] bool is_empty() const { return !m_head; }

    void clear()
    {
        for (auto* node = m_head; node;) {
            auto* next = node->next;
            drop_node(node);
            node = next;
        }
        m_head = nullptr;
        m_tail = nullptr;
        m_size = 0;
    }

    [[nodiscard]] T& first()
    {
        VERIFY(m_head);
        return m_head->value();
    }
    [[nodiscard]] T const& first() const
    {
        VERIFY(m_head);
        return m_head->value();
    }
    [[nodiscard]] T& last()
    {
        VERIFY(m_head);
        return m_tail->value();
    }
    [[nodiscard]] T const& last() const
    {
        VERIFY(m_head);
        return m_tail->value();
    }

    [[nodiscard]] T& unchecked_last()
    {
        return m_tail->value();
    }
    [[nodiscard]] T const& unchecked_last() const
    {
        return m_tail->value();
    }

    template<typename... Args>
    ALWAYS_INLINE ErrorOr<void> try_append(Args&&... args)
    {
        static_assert(
            requires { T(forward<Args>(args)...); }, "Initializer is missing.");
        auto* node = make_node(forward<Args>(args)...);
        if (!node)
            return Error::from_errno(ENOMEM);
        m_size += 1;
        if (!m_head) {
            VERIFY(!m_tail);
            m_head = node;
            m_tail = node;
            return {};
        }
        VERIFY(m_tail);
        VERIFY(!node->next);
        m_tail->next = node;
        node->prev = m_tail;
        m_tail = node;
        return {};
    }

    template<typename U>
    ErrorOr<void> try_prepend(U&& value)
    {
        static_assert(IsSame<T, U>);
        auto* node = make_node(forward<U>(value));
        if (!node)
            return Error::from_errno(ENOMEM);
        m_size += 1;
        if (!m_head) {
            VERIFY(!m_tail);
            m_head = node;
            m_tail = node;
            return {};
        }
        VERIFY(m_tail);
        VERIFY(!node->prev);
        m_head->prev = node;
        node->next = m_head;
        m_head = node;
        return {};
    }

    template<typename... Args>
    void append(Args&&... args)
    {
        MUST(try_append(forward<Args>(args)...));
    }

    template<typename U>
    void prepend(U&& value)
    {
        MUST(try_prepend(forward<U>(value)));
    }

    [[nodiscard]] bool contains_slow(T const& value) const
    {
        return find(value) != end();
    }

    using Iterator = DoublyLinkedListIterator<DoublyLinkedList, T>;
    friend Iterator;
    Iterator begin() { return Iterator(m_head); }
    Iterator end() { return Iterator::universal_end(); }

    using ConstIterator = DoublyLinkedListIterator<DoublyLinkedList const, T const>;
    friend ConstIterator;
    ConstIterator begin() const { return ConstIterator(m_head); }
    ConstIterator end() const { return ConstIterator::universal_end(); }

    ConstIterator find(T const& value) const
    {
        return AK::find(begin(), end(), value);
    }

    Iterator find(T const& value)
    {
        return AK::find(begin(), end(), value);
    }

    [[nodiscard]] Iterator remove(Iterator const& it)
    {
        VERIFY(it.m_node);
        auto next = it.m_node->next;
        auto* node = it.m_node;
        if (node->prev) {
            VERIFY(node != m_head);
            node->prev->next = node->next;
        } else {
            VERIFY(node == m_head);
            m_head = node->next;
        }
        if (node->next) {
            VERIFY(node != m_tail);
            node->next->prev = node->prev;
        } else {
            VERIFY(node == m_tail);
            m_tail = node->prev;
        }
        m_size -= 1;
        drop_node(node);
        return Iterator(next);
    }

    T take_first()
    {
        VERIFY(m_head);
        auto value = move(m_head->value());
        auto* old_head = m_head;
        m_head = m_head->next;
        if (m_head)
            m_head->prev = nullptr;
        else
            m_tail = nullptr; // We removed the only element, no more elements left.
        drop_node(old_head);
        m_size -= 1;
        return value;
    }

    T take_last()
    {
        VERIFY(m_tail);
        auto value = move(m_tail->value());
        auto* old_tail = m_tail;
        m_tail = m_tail->prev;
        if (m_tail)
            m_tail->next = nullptr;
        else
            m_head = nullptr; // We removed the only element, no more elements left.
        drop_node(old_tail);
        m_size -= 1;
        return value;
    }

    size_t size() const { return m_size; }

    template<typename F>
    void ensure_capacity(size_t new_capacity, F make_default_value = [] -> T { return T {}; })
    {
        if constexpr (node_cache_size == 0)
            return;

        if (m_size >= new_capacity)
            return;
        auto const rest = min(new_capacity - m_size, node_cache_size);
        for (size_t i = m_node_cache.used_count; i <= rest; ++i)
            m_node_cache.nodes[m_node_cache.used_count++] = make_node(make_default_value());
    }

private:
    void drop_node(Node* node)
    {
        if constexpr (node_cache_size > 0) {
            if (m_node_cache.used_count + 1 < node_cache_size) {
                node->value().~T();
                m_node_cache.nodes[m_node_cache.used_count++] = node;
                return;
            }
        }

        node->value().~T();
        delete node;
    }

    template<typename... Args>
    ALWAYS_INLINE Node* make_node(Args&&... args)
    {
        if constexpr (node_cache_size > 0) {
            if (m_node_cache.used_count > 0) {
                auto* node = m_node_cache.nodes.data()[--m_node_cache.used_count];
                new (node) Node(forward<Args>(args)...);
                return node;
            }
        }

        return new (nothrow) Node(forward<Args>(args)...);
    }

    Node* m_head { nullptr };
    Node* m_tail { nullptr };
    size_t m_size { 0 };

    struct NonemptyNodeCache {
        Array<Node*, node_cache_size> nodes;
        size_t used_count { 0 };
    };
    using NodeCache = Conditional<(node_cache_size > 0), NonemptyNodeCache, Empty>;

    NO_UNIQUE_ADDRESS NodeCache m_node_cache;
};

}

#if USING_AK_GLOBALLY
using AK::DoublyLinkedList;
#endif
