/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/StdLibExtras.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <LibGC/CompressedPointer.h>
#include <LibGC/Ptr.h>

namespace GC {

// Member and NonnullMember are the types of cell pointers stored in cells, and in storage that cells visit. They hold a
// compressed pointer in 32 bits. Locals, parameters and return values keep using the full-width Ptr and Ref, which
// members convert to implicitly. Like Ptr and Ref, members only hand out a raw pointer through ptr().
//
// A member holds the address of the T it points to, so T must start with its cell, which the conservative scanner
// requires of a compressed pointer it finds on the stack: it only recognizes cell starts.

template<typename T>
class Member;

template<typename T>
class NonnullMember;

namespace Detail {

// Every store of a member goes through this hook, where a generational or moving collector will record the slot. It is
// empty for now, so it compiles to nothing.
ALWAYS_INLINE void member_write_barrier([[maybe_unused]] void const* slot, [[maybe_unused]] u32 compressed)
{
}

template<typename T>
ALWAYS_INLINE u32 compress_cell(T* cell)
{
    return compress(static_cast<void const*>(cell));
}

template<typename T>
concept IsMember = IsSpecializationOf<T, Member> || IsSpecializationOf<T, NonnullMember>;

}

template<typename T>
class Member {
public:
    constexpr Member() = default;

    Member(nullptr_t) { }

    Member(T* cell)
        : m_compressed(Detail::compress_cell(cell))
    {
    }

    Member(T& cell)
        : m_compressed(Detail::compress_cell(&cell))
    {
    }

    template<typename U>
    Member(U* cell)
    requires(IsConvertible<U*, T*>)
        : m_compressed(Detail::compress_cell(static_cast<T*>(cell)))
    {
    }

    template<typename U>
    Member(U& cell)
    requires(IsConvertible<U*, T*>)
        : m_compressed(Detail::compress_cell(static_cast<T*>(&cell)))
    {
    }

