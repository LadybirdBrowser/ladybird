/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibJS/Heap/Cell.h>

namespace Web::Platform {

class Timer : public JS::Cell {
    GC_CELL(Timer, JS::Cell);

public:
    static GC::Ref<Timer> create(GC::Heap&);
    static GC::Ref<Timer> create_repeating(GC::Heap&, int interval_ms, GC::Ptr<GC::Function<void()>> timeout_handler);
    static GC::Ref<Timer> create_single_shot(GC::Heap&, int interval_ms, GC::Ptr<GC::Function<void()>> timeout_handler);

    virtual ~Timer();

    virtual void start() = 0;
    virtual void start(int interval_ms) = 0;
    virtual void restart() = 0;
    virtual void restart(int interval_ms) = 0;
    virtual void stop() = 0;

    virtual void set_active(bool) = 0;

    virtual bool is_active() const = 0;
    virtual int interval() const = 0;
    virtual void set_interval(int interval_ms) = 0;

    virtual bool is_single_shot() const = 0;
    virtual void set_single_shot(bool) = 0;

    GC::Ptr<GC::Function<void()>> on_timeout;

protected:
    virtual void visit_edges(JS::Cell::Visitor&) override;
};

}
