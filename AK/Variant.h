/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, Jonne Ransijn <jonne@yyny.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/BitCast.h>
#include <AK/Concepts.h>
#include <AK/Noncopyable.h>
#include <AK/StdLibExtras.h>
#include <AK/TypeList.h>
#include <AK/UntaggedUnion.h>

namespace AK {

struct Empty {
    constexpr bool operator==(Empty const&) const = default;
};

}

namespace AK::Detail {

template<typename T, typename IndexType, IndexType InitialIndex, typename... InTypes>
struct VariantIndexOf {
    static_assert(DependentFalse<T, IndexType, InTypes...>, "Invalid VariantIndex instantiated");
};

template<typename T, typename IndexType, IndexType InitialIndex, typename InType, typename... RestOfInTypes>
struct VariantIndexOf<T, IndexType, InitialIndex, InType, RestOfInTypes...> {
    consteval IndexType operator()()
    {
        if constexpr (IsSame<T, InType>)
            return InitialIndex;
        else
            return VariantIndexOf<T, IndexType, InitialIndex + 1, RestOfInTypes...> {}();
    }
};

template<typename T, typename IndexType, IndexType InitialIndex>
struct VariantIndexOf<T, IndexType, InitialIndex> {
    consteval IndexType operator()() { return InitialIndex; }
};

template<typename Arg, size_t I, typename T>
struct __Overload { };

template<typename Arg, size_t Index, typename T>
requires(IsArithmetic<T> || IsSame<RemoveCVReference<T>, bool> || IsNonNarrowingConvertible<Arg, T>)
struct __Overload<Arg, Index, T> {
    using FunctionPointer = IntegralConstant<size_t, Index> (*)(T);
    constexpr operator FunctionPointer() const { return nullptr; }
};

template<typename Arg, typename... Ts>
struct __OverloadSet {
private:
    template<typename>
    struct Make;

    template<size_t... Is>
    struct Make<IntegerSequence<size_t, Is...>> : __Overload<Arg, Is, Ts>... { };

public:
    using Type = Make<MakeIndexSequence<sizeof...(Ts)>>;
};

template<typename Arg, typename... Ts>
using OverloadSet = typename __OverloadSet<Arg, Ts...>::Type;

template<typename Arg, typename... Ts>
using BestViableOverload = InvokeResult<OverloadSet<Arg, Ts...>, Arg>;

template<typename IndexType, typename T, typename... Ts>
requires(requires { BestViableOverload<T, Ts...>(); })
constexpr IndexType VariantConstructedIndexOf = BestViableOverload<T, Ts...>();

template<typename T, typename IndexType, typename... Ts>
consteval IndexType index_of()
{
    return VariantIndexOf<T, IndexType, 0, Ts...> {}();
}

template<typename T, typename IndexType, typename... Ts>
consteval IndexType constructed_index_of()
{
    if constexpr (((IsSameIgnoringCV<T, Ts> || ...) || (IsConvertible<T, Ts> || ...)) && requires { VariantConstructedIndexOf<size_t, T, Ts...>; }) {
        return VariantConstructedIndexOf<size_t, T, Ts...>;
    } else {
        return sizeof...(Ts);
    }
}

template<typename IndexType, IndexType InitialIndex, typename... Ts>
struct VariantOperations;

template<typename IndexType, IndexType InitialIndex>
struct VariantOperations<IndexType, InitialIndex> {
    ALWAYS_INLINE static constexpr void delete_(IndexType, UntaggedUnion<>&)
    {
        ASSERT_NOT_REACHED();
    }

    ALWAYS_INLINE static constexpr void move_(IndexType, UntaggedUnion<>&&, UntaggedUnion<>&)
    {
        ASSERT_NOT_REACHED();
    }

    ALWAYS_INLINE static constexpr void copy_(IndexType, UntaggedUnion<> const&, UntaggedUnion<>&)
    {
        ASSERT_NOT_REACHED();
    }
};

template<typename IndexType, IndexType InitialIndex, typename F, typename... Ts>
struct VariantOperations<IndexType, InitialIndex, F, Ts...> {
    static constexpr auto current_index = VariantIndexOf<F, IndexType, InitialIndex, F, Ts...> {}();

