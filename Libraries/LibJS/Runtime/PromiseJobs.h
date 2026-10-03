/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/JobCallback.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseJob.h>

namespace JS {

struct PromiseJobRecord {
    PromiseJob job;       // [[Job]]
    GC::Ptr<Realm> realm; // [[Realm]]
};

// NOTE: These return a PromiseJobRecord to prevent awkward casting at call sites.
PromiseJobRecord create_promise_reaction_job(VM&, PromiseReaction&, Value argument);
PromiseJobRecord create_promise_resolve_thenable_job(VM&, Promise&, Value thenable, GC::Ref<JobCallback> then);

}
