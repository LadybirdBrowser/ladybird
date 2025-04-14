/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/BitCast.h>
#include <AK/Forward.h>
#include <AK/IntrusiveDetails.h>
#include <AK/Noncopyable.h>
#include <AK/StdLibExtras.h>

namespace AK::Detail {

template<typename T, typename Container = RawPtr<T>>
class IntrusiveListNode;

struct ExtractIntrusiveListTypes {
    template<typename V, typename Container, typename T>
    static V value(IntrusiveListNode<V, Container> T::* x);
    template<typename V, typename Container, typename T>
    static Container container(IntrusiveListNode<V, Container> T::* x);
};

template<typename T, typename Container = RawPtr<T>>
using SubstitutedIntrusiveListNode = IntrusiveListNode<T, typename Detail::SubstituteIntrusiveContainerType<T, Container>::Type>;

template<typename T, typename Container>
class IntrusiveListStorage {
private:
    friend class IntrusiveListNode<T, Container>;

    template<class T_, typename Container_, SubstitutedIntrusiveListNode<T_, Container_> T_::* member>
    friend class IntrusiveList;

    SubstitutedIntrusiveListNode<T, Container>* m_first { nullptr };
    SubstitutedIntrusiveListNode<T, Container>* m_last { nullptr };
};

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
class IntrusiveList {
    AK_MAKE_NONCOPYABLE(IntrusiveList);
    AK_MAKE_NONMOVABLE(IntrusiveList);

public:
    IntrusiveList() = default;
    ~IntrusiveList();

    void clear();
    [[nodiscard]] bool is_empty() const;
    [[nodiscard]] size_t size_slow() const;
    void append(T& n);
    void prepend(T& n);
    void insert_before(T&, T&);
    void remove(T& n);
    [[nodiscard]] bool contains(T const&) const;
    [[nodiscard]] Container first() const;
    [[nodiscard]] Container last() const;

    [[nodiscard]] Container take_first();
    [[nodiscard]] Container take_last();

    class Iterator {
    public:
        Iterator() = default;
        Iterator(T* value)
            : m_value(move(value))
        {
        }

        T const& operator*() const { return *m_value; }
        auto operator->() const { return m_value; }
        T& operator*() { return *m_value; }
        auto operator->() { return m_value; }
        bool operator==(Iterator const& other) const { return other.m_value == m_value; }
        Iterator& operator++()
        {
            m_value = IntrusiveList<T, Container, member>::next(m_value);
            return *this;
        }
        Iterator& erase();

    private:
        T* m_value { nullptr };
    };

    Iterator begin();
    Iterator end() { return Iterator {}; }

    class ReverseIterator {
    public:
        ReverseIterator() = default;
        ReverseIterator(T* value)
            : m_value(move(value))
        {
        }

        T const& operator*() const { return *m_value; }
        auto operator->() const { return m_value; }
        T& operator*() { return *m_value; }
        auto operator->() { return m_value; }
        bool operator==(ReverseIterator const& other) const { return other.m_value == m_value; }
        ReverseIterator& operator++()
        {
            m_value = IntrusiveList<T, Container, member>::prev(m_value);
            return *this;
        }

    private:
        T* m_value { nullptr };
    };

    ReverseIterator rbegin();
    ReverseIterator rend() { return ReverseIterator {}; }

    class ConstIterator {
    public:
        ConstIterator() = default;
        ConstIterator(T const* value)
            : m_value(value)
        {
        }

        T const& operator*() const { return *m_value; }
        auto operator->() const { return m_value; }
        bool operator==(ConstIterator const& other) const { return other.m_value == m_value; }
        ConstIterator& operator++()
        {
            m_value = IntrusiveList<T, Container, member>::next(m_value);
            return *this;
        }

    private:
        T const* m_value { nullptr };
    };

    ConstIterator begin() const;
    ConstIterator end() const { return ConstIterator {}; }

private:
    static T* next(T* current);
    static T* prev(T* current);
    static T const* next(T const* current);
    static T const* prev(T const* current);
    static T* node_to_value(SubstitutedIntrusiveListNode<T, Container>& node);
    IntrusiveListStorage<T, Container> m_storage;
};

template<typename T, typename Container>
class IntrusiveListNode {
public:
    ~IntrusiveListNode();
    void remove();
    bool is_in_list() const;

    static constexpr bool IsRaw = IsPointer<Container>;

    // Note: For some reason, clang does not consider `member` as declared here, and as declared above (`SubstitutedIntrusiveListNode<T, Container> T::*`)
    //       to be of equal types. so for now, just make the members public on clang.
#if !defined(AK_COMPILER_CLANG)
private:
    template<class T_, typename Container_, SubstitutedIntrusiveListNode<T_, Container_> T_::* member>
    friend class ::AK::Detail::IntrusiveList;
#endif

