/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>

namespace AK {
namespace Detail {

template<class T, typename Container, IntrusiveListNode<T, Container> T::* member>
class IntrusiveListRelaxedConst : public IntrusiveList<T, Container, member> {
    AK_MAKE_NONCOPYABLE(IntrusiveListRelaxedConst);
    AK_MAKE_NONMOVABLE(IntrusiveListRelaxedConst);

public:
    using IntrusiveList<T, Container, member>::IntrusiveList;

    using Iterator = typename IntrusiveList<T, Container, member>::Iterator;

    Iterator begin() const { return const_cast<IntrusiveListRelaxedConst*>(this)->IntrusiveList<T, Container, member>::begin(); }
    Iterator end() const { return Iterator {}; }
};

}

template<auto member>
using IntrusiveListRelaxedConst = Detail::IntrusiveListRelaxedConst<
    decltype(Detail::ExtractIntrusiveListTypes::value(member)),
    decltype(Detail::ExtractIntrusiveListTypes::container(member)),
    member>;

}

#if USING_AK_GLOBALLY
using AK::IntrusiveListRelaxedConst;
#endif
