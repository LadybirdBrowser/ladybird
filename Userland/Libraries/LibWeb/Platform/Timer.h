/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/SafeFunction.h>

namespace Web::Platform {

class Timer : public JS::Cell {
    JS_CELL(Timer, JS::Cell);

public:
    static JS::NonnullGCPtr<Timer> create(JS::Heap&);
    static JS::NonnullGCPtr<Timer> create_repeating(JS::Heap&, int interval_ms, JS::SafeFunction<void()>&& timeout_handler);
    static JS::NonnullGCPtr<Timer> create_single_shot(JS::Heap&, int interval_ms, JS::SafeFunction<void()>&& timeout_handler);

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

    JS::SafeFunction<void()> on_timeout;
};

}
