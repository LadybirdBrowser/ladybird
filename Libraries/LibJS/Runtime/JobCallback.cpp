/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/JobCallback.h>

namespace JS {

JS_DEFINE_ALLOCATOR(JobCallback);

JS::NonnullGCPtr<JobCallback> JobCallback::create(JS::VM& vm, FunctionObject& callback, OwnPtr<CustomData> custom_data)
{
    return vm.heap().allocate_without_realm<JobCallback>(callback, move(custom_data));
}

void JobCallback::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
}

// 9.5.2 HostMakeJobCallback ( callback ), https://tc39.es/ecma262/#sec-hostmakejobcallback
JS::NonnullGCPtr<JobCallback> make_job_callback(FunctionObject& callback)
{
    // 1. Return the JobCallback Record { [[Callback]]: callback, [[HostDefined]]: empty }.
    return JobCallback::create(callback.vm(), callback, {});
}

// 9.5.3 HostCallJobCallback ( jobCallback, V, argumentsList ), https://tc39.es/ecma262/#sec-hostcalljobcallback
ThrowCompletionOr<Value> call_job_callback(VM& vm, JS::NonnullGCPtr<JobCallback> job_callback, Value this_value, ReadonlySpan<Value> arguments_list)
{
    // 1. Assert: IsCallable(jobCallback.[[Callback]]) is true.

    // 2. Return ? Call(jobCallback.[[Callback]], V, argumentsList).
    return call(vm, job_callback->callback(), this_value, arguments_list);
}

}
