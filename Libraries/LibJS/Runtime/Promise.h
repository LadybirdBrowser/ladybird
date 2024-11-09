/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

ThrowCompletionOr<Object*> promise_resolve(VM&, Object& constructor, Value);

class Promise : public Object {
    JS_OBJECT(Promise, Object);
    JS_DECLARE_ALLOCATOR(Promise);

public:
    enum class State {
        Pending,
        Fulfilled,
        Rejected,
    };
    enum class RejectionOperation {
        Reject,
        Handle,
    };

    static NonnullGCPtr<Promise> create(Realm&);

    virtual ~Promise() = default;

    State state() const { return m_state; }
    Value result() const { return m_result; }

    struct ResolvingFunctions {
        NonnullGCPtr<FunctionObject> resolve;
        NonnullGCPtr<FunctionObject> reject;
    };
    ResolvingFunctions create_resolving_functions();

    void fulfill(Value value);
    void reject(Value reason);
    Value perform_then(Value on_fulfilled, Value on_rejected, GCPtr<PromiseCapability> result_capability);

    bool is_handled() const { return m_is_handled; }
    void set_is_handled() { m_is_handled = true; }

protected:
    explicit Promise(Object& prototype);

    virtual void visit_edges(Visitor&) override;

private:
    bool is_settled() const { return m_state == State::Fulfilled || m_state == State::Rejected; }

    void trigger_reactions() const;

    // 27.2.6 Properties of Promise Instances, https://tc39.es/ecma262/#sec-properties-of-promise-instances
    State m_state { State::Pending };                   // [[PromiseState]]
    Value m_result;                                     // [[PromiseResult]]
    Vector<GCPtr<PromiseReaction>> m_fulfill_reactions; // [[PromiseFulfillReactions]]
    Vector<GCPtr<PromiseReaction>> m_reject_reactions;  // [[PromiseRejectReactions]]
    bool m_is_handled { false };                        // [[PromiseIsHandled]]
};

}
