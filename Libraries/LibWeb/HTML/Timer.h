/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/WeakPtr.h>
#include <LibCore/Forward.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class Timer final : public JS::Cell {
    GC_CELL(Timer, JS::Cell);
    GC_DECLARE_ALLOCATOR(Timer);

public:
    enum class Repeating {
        No,
        Yes,
    };

    static constexpr bool OVERRIDES_FINALIZE = true;

    static GC::Ref<Timer> create(i32 milliseconds, Function<void()> callback, i32 id, Repeating);

    void start();
    void stop();

    void set_callback(Function<void()>);
    void set_interval(i32 milliseconds);

private:
    Timer(i32 milliseconds, Function<void()> callback, i32 id, Repeating);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    RefPtr<Core::Timer> m_timer;
    i32 m_id { 0 };
};

}
