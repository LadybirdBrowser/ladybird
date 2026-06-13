/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/IntrusiveList.h>
#include <AK/Span.h>
#include <LibGC/Forward.h>

namespace GC {

class GC_API ConservativeRangeProvider {
    AK_MAKE_NONCOPYABLE(ConservativeRangeProvider);
    AK_MAKE_NONMOVABLE(ConservativeRangeProvider);

public:
    virtual ~ConservativeRangeProvider();

    // Called while gathering roots; every word of every reported range is treated as a possible cell pointer.
    virtual void for_each_conservative_range(AK::Function<void(ReadonlySpan<FlatPtr>)> const&) const = 0;

    void detach_from_heap(Badge<Heap>) { m_heap = nullptr; }

protected:
    explicit ConservativeRangeProvider(Heap&);

    Heap* m_heap { nullptr };
    IntrusiveListNode<ConservativeRangeProvider> m_list_node;

public:
    using List = IntrusiveList<&ConservativeRangeProvider::m_list_node>;
};

}
