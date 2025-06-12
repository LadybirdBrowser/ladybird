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
#include <AK/Traits.h>
#include <AK/Types.h>

namespace AK {

template<typename ListType, typename ElementType>
class SinglyLinkedListIterator {
public:
    SinglyLinkedListIterator() = default;
    bool operator!=(SinglyLinkedListIterator const& other) const { return m_node != other.m_node; }
    SinglyLinkedListIterator& operator++()
    {
        if (m_removed)
            m_removed = false;
        else
            m_prev = m_node;
        m_node = m_next;
        if (m_next)
            m_next = m_next->next;
        return *this;
    }
    ElementType& operator*()
    {
        VERIFY(!m_removed);
        return m_node->value;
    }
    ElementType* operator->()
    {
        VERIFY(!m_removed);
        return &m_node->value;
    }
    bool is_end() const { return !m_node; }
    bool is_begin() const { return !m_prev; }
    void remove(ListType& list)
    {
        m_removed = true;
        list.remove(*this);
    }

private:
    friend ListType;
    explicit SinglyLinkedListIterator(typename ListType::Node* node, typename ListType::Node* prev = nullptr)
        : m_node(node)
        , m_prev(prev)
        , m_next(node ? node->next : nullptr)
    {
    }
    typename ListType::Node* m_node { nullptr };
    typename ListType::Node* m_prev { nullptr };
    typename ListType::Node* m_next { nullptr };
    bool m_removed { false };
};

template<typename T, typename TSizeCalculationPolicy>
class SinglyLinkedList {
private:
    struct Node {
        explicit Node(T&& v)
            : value(move(v))
        {
        }
        explicit Node(T const& v)
            : value(v)
        {
        }
        T value;
        Node* next { nullptr };
    };

public:
    SinglyLinkedList() = default;
    SinglyLinkedList(SinglyLinkedList const& other) = delete;
    SinglyLinkedList(SinglyLinkedList&& other)
        : m_head(other.m_head)
        , m_tail(other.m_tail)
    {
        other.m_head = nullptr;
        other.m_tail = nullptr;
    }
    SinglyLinkedList& operator=(SinglyLinkedList const& other) = delete;
    SinglyLinkedList& operator=(SinglyLinkedList&&) = delete;

    ~SinglyLinkedList() { clear(); }

    bool is_empty() const { return !head(); }

    inline size_t size() const
    {
        return m_size_policy.size(m_head);
    }

    void clear()
    {
        for (auto* node = m_head; node;) {
            auto* next = node->next;
            delete node;
            node = next;
        }
        m_head = nullptr;
        m_tail = nullptr;
        m_size_policy.reset();
    }

    T& first()
    {
        VERIFY(head());
        return head()->value;
    }
    T const& first() const
    {
        VERIFY(head());
        return head()->value;
    }
    T& last()
    {
        VERIFY(head());
        return tail()->value;
    }
    T const& last() const
    {
        VERIFY(head());
        return tail()->value;
    }

    T take_first()
    {
        VERIFY(m_head);
        auto* prev_head = m_head;
        T value = move(first());
        if (m_tail == m_head)
            m_tail = nullptr;
        m_head = m_head->next;
        m_size_policy.decrease_size(value);
        delete prev_head;
        return value;
    }

    template<typename U = T>
    ErrorOr<void> try_append(U&& value)
    {
        auto* node = new (nothrow) Node(forward<U>(value));
        if (!node)
            return Error::from_errno(ENOMEM);
        m_size_policy.increase_size(value);
        if (!m_head) {
            m_head = node;
            m_tail = node;
            return {};
        }
        m_tail->next = node;
        m_tail = node;
        return {};
    }

    template<typename U = T>
    ErrorOr<void> try_prepend(U&& value)
    {
        auto* node = new (nothrow) Node(forward<U>(value));
        if (!node)
            return Error::from_errno(ENOMEM);
        m_size_policy.increase_size(value);
        if (!m_head) {
            m_head = node;
            m_tail = node;
            return {};
        }
        node->next = m_head;
        m_head = node;
        return {};
    }

    template<typename U = T>
    void append(U&& value)
    {
        MUST(try_append(forward<U>(value)));
    }

    template<typename U = T>
    void prepend(U&& value)
    {
        MUST(try_prepend(forward<U>(value)));
    }

    bool contains_slow(T const& value) const
    {
        return find(value) != end();
    }

    using Iterator = SinglyLinkedListIterator<SinglyLinkedList, T>;
    friend Iterator;
    Iterator begin() { return Iterator(m_head); }
    Iterator end() { return {}; }

    using ConstIterator = SinglyLinkedListIterator<SinglyLinkedList const, T const>;
    friend ConstIterator;
    ConstIterator begin() const { return ConstIterator(m_head); }
    ConstIterator end() const { return {}; }

    template<typename TUnaryPredicate>
    ConstIterator find_if(TUnaryPredicate&& pred) const
    {
        return AK::find_if(begin(), end(), forward<TUnaryPredicate>(pred));
    }

    template<typename TUnaryPredicate>
    Iterator find_if(TUnaryPredicate&& pred)
    {
        return AK::find_if(begin(), end(), forward<TUnaryPredicate>(pred));
    }

    ConstIterator find(T const& value) const
    {
        return find_if([&](auto& entry) { return Traits<T>::equals(entry, value); });
    }

    Iterator find(T const& value)
    {
        return find_if([&](auto& entry) { return Traits<T>::equals(entry, value); });
    }

    template<typename U = T>
    ErrorOr<void> try_insert_before(Iterator iterator, U&& value)
    {
        auto* node = new (nothrow) Node(forward<U>(value));
        if (!node)
            return Error::from_errno(ENOMEM);
        m_size_policy.increase_size(value);
        node->next = iterator.m_node;
        if (m_head == iterator.m_node)
            m_head = node;
        if (iterator.m_prev)
            iterator.m_prev->next = node;
        return {};
    }

    template<typename U = T>
    ErrorOr<void> try_insert_after(Iterator iterator, U&& value)
    {
        if (iterator.is_end())
            return try_append(value);

        auto* node = new (nothrow) Node(forward<U>(value));
        if (!node)
            return Error::from_errno(ENOMEM);
        m_size_policy.increase_size(value);
        node->next = iterator.m_node->next;

        iterator.m_node->next = node;

        if (m_tail == iterator.m_node)
            m_tail = node;
        return {};
    }

    template<typename U = T>
    void insert_before(Iterator iterator, U&& value)
    {
        MUST(try_insert_before(iterator, forward<U>(value)));
    }

    template<typename U = T>
    void insert_after(Iterator iterator, U&& value)
    {
        MUST(try_insert_after(iterator, forward<U>(value)));
    }

    void remove(Iterator& iterator)
    {
        VERIFY(!iterator.is_end());
        if (m_head == iterator.m_node)
            m_head = iterator.m_node->next;
        if (m_tail == iterator.m_node)
            m_tail = iterator.m_prev;
        if (iterator.m_prev)
            iterator.m_prev->next = iterator.m_node->next;
        m_size_policy.decrease_size(iterator.m_node->value);
        delete iterator.m_node;
    }

private:
    Node* head() { return m_head; }
    Node const* head() const { return m_head; }

    Node* tail() { return m_tail; }
    Node const* tail() const { return m_tail; }

    Node* m_head { nullptr };
    Node* m_tail { nullptr };
    TSizeCalculationPolicy m_size_policy {};
};

}

#if USING_AK_GLOBALLY
using AK::SinglyLinkedList;
#endif
