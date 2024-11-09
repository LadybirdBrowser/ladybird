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
    JS_CELL(PromiseReaction, Cell);
    JS_DECLARE_ALLOCATOR(PromiseReaction);

public:
    enum class Type {
        Fulfill,
        Reject,
    };

    static NonnullGCPtr<PromiseReaction> create(VM& vm, Type type, GCPtr<PromiseCapability> capability, JS::GCPtr<JobCallback> handler);

    virtual ~PromiseReaction() = default;

    Type type() const { return m_type; }
    GCPtr<PromiseCapability> capability() const { return m_capability; }

    JS::GCPtr<JobCallback> handler() { return m_handler; }
    JS::GCPtr<JobCallback const> handler() const { return m_handler; }

private:
    PromiseReaction(Type type, GCPtr<PromiseCapability> capability, JS::GCPtr<JobCallback> handler);

    virtual void visit_edges(Visitor&) override;

    Type m_type;
    GCPtr<PromiseCapability> m_capability;
    JS::GCPtr<JobCallback> m_handler;
};

}
