/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/JsonValue.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Forward.h>

namespace Web::WebDriver {

enum class ExecuteScriptResultType {
    PromiseResolved,
    PromiseRejected,
    Timeout,
    JavaScriptError,
    BrowsingContextDiscarded,
    StaleElement,
};

struct ExecuteScriptResult {
    ExecuteScriptResultType type;
    JS::Value value;
};

struct ExecuteScriptResultSerialized {
    ExecuteScriptResultType type;
    JsonValue value;
};

using OnScriptComplete = JS::HeapFunction<void(ExecuteScriptResultSerialized)>;

JS::ThrowCompletionOr<JS::Value> execute_a_function_body(HTML::BrowsingContext const&, ByteString const& body, ReadonlySpan<JS::Value> parameters);
JS::ThrowCompletionOr<JS::Value> execute_a_function_body(HTML::Window const&, ByteString const& body, ReadonlySpan<JS::Value> parameters, JS::GCPtr<JS::Object> environment_override_object = {});

void execute_script(HTML::BrowsingContext const&, ByteString body, JS::MarkedVector<JS::Value> arguments, Optional<u64> const& timeout_ms, JS::NonnullGCPtr<OnScriptComplete> on_complete);
void execute_async_script(HTML::BrowsingContext const&, ByteString body, JS::MarkedVector<JS::Value> arguments, Optional<u64> const& timeout_ms, JS::NonnullGCPtr<OnScriptComplete> on_complete);

}
