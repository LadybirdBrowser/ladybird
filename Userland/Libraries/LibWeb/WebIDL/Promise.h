/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/SafeFunction.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebIDL {

// NOTE: This is Function, not SafeFunction, because they get stored in a NativeFunction anyway, which will protect captures.
using ReactionSteps = JS::HeapFunction<WebIDL::ExceptionOr<JS::Value>(JS::Value)>;

// https://webidl.spec.whatwg.org/#es-promise
using Promise = JS::PromiseCapability;

JS::NonnullGCPtr<Promise> create_promise(JS::Realm&);
JS::NonnullGCPtr<Promise> create_resolved_promise(JS::Realm&, JS::Value);
JS::NonnullGCPtr<Promise> create_rejected_promise(JS::Realm&, JS::Value);
void resolve_promise(JS::Realm&, Promise const&, JS::Value = JS::js_undefined());
void reject_promise(JS::Realm&, Promise const&, JS::Value);
JS::NonnullGCPtr<Promise> react_to_promise(Promise const&, JS::GCPtr<ReactionSteps> on_fulfilled_callback, JS::GCPtr<ReactionSteps> on_rejected_callback);
JS::NonnullGCPtr<Promise> upon_fulfillment(Promise const&, JS::NonnullGCPtr<ReactionSteps>);
JS::NonnullGCPtr<Promise> upon_rejection(Promise const&, JS::NonnullGCPtr<ReactionSteps>);
void mark_promise_as_handled(Promise const&);
void wait_for_all(JS::Realm&, Vector<JS::NonnullGCPtr<Promise>> const& promises, Function<void(Vector<JS::Value> const&)> success_steps, Function<void(JS::Value)> failure_steps);

// Non-spec, convenience method.
JS::NonnullGCPtr<Promise> create_rejected_promise_from_exception(JS::Realm&, Exception);

}