    IntrusiveListStorage<T, Container>* m_storage = nullptr;
    SubstitutedIntrusiveListNode<T, Container>* m_next = nullptr;
    SubstitutedIntrusiveListNode<T, Container>* m_prev = nullptr;
    NO_UNIQUE_ADDRESS SelfReferenceIfNeeded<Container, IsRaw> m_self;
};

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline typename IntrusiveList<T, Container, member>::Iterator& IntrusiveList<T, Container, member>::Iterator::erase()
{
    auto old = m_value;
    m_value = IntrusiveList<T, Container, member>::next(m_value);
    (old->*member).remove();
    return *this;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline IntrusiveList<T, Container, member>::~IntrusiveList()
{
    clear();
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline void IntrusiveList<T, Container, member>::clear()
{
    while (m_storage.m_first)
        m_storage.m_first->remove();
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline bool IntrusiveList<T, Container, member>::is_empty() const
{
    return m_storage.m_first == nullptr;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline size_t IntrusiveList<T, Container, member>::size_slow() const
{
    size_t size = 0;
    auto it_end = end();
    for (auto it = begin(); it != it_end; ++it) {
        ++size;
    }
    return size;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline void IntrusiveList<T, Container, member>::append(T& n)
{
    remove(n);

    auto& nnode = n.*member;
    nnode.m_storage = &m_storage;
    nnode.m_prev = m_storage.m_last;
    nnode.m_next = nullptr;
    if constexpr (!RemoveReference<decltype(nnode)>::IsRaw)
        nnode.m_self.reference = &n; // Note: Self-reference ensures that the object will keep a ref to itself when the Container is a smart pointer.

    if (m_storage.m_last)
        m_storage.m_last->m_next = &nnode;
    m_storage.m_last = &nnode;
    if (!m_storage.m_first)
        m_storage.m_first = &nnode;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline void IntrusiveList<T, Container, member>::prepend(T& n)
{
    remove(n);

    auto& nnode = n.*member;
    nnode.m_storage = &m_storage;
    nnode.m_prev = nullptr;
    nnode.m_next = m_storage.m_first;
    if constexpr (!RemoveReference<decltype(nnode)>::IsRaw)
        nnode.m_self.reference = &n;

    if (m_storage.m_first)
        m_storage.m_first->m_prev = &nnode;
    m_storage.m_first = &nnode;
    if (!m_storage.m_last)
        m_storage.m_last = &nnode;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline void IntrusiveList<T, Container, member>::insert_before(T& bn, T& n)
{
    remove(n);

    auto& new_node = n.*member;
    auto& before_node = bn.*member;
    new_node.m_storage = &m_storage;
    new_node.m_next = &before_node;
    new_node.m_prev = before_node.m_prev;
    if (before_node.m_prev)
        before_node.m_prev->m_next = &new_node;
    before_node.m_prev = &new_node;

    if (m_storage.m_first == &before_node) {
        m_storage.m_first = &new_node;
    }

    if constexpr (!RemoveReference<decltype(new_node)>::IsRaw)
        new_node.m_self.reference = &n;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline void IntrusiveList<T, Container, member>::remove(T& n)
{
    auto& nnode = n.*member;
    if (nnode.m_storage)
        nnode.remove();
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline bool IntrusiveList<T, Container, member>::contains(T const& n) const
{
    auto& nnode = n.*member;
    return nnode.m_storage == &m_storage;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline Container IntrusiveList<T, Container, member>::first() const
{
    return m_storage.m_first ? node_to_value(*m_storage.m_first) : nullptr;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline Container IntrusiveList<T, Container, member>::take_first()
{
    if (Container ptr = first()) {
        remove(*ptr);
        return ptr;
    }
    return nullptr;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline Container IntrusiveList<T, Container, member>::take_last()
{
    if (Container ptr = last()) {
        remove(*ptr);
        return ptr;
    }
    return nullptr;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline Container IntrusiveList<T, Container, member>::last() const
{
    return m_storage.m_last ? node_to_value(*m_storage.m_last) : nullptr;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline T const* IntrusiveList<T, Container, member>::next(T const* current)
{
    auto& nextnode = (current->*member).m_next;
    T const* nextstruct = nextnode ? node_to_value(*nextnode) : nullptr;
    return nextstruct;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline T const* IntrusiveList<T, Container, member>::prev(T const* current)
{
    auto& prevnode = (current->*member).m_prev;
    T const* prevstruct = prevnode ? node_to_value(*prevnode) : nullptr;
    return prevstruct;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline T* IntrusiveList<T, Container, member>::next(T* current)
{
    auto& nextnode = (current->*member).m_next;
    T* nextstruct = nextnode ? node_to_value(*nextnode) : nullptr;
    return nextstruct;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline T* IntrusiveList<T, Container, member>::prev(T* current)
{
    auto& prevnode = (current->*member).m_prev;
    T* prevstruct = prevnode ? node_to_value(*prevnode) : nullptr;
    return prevstruct;
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline typename IntrusiveList<T, Container, member>::Iterator IntrusiveList<T, Container, member>::begin()
{
    return m_storage.m_first ? Iterator(node_to_value(*m_storage.m_first)) : Iterator();
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline typename IntrusiveList<T, Container, member>::ReverseIterator IntrusiveList<T, Container, member>::rbegin()
{
    return m_storage.m_last ? ReverseIterator(node_to_value(*m_storage.m_last)) : ReverseIterator();
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline typename IntrusiveList<T, Container, member>::ConstIterator IntrusiveList<T, Container, member>::begin() const
{
    return m_storage.m_first ? ConstIterator(node_to_value(*m_storage.m_first)) : ConstIterator();
}

template<class T, typename Container, SubstitutedIntrusiveListNode<T, Container> T::* member>
inline T* IntrusiveList<T, Container, member>::node_to_value(SubstitutedIntrusiveListNode<T, Container>& node)
{
    // Note: A data member pointer is a 32-bit offset in the Windows ABI (both x86 and x86_64),
    //       whereas it is an appropriately sized ptrdiff_t in the Itanium ABI, the following ensures
    //       that we always use the correct type for the subtraction.
    using EquivalentNumericTypeForDataMemberPointer = Conditional<sizeof(member) == sizeof(ptrdiff_t), ptrdiff_t, u32>;
    static_assert(sizeof(EquivalentNumericTypeForDataMemberPointer) == sizeof(member),
        "The equivalent numeric type for the data member pointer must have the same size as the data member pointer itself.");

    // Note: Since this might seem odd, here's an explanation on what this function actually does:
    //       `node` is a reference that resides in some part of the actual value (of type T), the
    //       placement (i.e. offset) of which is described by the pointer-to-data-member parameter
    //       named `member`.
    //       This function effectively takes in the address of the data member, and returns the address
    //       of the value (of type T) holding that member.
    return bit_cast<T*>(bit_cast<unsigned char*>(&node) - bit_cast<EquivalentNumericTypeForDataMemberPointer>(member));
}

template<typename T, typename Container>
inline IntrusiveListNode<T, Container>::~IntrusiveListNode()
{
    VERIFY(!is_in_list());
}

template<typename T, typename Container>
inline void IntrusiveListNode<T, Container>::remove()
{
    VERIFY(m_storage);
    if (m_storage->m_first == this)
        m_storage->m_first = m_next;
    if (m_storage->m_last == this)
        m_storage->m_last = m_prev;
    if (m_prev)
        m_prev->m_next = m_next;
    if (m_next)
        m_next->m_prev = m_prev;
    m_prev = nullptr;
    m_next = nullptr;
    m_storage = nullptr;
    if constexpr (!IsRaw)
        m_self.reference = nullptr;
}

template<typename T, typename Container>
inline bool IntrusiveListNode<T, Container>::is_in_list() const
{
    return m_storage != nullptr;
}

// Specialise IntrusiveList for NonnullRefPtr
// By default, intrusive lists cannot contain null entries anyway, so switch to RefPtr
// and just make the user-facing functions deref the pointers.

template<class T, SubstitutedIntrusiveListNode<T, NonnullRefPtr<T>> T::* member>
class IntrusiveList<T, NonnullRefPtr<T>, member> : public IntrusiveList<T, RefPtr<T>, member> {
public:
    [[nodiscard]] NonnullRefPtr<T> first() const { return *IntrusiveList<T, RefPtr<T>, member>::first(); }
    [[nodiscard]] NonnullRefPtr<T> last() const { return *IntrusiveList<T, RefPtr<T>, member>::last(); }

    [[nodiscard]] NonnullRefPtr<T> take_first() { return *IntrusiveList<T, RefPtr<T>, member>::take_first(); }
    [[nodiscard]] NonnullRefPtr<T> take_last() { return *IntrusiveList<T, RefPtr<T>, member>::take_last(); }
};

}

namespace AK {

template<typename T, typename Container = RawPtr<T>>
using IntrusiveListNode = Detail::SubstitutedIntrusiveListNode<T, Container>;

template<auto member>
using IntrusiveList = Detail::IntrusiveList<
    decltype(Detail::ExtractIntrusiveListTypes::value(member)),
    decltype(Detail::ExtractIntrusiveListTypes::container(member)),
    member>;

}

#if USING_AK_GLOBALLY
using AK::IntrusiveList;
using AK::IntrusiveListNode;
#endif
