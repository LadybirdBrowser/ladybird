/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/JobCallback.h>

namespace JS {

// 27.2.1.2 PromiseReaction Records, https://tc39.es/ecma262/#sec-promisereaction-records
class PromiseReaction final : public Cell {
    GC_CELL(PromiseReaction, Cell);
    GC_DECLARE_ALLOCATOR(PromiseReaction);

public:
    enum class Type {
        Fulfill,
        Reject,
    };

    static GC::Ref<PromiseReaction> create(VM& vm, Type type, GC::Ptr<PromiseCapability> capability, GC::Ptr<JobCallback> handler);

    virtual ~PromiseReaction() = default;

    Type type() const { return m_type; }
    GC::Ptr<PromiseCapability> capability() const { return m_capability; }

    GC::Ptr<JobCallback> handler() { return m_handler; }
    GC::Ptr<JobCallback const> handler() const { return m_handler; }

private:
    PromiseReaction(Type type, GC::Ptr<PromiseCapability> capability, GC::Ptr<JobCallback> handler);

    virtual void visit_edges(Visitor&) override;

    Type m_type;
    GC::Ptr<PromiseCapability> m_capability;
    GC::Ptr<JobCallback> m_handler;
};

}
