/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Heap/HeapFunction.h>

namespace Web::WebDriver {

class HeapTimer : public JS::Cell {
    JS_CELL(HeapTimer, JS::Cell);
    JS_DECLARE_ALLOCATOR(HeapTimer);

public:
    explicit HeapTimer();
    virtual ~HeapTimer() override;

    void start(u64 timeout_ms, JS::NonnullGCPtr<JS::HeapFunction<void()>> on_timeout);
    void stop_and_fire_timeout_handler();
    void stop();

    bool is_timed_out() const { return m_timed_out; }

private:
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    NonnullRefPtr<Core::Timer> m_timer;
    JS::GCPtr<JS::HeapFunction<void()>> m_on_timeout;
    bool m_timed_out { false };
};

}