    ALWAYS_INLINE static constexpr void delete_(IndexType id, UntaggedUnion<F, Ts...>& data)
    {
        if (id == current_index)
            data.template get<F>().~F();
        else
            VariantOperations<IndexType, InitialIndex + 1, Ts...>::delete_(id, data.next);
    }

    ALWAYS_INLINE static constexpr void move_(IndexType old_id, UntaggedUnion<F, Ts...>&& old_data, UntaggedUnion<F, Ts...>& new_data)
    {
        if (old_id == current_index)
            AK::construct_at(&new_data.template get<F>(), move(old_data.template get<F>()));
        else
            VariantOperations<IndexType, InitialIndex + 1, Ts...>::move_(old_id, move(old_data.next), new_data.next);
    }

    ALWAYS_INLINE static constexpr void copy_(IndexType old_id, UntaggedUnion<F, Ts...> const& old_data, UntaggedUnion<F, Ts...>& new_data)
    {
        if (old_id == current_index)
            AK::construct_at(&new_data.template get<F>(), old_data.template get<F>());
        else
            VariantOperations<IndexType, InitialIndex + 1, Ts...>::copy_(old_id, old_data.next, new_data.next);
    }

    template<typename Visitor>
    ALWAYS_INLINE static constexpr decltype(auto) visit_(IndexType id, DerivedFrom<UntaggedUnion<F, Ts...>> auto& data, Visitor&& visitor)
    {
        if (id == current_index) {
            // Check if Visitor::operator() is an explicitly typed function (as opposed to a templated function)
            // if so, try to call that with `T const&` first before copying the Variant's const-ness.
            // This emulates normal C++ call semantics where templated functions are considered last, after all non-templated overloads
            // are checked and found to be unusable.
            if constexpr (should_invoke_const_overload<F, Visitor>(MakeIndexSequence<Visitor::Types::size>())) {
                return visitor(data.template get<AddConst<F>>());
            }

            return visitor(data.template get<F>());
        }

        if constexpr (requires { VariantOperations<IndexType, InitialIndex + 1, Ts...>::template visit_<Visitor>(id, data.next, forward<Visitor>(visitor)); })
            return VariantOperations<IndexType, InitialIndex + 1, Ts...>::template visit_<Visitor>(id, data.next, forward<Visitor>(visitor));
        VERIFY_NOT_REACHED();
    }

private:
    template<typename T, size_t I, typename Fn>
    static constexpr bool has_explicitly_named_overload()
    {
        // If we're not allowed to make a member function pointer and call it directly (without explicitly resolving it),
        // we have a templated function on our hands (or a function overload set).
        // in such cases, we don't have an explicitly named overload, and we would have to select it.
        return requires { (declval<Fn>().*(&Fn::operator()))(declval<T>()); };
    }

    template<typename T, typename Visitor, auto... Is>
    static constexpr bool should_invoke_const_overload(IndexSequence<Is...>)
    {
        // Scan over all the different visitor functions, if none of them are suitable for calling with `T const&`, avoid calling that first.
        return ((has_explicitly_named_overload<T, Is, typename Visitor::Types::template Type<Is>>()) || ...);
    }
};

template<typename T, typename... Ts>
struct __DeduplicateParameterPack {
    using Type = T;
};

template<template<typename...> typename Container, typename... SeenTypes, typename First, typename... Rest>
struct __DeduplicateParameterPack<Container<SeenTypes...>, First, Rest...>
    : Conditional<(SameAs<First, SeenTypes> || ...), __DeduplicateParameterPack<Container<SeenTypes...>, Rest...>, __DeduplicateParameterPack<Container<SeenTypes..., First>, Rest...>> { };

template<typename Container>
struct DeduplicateParameterPack;

template<template<typename...> typename Container, typename... Types>
struct DeduplicateParameterPack<Container<Types...>> : public __DeduplicateParameterPack<Container<>, Types...> { };

template<typename... Ts>
class Variant {
public:
    using IndexType = Conditional<(sizeof...(Ts) < 255), u8, size_t>; // Note: size+1 reserved for internal value checks

private:
    static constexpr IndexType invalid_index = sizeof...(Ts);

    template<typename T>
    static constexpr IndexType index_of() { return AK::Detail::index_of<T, IndexType, Ts...>(); }

    template<typename T>
    static constexpr IndexType constructed_index_of() { return AK::Detail::constructed_index_of<T, IndexType, Ts...>(); }

