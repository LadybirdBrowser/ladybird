/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class GeneratorResult final : public Cell {
    GC_CELL(GeneratorResult, Cell);
    GC_DECLARE_ALLOCATOR(GeneratorResult);

public:
    GeneratorResult(Value result, Value continuation, bool is_await)
        : m_is_await(is_await)
        , m_result(result)
        , m_continuation(continuation)
    {
    }

    virtual ~GeneratorResult() override;

    [[nodiscard]] Value result() const { return m_result; }
    [[nodiscard]] Value continuation() const { return m_continuation; }
    [[nodiscard]] bool is_await() const { return m_is_await; }

private:
    virtual void visit_edges(Cell::Visitor& visitor) override;

    bool m_is_await { false };
    Value m_result;
    Value m_continuation;
};

}
