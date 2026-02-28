/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibCore/Forward.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>

namespace Web::Platform {

class Timer : public JS::Cell {
    GC_CELL(Timer, JS::Cell);
    GC_DECLARE_ALLOCATOR(Timer);

public:
    static GC::Ref<Timer> create(GC::Heap&);
    static GC::Ref<Timer> create_repeating(GC::Heap&, int interval_ms, GC::Ptr<GC::Function<void()>> timeout_handler);
    static GC::Ref<Timer> create_single_shot(GC::Heap&, int interval_ms, GC::Ptr<GC::Function<void()>> timeout_handler);

    virtual ~Timer();

    void start();
    void start(int interval_ms);
    void restart();
    void restart(int interval_ms);
    void stop();

    void set_active(bool);

    bool is_active() const;
    int interval() const;
    void set_interval(int interval_ms);

    bool is_single_shot() const;
    void set_single_shot(bool);

    GC::Ptr<GC::Function<void()>> on_timeout;

private:
    Timer();

    virtual void visit_edges(JS::Cell::Visitor&) override;

    NonnullRefPtr<Core::Timer> m_timer;
};

}
