/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Completion.h>

namespace JS {

class CompletionCell final : public Cell {
    GC_CELL(CompletionCell, Cell);
    GC_DECLARE_ALLOCATOR(CompletionCell);

public:
    CompletionCell(Completion completion)
        : m_completion(move(completion))
    {
    }

    virtual ~CompletionCell() override;

    [[nodiscard]] Completion const& completion() const { return m_completion; }
    void set_completion(Completion completion) { m_completion = move(completion); }

private:
    virtual void visit_edges(Cell::Visitor& visitor) override;

    Completion m_completion;
};

}