    using Operations = VariantOperations<IndexType, 0, Ts...>;

    template<IndexType N>
    using Nth = TypeList<Ts...>::template Type<N>;

public:
    template<typename T>
    static constexpr bool can_contain()
    {
        return index_of<T>() != invalid_index;
    }

    template<typename T>
    static constexpr bool can_construct()
    {
        return constructed_index_of<T>() != invalid_index;
    }

    template<typename... NewTs>
    constexpr Variant(Variant<NewTs...>&& old)
    requires((can_contain<NewTs>() && ...))
        : Variant(move(old).template downcast<Ts...>())
    {
    }

    template<typename... NewTs>
    constexpr Variant(Variant<NewTs...> const& old)
    requires((can_contain<NewTs>() && ...))
        : Variant(old.template downcast<Ts...>())
    {
    }

    template<typename... NewTs>
    friend class Variant;

    Variant()
    requires(!can_contain<Empty>())
    = delete;

    constexpr Variant()
    requires(can_contain<Empty>())
        : Variant(Empty())
    {
    }

    AK_MAKE_CONDITIONALLY_COPYABLE(Variant, <Ts>&&...);
    AK_MAKE_CONDITIONALLY_MOVABLE(Variant, <Ts>&&...);
    AK_MAKE_CONDITIONALLY_DESTRUCTIBLE(Variant, <Ts>&&...);

    ALWAYS_INLINE constexpr Variant(Variant const& other)
    requires(!(IsTriviallyCopyConstructible<Ts> && ...))
    {
        ASSERT(other.m_index != invalid_index);
        Operations::copy_(other.m_index, other.m_storage, m_storage);
        m_index = other.m_index;
    }

    ALWAYS_INLINE constexpr Variant(Variant&& other)
    requires(!(IsTriviallyMoveConstructible<Ts> && ...))
    {
        ASSERT(other.m_index != invalid_index);
        Operations::move_(other.m_index, move(other.m_storage), m_storage);
        m_index = other.m_index;
    }

    ALWAYS_INLINE constexpr ~Variant()
    requires(!(IsTriviallyDestructible<Ts> && ...))
    {
        Operations::delete_(m_index, m_storage);
#ifndef NDEBUG
        m_index = invalid_index;
#endif
    }

    ALWAYS_INLINE constexpr Variant& operator=(Variant const& other)
    requires(!(IsTriviallyCopyConstructible<Ts> && ...) || !(IsTriviallyDestructible<Ts> && ...))
    {
        if (this != &other) {
            if constexpr (!(IsTriviallyDestructible<Ts> && ...)) {
                Operations::delete_(m_index, m_storage);
            }
            m_index = other.m_index;
            Operations::copy_(other.m_index, other.m_storage, m_storage);
        }
        return *this;
    }

    ALWAYS_INLINE constexpr Variant& operator=(Variant&& other)
    requires(!(IsTriviallyMoveConstructible<Ts> && ...) || !(IsTriviallyDestructible<Ts> && ...))
    {
        if (this != &other) {
            if constexpr (!(IsTriviallyDestructible<Ts> && ...)) {
                Operations::delete_(m_index, m_storage);
            }
            m_index = other.m_index;
            Operations::move_(other.m_index, move(other.m_storage), m_storage);
        }
        return *this;
    }

    template<typename T>
    ALWAYS_INLINE constexpr Variant(T&& v)
    requires(!IsSameIgnoringCV<T, Variant> && can_construct<T>())
        : m_storage(Nth<constructed_index_of<T>()>(forward<T>(v)))
        , m_index(constructed_index_of<T>())
    {
    }

    template<typename T, typename StrippedT = RemoveCVReference<T>>
    ALWAYS_INLINE constexpr Variant& operator=(T&& v)
    requires(can_contain<StrippedT>() && requires { StrippedT(forward<T>(v)); })
    {
        emplace<StrippedT, T>(forward<T>(v));

        return *this;
    }

    template<typename T>
    [[nodiscard]] ALWAYS_INLINE constexpr bool has() const
    requires(can_contain<RemoveConst<T>>())
    {
        return index_of<RemoveConst<T>>() == m_index;
    }

    template<typename T>
    [[nodiscard]] constexpr T& get()
    requires(can_contain<T>())
    {
        VERIFY(has<T>());
        return m_storage.template get<T>();
    }

