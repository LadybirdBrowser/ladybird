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
    ElementType& operator*() { return m_node->value; }
    ElementType* operator->() { return &m_node->value; }
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

template<typename T>
class DoublyLinkedList {
private:
    struct Node {
        template<typename U>
        explicit Node(U&& v)
            : value(forward<U>(v))
        {
            static_assert(
                requires { T(v); }, "Conversion operator is missing.");
        }
        T value;
        Node* next { nullptr };
        Node* prev { nullptr };
    };

public:
    DoublyLinkedList() = default;
    ~DoublyLinkedList() { clear(); }

    [[nodiscard]] bool is_empty() const { return !m_head; }

    void clear()
    {
        for (auto* node = m_head; node;) {
            auto* next = node->next;
            delete node;
            node = next;
        }
        m_head = nullptr;
        m_tail = nullptr;
    }

    [[nodiscard]] T& first()
    {
        VERIFY(m_head);
        return m_head->value;
    }
    [[nodiscard]] T const& first() const
    {
        VERIFY(m_head);
        return m_head->value;
    }
    [[nodiscard]] T& last()
    {
        VERIFY(m_head);
        return m_tail->value;
    }
    [[nodiscard]] T const& last() const
    {
        VERIFY(m_head);
        return m_tail->value;
    }

    template<typename U>
    ErrorOr<void> try_append(U&& value)
    {
        static_assert(
            requires { T(value); }, "Conversion operator is missing.");
        auto* node = new (nothrow) Node(forward<U>(value));
        if (!node)
            return Error::from_errno(ENOMEM);
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
        auto* node = new (nothrow) Node(forward<U>(value));
        if (!node)
            return Error::from_errno(ENOMEM);
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

    template<typename U>
    void append(U&& value)
    {
        MUST(try_append(forward<U>(value)));
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

    void remove(Iterator it)
    {
        VERIFY(it.m_node);
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
        delete node;
    }

private:
    Node* m_head { nullptr };
    Node* m_tail { nullptr };
};

}

#if USING_AK_GLOBALLY
using AK::DoublyLinkedList;
#endif
