/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibGC/Cell.h>
#include <LibGC/Heap.h>

namespace GC {

template<typename T>
class Function final : public Cell {
    GC_CELL(Function, Cell);

public:
    static Ref<Function> create(Heap& heap, AK::Function<T> function)
    {
        return heap.allocate<Function>(move(function));
    }

    virtual ~Function() override = default;

    [[nodiscard]] AK::Function<T> const& function() const { return m_function; }

private:
    Function(AK::Function<T> function)
        : m_function(move(function))
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit_possible_values(m_function.raw_capture_range());
    }

    AK::Function<T> m_function;
};

template<typename Callable, typename T = EquivalentFunctionType<Callable>>
static Ref<Function<T>> create_function(Heap& heap, Callable&& function)
{
    return Function<T>::create(heap, AK::Function<T> { forward<Callable>(function) });
}

}