    template<typename T>
    [[nodiscard]] constexpr T const& get() const
    requires(can_contain<RemoveConst<T>>())
    {
        VERIFY(has<T>());
        return m_storage.template get<T>();
    }

    template<typename T>
    [[nodiscard]] constexpr T* get_pointer()
    requires(can_contain<T>())
    {
        if (has<T>())
            return &m_storage.template get<T>();
        return nullptr;
    }

    template<typename T>
    [[nodiscard]] constexpr T const* get_pointer() const
    requires(can_contain<RemoveConst<T>>())
    {
        if (has<T>())
            return &m_storage.template get<T>();
        return nullptr;
    }

    template<typename T, typename... Args>
    constexpr void emplace(Args&&... args)
    requires(can_contain<T>())
    {
        Operations::delete_(m_index, m_storage);
        m_storage.template set<T>(forward<Args>(args)...);
        m_index = index_of<T>();
    }

    template<typename T, typename StrippedT = RemoveCVReference<T>>
    constexpr void set(T&& t)
    requires(can_contain<StrippedT>() && requires { StrippedT(forward<T>(t)); })
    {
        emplace<StrippedT, T>(forward<T>(t));
    }

    template<typename... Fs>
    ALWAYS_INLINE constexpr decltype(auto) visit(Fs&&... functions)
    {
        Visitor<Fs...> visitor { forward<Fs>(functions)... };
        return Operations::visit_(m_index, m_storage, move(visitor));
    }

    template<typename... Fs>
    ALWAYS_INLINE constexpr decltype(auto) visit(Fs&&... functions) const
    {
        Visitor<Fs...> visitor { forward<Fs>(functions)... };
        return Operations::visit_(m_index, m_storage, move(visitor));
    }

    template<typename... NewTs>
    constexpr decltype(auto) downcast() &&
    {
        if constexpr (sizeof...(NewTs) == 1 && (IsSpecializationOf<NewTs, Variant> && ...)) {
            return move(*this).template downcast_variant<NewTs...>();
        } else {
            return visit(
                [&](auto&& value) -> Variant<NewTs...> {
                    if constexpr (Variant<NewTs...>::template can_construct<decltype(move(value))>())
                        return Variant<NewTs...> { move(value) };
                    VERIFY_NOT_REACHED();
                });
        }
    }

    template<typename... NewTs>
    constexpr decltype(auto) downcast() const&
    {
        if constexpr (sizeof...(NewTs) == 1 && (IsSpecializationOf<NewTs, Variant> && ...)) {
            return (*this).downcast_variant(TypeWrapper<NewTs...> {});
        } else {
            return visit(
                [&](auto const& value) -> Variant<NewTs...> {
                    if constexpr (Variant<NewTs...>::template can_construct<decltype(value)>())
                        return Variant<NewTs...> { value };
                    VERIFY_NOT_REACHED();
                });
        }
    }

    constexpr bool operator==(Variant const& other) const
    requires(requires { declval<Ts const&>() == declval<Ts const&>(); } && ...)
    {
        return visit(
            [&]<typename T>(T self) {
                if (auto* p = other.get_pointer<decltype(self)>())
                    return self == *p;
                return false;
            });
    }

    constexpr auto index() const { return m_index; }

private:
    template<typename... NewTs>
    constexpr Variant<NewTs...> downcast_variant(TypeWrapper<Variant<NewTs...>>) &&
    {
        return move(*this).template downcast<NewTs...>();
    }

    template<typename... NewTs>
    constexpr Variant<NewTs...> downcast_variant(TypeWrapper<Variant<NewTs...>>) const&
    {
        return (*this).template downcast<NewTs...>();
    }

    template<typename... Fs>
    struct Visitor : Fs... {
        using Types = TypeList<Fs...>;

        constexpr Visitor(Fs&&... args)
            : Fs(forward<Fs>(args))...
        {
        }

        using Fs::operator()...;
    };

    UntaggedUnion<Ts...> m_storage;
    IndexType m_index;
};

}

namespace AK {

template<typename... Ts>
using Variant = Detail::DeduplicateParameterPack<Detail::Variant<Ts...>>::Type;

template<typename... Ts>
struct TypeList<Detail::Variant<Ts...>> : TypeList<Ts...> { };

}

#if USING_AK_GLOBALLY
using AK::Empty;
using AK::Variant;
#endif
