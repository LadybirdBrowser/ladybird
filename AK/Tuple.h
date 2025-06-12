/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <AK/TypeList.h>

namespace AK::Detail {

template<typename... Ts>
struct Tuple {
};

template<typename T>
struct Tuple<T> {
    Tuple(T&& value)
    requires(!IsSame < T &&, T const& >)
        : value(forward<T>(value))
    {
    }

    Tuple(T const& value)
        : value(value)
    {
    }

    template<typename U>
    U& get()
    {
        static_assert(IsSame<T, U>, "Invalid tuple access");
        return value;
    }

    template<typename U>
    U const& get() const
    {
        return const_cast<Tuple<T>&>(*this).get<U>();
    }

    template<typename U, size_t index>
    U& get_with_index()
    {
        static_assert(IsSame<T, U> && index == 0, "Invalid tuple access");
        return value;
    }

    template<typename U, size_t index>
    U const& get_with_index() const
    {
        return const_cast<Tuple<T>&>(*this).get_with_index<U, index>();
    }

private:
    T value;
};

template<typename T, typename... TRest>
struct Tuple<T, TRest...> : Tuple<TRest...> {

    template<typename FirstT, typename... RestT>
    Tuple(FirstT&& first, RestT&&... rest)
        : Tuple<TRest...>(forward<RestT>(rest)...)
        , value(forward<FirstT>(first))
    {
    }

    Tuple(T&& first, TRest&&... rest)
        : Tuple<TRest...>(move(rest)...)
        , value(move(first))
    {
    }

    template<typename U>
    U& get()
    {
        if constexpr (IsSame<T, U>)
            return value;
        else
            return Tuple<TRest...>::template get<U>();
    }

    template<typename U>
    U const& get() const
    {
        return const_cast<Tuple<T, TRest...>&>(*this).get<U>();
    }

    template<typename U, size_t index>
    U& get_with_index()
    {
        if constexpr (IsSame<T, U> && index == 0)
            return value;
        else
            return Tuple<TRest...>::template get_with_index<U, index - 1>();
    }

    template<typename U, size_t index>
    U const& get_with_index() const
    {
        return const_cast<Tuple<T, TRest...>&>(*this).get_with_index<U, index>();
    }

private:
    T value;
};

}

namespace AK {

template<typename... Ts>
struct Tuple : Detail::Tuple<Ts...> {
    using Types = TypeList<Ts...>;
    using Detail::Tuple<Ts...>::Tuple;
    using Indices = MakeIndexSequence<sizeof...(Ts)>;

    Tuple(Tuple&& other)
        : Tuple(move(other), Indices())
    {
    }

    Tuple(Tuple const& other)
        : Tuple(other, Indices())
    {
    }

    Tuple& operator=(Tuple&& other)
    {
        set(move(other), Indices());
        return *this;
    }

    Tuple& operator=(Tuple const& other)
    {
        set(other, Indices());
        return *this;
    }

    template<typename T>
    auto& get()
    {
        return Detail::Tuple<Ts...>::template get<T>();
    }

    template<size_t index>
    auto& get()
    {
        return Detail::Tuple<Ts...>::template get_with_index<typename Types::template Type<index>, index>();
    }

    template<typename T>
    auto& get() const
    {
        return Detail::Tuple<Ts...>::template get<T>();
    }

    template<size_t index>
    auto& get() const
    {
        return Detail::Tuple<Ts...>::template get_with_index<typename Types::template Type<index>, index>();
    }

    template<typename F>
    auto apply_as_args(F&& f)
    {
        return apply_as_args(forward<F>(f), Indices());
    }

    template<typename F>
    auto apply_as_args(F&& f) const
    {
        return apply_as_args(forward<F>(f), Indices());
    }

    static constexpr auto size() { return sizeof...(Ts); }

private:
    template<size_t... Is>
    Tuple(Tuple&& other, IndexSequence<Is...>)
        : Detail::Tuple<Ts...>(move(other.get<Is>())...)
    {
    }

    template<size_t... Is>
    Tuple(Tuple const& other, IndexSequence<Is...>)
        : Detail::Tuple<Ts...>(other.get<Is>()...)
    {
    }

    template<size_t... Is>
    void set(Tuple&& other, IndexSequence<Is...>)
    {
        ((get<Is>() = move(other.get<Is>())), ...);
    }

    template<size_t... Is>
    void set(Tuple const& other, IndexSequence<Is...>)
    {
        ((get<Is>() = other.get<Is>()), ...);
    }

    template<typename F, size_t... Is>
    auto apply_as_args(F&& f, IndexSequence<Is...>)
    {
        return forward<F>(f)(get<Is>()...);
    }

    template<typename F, size_t... Is>
    auto apply_as_args(F&& f, IndexSequence<Is...>) const
    {
        return forward<F>(f)(get<Is>()...);
    }
};

template<class... Args>
Tuple(Args... args) -> Tuple<Args...>;

}

#if USING_AK_GLOBALLY
using AK::Tuple;
#endif
