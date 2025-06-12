/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>

namespace AK {

template<typename... Types>
struct TypeList;

template<unsigned Index, typename List>
struct TypeListElement;

#if __has_builtin(__type_pack_element)
template<unsigned Index, typename... Types>
struct TypeListElement<Index, TypeList<Types...>> {
    using Type = __type_pack_element<Index, Types...>;
};
#else
template<unsigned Index, typename Head, typename... Tail>
struct TypeListElement<Index, TypeList<Head, Tail...>>
    : TypeListElement<Index - 1, TypeList<Tail...>> {
};

template<typename Head, typename... Tail>
struct TypeListElement<0, TypeList<Head, Tail...>> {
    using Type = Head;
};
#endif

template<typename... Types>
struct TypeList {
    static constexpr unsigned size = sizeof...(Types);

    template<unsigned N>
    using Type = typename TypeListElement<N, TypeList<Types...>>::Type;
};

template<typename T>
struct TypeWrapper {
    using Type = T;
};

template<typename List, typename F, size_t... Indices>
constexpr void for_each_type_impl(F&& f, IndexSequence<Indices...>)
{
    (forward<F>(f)(TypeWrapper<typename List::template Type<Indices>> {}), ...);
}

template<typename List, typename F>
constexpr void for_each_type(F&& f)
{
    for_each_type_impl<List>(forward<F>(f), MakeIndexSequence<List::size> {});
}

template<typename ListA, typename ListB, typename F, size_t... Indices>
constexpr void for_each_type_zipped_impl(F&& f, IndexSequence<Indices...>)
{
    (forward<F>(f)(TypeWrapper<typename ListA::template Type<Indices>> {}, TypeWrapper<typename ListB::template Type<Indices>> {}), ...);
}

template<typename ListA, typename ListB, typename F>
constexpr void for_each_type_zipped(F&& f)
{
    static_assert(ListA::size == ListB::size, "Can't zip TypeLists that aren't the same size!");
    for_each_type_zipped_impl<ListA, ListB>(forward<F>(f), MakeIndexSequence<ListA::size> {});
}

}

#if USING_AK_GLOBALLY
using AK::for_each_type;
using AK::for_each_type_zipped;
using AK::TypeList;
using AK::TypeListElement;
using AK::TypeWrapper;
#endif
