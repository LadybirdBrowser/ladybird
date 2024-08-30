/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/JsonValue.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Forward.h>

namespace Web::WebDriver {

enum class ExecuteScriptResultType {
    PromiseResolved,
    PromiseRejected,
    Timeout,
    JavaScriptError,
    BrowsingContextDiscarded,
};

struct ExecuteScriptResult {
    ExecuteScriptResultType type;
    JS::Value value;
};

struct ExecuteScriptResultSerialized {
    ExecuteScriptResultType type;
    JsonValue value;
};

ExecuteScriptResultSerialized execute_script(Page& page, ByteString const& body, JS::MarkedVector<JS::Value> arguments, Optional<u64> const& timeout_ms);
ExecuteScriptResultSerialized execute_async_script(Page& page, ByteString const& body, JS::MarkedVector<JS::Value> arguments, Optional<u64> const& timeout_ms);

}
