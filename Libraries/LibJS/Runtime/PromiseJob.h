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

// The job HostEnqueuePromiseJob hands to the host. The host keeps it alive until it runs it, by capturing it in a GC
// function or visiting it; how the engine represents the job stays private to the engine.
class JS_API PromiseJob {
public:
    explicit PromiseJob(GC::Ref<GC::Function<ThrowCompletionOr<Value>()>> steps)
        : m_steps(steps)
    {
    }

    ThrowCompletionOr<Value> run() const;

    void visit_edges(GC::Cell::Visitor& visitor) { visitor.visit(m_steps); }

private:
    friend class VM;

    GC::Ref<GC::Cell> m_steps;
};

}
