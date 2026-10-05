/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>

namespace JS {

struct HostHookThunks;

// The job HostEnqueuePromiseJob hands to the host. The host keeps it alive until it runs it, by capturing it in a GC
// function or visiting it; how the engine represents the job stays private to the engine.
class JS_API PromiseJob {
public:
    ThrowCompletionOr<Value> run() const;

    void visit_edges(GC::Cell::Visitor& visitor) { visitor.visit(m_job); }

private:
    friend class VM;
    friend struct HostHookThunks;

    // The runtime's job, a cell of LibGC's heap that only the runtime looks into.
    explicit PromiseJob(GC::Ref<GC::Cell> job)
        : m_job(job)
    {
    }

    GC::Ref<GC::Cell> m_job;
};

}
