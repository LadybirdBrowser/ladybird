/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Forward.h>
#include <LibGC/Cell.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>

namespace GC {

class GC_API Timer : public Cell {
    GC_CELL(Timer, Cell);
    GC_DECLARE_ALLOCATOR(Timer);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    explicit Timer();
    virtual ~Timer() override;
    virtual void finalize() override;

    void start(u64 timeout_ms, GC::Ref<GC::Function<void()>> on_timeout);
    void stop_and_fire_timeout_handler();
    void stop();

    bool is_timed_out() const { return m_timed_out; }

private:
    virtual void visit_edges(Cell::Visitor& visitor) override;

    NonnullRefPtr<Core::Timer> m_timer;
    GC::Ptr<GC::Function<void()>> m_on_timeout;
    bool m_timed_out { false };
};

}
