/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class JS_API Promise : public Object {
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

    State state() const;
    Value result() const;

    struct ResolvingFunctions {
        GC::Ref<FunctionObject> resolve;
        GC::Ref<FunctionObject> reject;
    };

    ResolvingFunctions create_resolving_functions();

    Value perform_then(Value on_fulfilled, Value on_rejected, GC::Ptr<PromiseCapability> result_capability);

    bool is_handled() const;
    void set_is_handled();

    static bool is_engine_class_of(Object const& object) { return object.is_of_engine_class_or_subclass(JS_LAYOUT_CLASS_ID_PROMISE); }
};

}