    template<typename U>
    Member(Ptr<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_compressed(Detail::compress_cell(static_cast<T*>(other.ptr())))
    {
    }

    template<typename U>
    Member(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_compressed(Detail::compress_cell(static_cast<T*>(other.ptr())))
    {
    }

    // OPTIMIZATION: Constructing a member from another copies its u32 and nothing else, which also has members passed
    //               and returned in registers. Only stores into existing members go through the write barrier.
    Member(Member const&) = default;
    Member(Member&&) = default;

    template<typename U>
    Member(Member<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_compressed(Detail::compress_cell(static_cast<T*>(other.ptr())))
    {
    }

    template<typename U>
    Member(NonnullMember<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_compressed(Detail::compress_cell(static_cast<T*>(other.ptr())))
    {
    }

    template<typename U>
    Member(U* foreign_cell)
    requires(IsForeignCellConvertibleToCell<U, T>)
        : m_compressed(Detail::compress_cell(as_cell(foreign_cell)))
    {
    }

    template<typename U>
    Member(U& foreign_cell)
    requires(IsForeignCellConvertibleToCell<U, T>)
        : m_compressed(Detail::compress_cell(as_cell(&foreign_cell)))
    {
    }

    template<typename U>
    Member(Ptr<U> const& foreign_cell)
    requires(IsForeignCellConvertibleToCell<U, T>)
        : m_compressed(Detail::compress_cell(as_cell(foreign_cell.ptr())))
    {
    }

    template<typename U>
    Member(Ref<U> const& foreign_cell)
    requires(IsForeignCellConvertibleToCell<U, T>)
        : m_compressed(Detail::compress_cell(as_cell(foreign_cell.ptr())))
    {
    }

    Member& operator=(Member const& other)
    {
        set_compressed(other.m_compressed);
        return *this;
    }

    template<typename U>
    Member& operator=(U&& other)
    requires(requires { Member(forward<U>(other)); })
    {
        set_compressed(Member(forward<U>(other)).m_compressed);
        return *this;
    }

    T* ptr() const { return reinterpret_cast<T*>(decompress(m_compressed)); }

    RETURNS_NONNULL T* operator->() const
    {
        ASSERT(m_compressed);
        return reinterpret_cast<T*>(decompress_nonnull(m_compressed));
    }

    [[nodiscard]] T& operator*() const
    {
        ASSERT(m_compressed);
        return *reinterpret_cast<T*>(decompress_nonnull(m_compressed));
    }

    explicit operator bool() const { return m_compressed != 0; }
    bool operator!() const { return m_compressed == 0; }

    Ref<T> as_nonnull() const
    {
        VERIFY(m_compressed);
        return *reinterpret_cast<T*>(decompress_nonnull(m_compressed));
    }

    template<typename U>
    operator Ptr<U>() const
    requires(IsConvertible<T*, U*> || IsForeignCellConvertibleToCell<T, U>)
    {
        return Ptr<U>(ptr());
    }

    u32 compressed() const { return m_compressed; }

private:
    template<typename U>
    friend class Member;

    ALWAYS_INLINE void set_compressed(u32 compressed)
    {
        Detail::member_write_barrier(this, compressed);
        m_compressed = compressed;
    }

    u32 m_compressed { 0 };
};

template<typename T>
class NonnullMember {
public:
    NonnullMember() = delete;

    NonnullMember(T& cell)
        : m_compressed(Detail::compress_cell(&cell))
    {
    }

    template<typename U>
    NonnullMember(U& cell)
    requires(IsConvertible<U*, T*>)
        : m_compressed(Detail::compress_cell(static_cast<T*>(&cell)))
    {
    }

    template<typename U>
    NonnullMember(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_compressed(Detail::compress_cell(static_cast<T*>(other.ptr())))
    {
    }

    NonnullMember(NonnullMember const&) = default;
    NonnullMember(NonnullMember&&) = default;

    template<typename U>
    NonnullMember(NonnullMember<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_compressed(Detail::compress_cell(static_cast<T*>(other.ptr())))
    {
    }

    template<typename U>
    NonnullMember(U& foreign_cell)
    requires(IsForeignCellConvertibleToCell<U, T>)
        : m_compressed(Detail::compress_cell(as_cell(&foreign_cell)))
    {
    }

    template<typename U>
    NonnullMember(Ref<U> const& foreign_cell)
    requires(IsForeignCellConvertibleToCell<U, T>)
        : m_compressed(Detail::compress_cell(as_cell(foreign_cell.ptr())))
    {
    }

    NonnullMember& operator=(NonnullMember const& other)
    {
        set_compressed(other.m_compressed);
        return *this;
    }

    template<typename U>
    NonnullMember& operator=(U&& other)
    requires(requires { NonnullMember(forward<U>(other)); })
    {
        set_compressed(NonnullMember(forward<U>(other)).m_compressed);
        return *this;
    }

    RETURNS_NONNULL T* ptr() const { return reinterpret_cast<T*>(decompress_nonnull(m_compressed)); }

    RETURNS_NONNULL T* operator->() const { return ptr(); }

    [[nodiscard]] T& operator*() const { return *ptr(); }

    operator T&() const { return *ptr(); }

    template<typename U>
    operator Ref<U>() const
    requires(IsConvertible<T*, U*> || IsForeignCellConvertibleToCell<T, U>)
    {
        return Ref<U>(*ptr());
    }

    template<typename U>
    operator Ptr<U>() const
    requires(IsConvertible<T*, U*> || IsForeignCellConvertibleToCell<T, U>)
    {
        return Ptr<U>(ptr());
    }

    operator bool() const = delete;
    bool operator!() const = delete;

    u32 compressed() const { return m_compressed; }

private:
    template<typename U>
    friend class NonnullMember;

    ALWAYS_INLINE void set_compressed(u32 compressed)
    {
        Detail::member_write_barrier(this, compressed);
        m_compressed = compressed;
    }

    u32 m_compressed;
};

namespace Detail {

// Whether a member of T that holds `compressed` points to `other`, as a comparison of T* with U* would tell.
template<typename T, typename U>
ALWAYS_INLINE bool member_points_to(u32 compressed, U const* other)
{
    if constexpr (IsConvertible<U*, T const*>) {
        // OPTIMIZATION: Compressing the other pointer is a shift, where decoding the member would load the base.
        return compressed == compress(static_cast<T const*>(other));
    } else if constexpr (IsConvertible<T*, U const*>) {
        return static_cast<U const*>(reinterpret_cast<T const*>(decompress(compressed))) == other;
    } else {
        // A cell compared to a type that names foreign cells, which shares its address.
        return compressed == compress(other);
    }
}

}

// OPTIMIZATION: Two members point to the same cell exactly when their compressed pointers are equal, so comparing them
//               decodes neither.
template<typename T, typename U>
requires(Detail::IsMember<T> && Detail::IsMember<U>)
inline bool operator==(T const& a, U const& b)
{
    return a.compressed() == b.compressed();
}

template<typename T, typename U>
inline bool operator==(Member<T> const& a, Ptr<U> const& b)
{
    return Detail::member_points_to<T>(a.compressed(), b.ptr());
}

template<typename T, typename U>
inline bool operator==(Member<T> const& a, Ref<U> const& b)
{
    return Detail::member_points_to<T>(a.compressed(), b.ptr());
}

template<typename T, typename U>
inline bool operator==(NonnullMember<T> const& a, Ptr<U> const& b)
{
    return Detail::member_points_to<T>(a.compressed(), b.ptr());
}

template<typename T, typename U>
inline bool operator==(NonnullMember<T> const& a, Ref<U> const& b)
{
    return Detail::member_points_to<T>(a.compressed(), b.ptr());
}

template<typename T, typename U>
inline bool operator==(Member<T> const& a, U const* b)
{
    return Detail::member_points_to<T>(a.compressed(), b);
}

template<typename T, typename U>
inline bool operator==(NonnullMember<T> const& a, U const* b)
{
    return Detail::member_points_to<T>(a.compressed(), b);
}

template<typename T>
inline bool operator==(Member<T> const& member, nullptr_t)
{
    return !member;
}

}

namespace AK {

template<typename T>
struct Traits<GC::Member<T>> : public DefaultTraits<GC::Member<T>> {
    static unsigned hash(GC::Member<T> const& value)
    {
        return Traits<T*>::hash(value.ptr());
    }
    static constexpr bool may_have_slow_equality_check() { return false; }
};

template<typename T>
struct Traits<GC::NonnullMember<T>> : public DefaultTraits<GC::NonnullMember<T>> {
    static unsigned hash(GC::NonnullMember<T> const& value)
    {
        return Traits<T*>::hash(value.ptr());
    }
    static constexpr bool may_have_slow_equality_check() { return false; }
};

namespace Detail {

template<typename T>
inline constexpr bool IsHashCompatible<GC::Member<T>, GC::Ptr<T>> = true;

template<typename T>
inline constexpr bool IsHashCompatible<GC::Ptr<T>, GC::Member<T>> = true;

template<typename T>
inline constexpr bool IsHashCompatible<GC::NonnullMember<T>, GC::Ref<T>> = true;

template<typename T>
inline constexpr bool IsHashCompatible<GC::Ref<T>, GC::NonnullMember<T>> = true;

}

template<typename T>
struct Formatter<GC::Member<T>> : Formatter<T const*> {
    ErrorOr<void> format(FormatBuilder& builder, GC::Member<T> const& value)
    {
        return Formatter<T const*>::format(builder, value.ptr());
    }
};

template<Formattable T>
struct Formatter<GC::NonnullMember<T>> : Formatter<T> {
    ErrorOr<void> format(FormatBuilder& builder, GC::NonnullMember<T> const& value)
    {
        return Formatter<T>::format(builder, *value);
    }
};

template<typename T>
requires(!HasFormatter<T>)
struct Formatter<GC::NonnullMember<T>> : Formatter<T const*> {
    ErrorOr<void> format(FormatBuilder& builder, GC::NonnullMember<T> const& value)
    {
        return Formatter<T const*>::format(builder, value.ptr());
    }
};

}
