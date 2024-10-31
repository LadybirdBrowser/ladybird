/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Forward.h>

namespace Web::WebDriver {

struct ExecutionResult {
    JS::Promise::State state { JS::Promise::State::Pending };
    JS::Value value {};
};

using OnScriptComplete = JS::HeapFunction<void(ExecutionResult)>;

void execute_script(HTML::BrowsingContext const&, ByteString body, JS::MarkedVector<JS::Value> arguments, Optional<u64> const& timeout_ms, JS::NonnullGCPtr<OnScriptComplete> on_complete);
void execute_async_script(HTML::BrowsingContext const&, ByteString body, JS::MarkedVector<JS::Value> arguments, Optional<u64> const& timeout_ms, JS::NonnullGCPtr<OnScriptComplete> on_complete);

}
