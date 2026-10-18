/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibGC/ActivityRoot.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Export.h>

namespace Web::Platform {

class WEB_API Timer : public JS::Cell {
    GC_CELL(Timer, JS::Cell);
    GC_DECLARE_ALLOCATOR(Timer);

public:
    static GC::Ref<Timer> create(GC::Heap&);
    static GC::Ref<Timer> create_repeating(GC::Heap&, AK::Duration interval, GC::Ptr<GC::Function<void()>> timeout_handler);
    static GC::Ref<Timer> create_single_shot(GC::Heap&, AK::Duration interval, GC::Ptr<GC::Function<void()>> timeout_handler);

    virtual ~Timer();
    virtual void finalize() override;

    void start();
    void start(AK::Duration interval);
    void restart();
    void restart(AK::Duration interval);
    void stop();

    void set_active(bool);

    bool is_active() const;
    AK::Duration interval() const;
    void set_interval(AK::Duration interval);

    bool is_single_shot() const;
    void set_single_shot(bool);

    bool has_activity_root() const { return m_activity_root.is_taken(); }

    GC::Ptr<GC::Function<void()>> on_timeout;

private:
    Timer();

    virtual void visit_edges(JS::Cell::Visitor&) override;

    NonnullRefPtr<Core::Timer> m_timer;
    GC::ActivityRoot m_activity_root;
};

}
