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
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebIDL {

using ReactionSteps = GC::Function<WebIDL::ExceptionOr<JS::Value>(JS::Value)>;

// https://webidl.spec.whatwg.org/#es-promise
using Promise = JS::PromiseCapability;

WEB_API GC::Ref<Promise> create_promise(JS::Realm&);
WEB_API GC::Ref<Promise> create_resolved_promise(JS::Realm&, JS::Value);
WEB_API GC::Ref<Promise> create_rejected_promise(JS::Realm&, JS::Value);
WEB_API void resolve_promise(JS::Realm&, Promise const&, JS::Value = JS::js_undefined());
WEB_API void reject_promise(JS::Realm&, Promise const&, JS::Value);
WEB_API GC::Ref<Promise> react_to_promise(Promise const&, GC::Ptr<ReactionSteps> on_fulfilled_callback, GC::Ptr<ReactionSteps> on_rejected_callback);
WEB_API GC::Ref<Promise> upon_fulfillment(Promise const&, GC::Ref<ReactionSteps>);
WEB_API GC::Ref<Promise> upon_rejection(Promise const&, GC::Ref<ReactionSteps>);
WEB_API void mark_promise_as_handled(Promise const&);
WEB_API bool is_promise_fulfilled(Promise const&);
WEB_API void wait_for_all(JS::Realm&, Vector<GC::Ref<Promise>> const& promises, Function<void(Vector<JS::Value> const&)> success_steps, Function<void(JS::Value)> failure_steps);
WEB_API GC::Ref<Promise> get_promise_for_wait_for_all(JS::Realm&, Vector<GC::Ref<Promise>> const& promises);

// Non-spec, convenience method.
WEB_API GC::Ref<Promise> create_rejected_promise_from_exception(JS::Realm&, Exception);

}
