/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/String.h>
#include <LibGC/Function.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::WebDriver {

struct ExecutionResult {
    JS::Promise::State state { JS::Promise::State::Pending };
    JS::Value value {};
};

using OnScriptComplete = GC::Function<void(ExecutionResult)>;

WEB_API void execute_script(HTML::BrowsingContext const&, String body, GC::RootVector<JS::Value> arguments, Optional<u64> const& timeout_ms, GC::Ref<OnScriptComplete> on_complete);
WEB_API void execute_async_script(HTML::BrowsingContext const&, String body, GC::RootVector<JS::Value> arguments, Optional<u64> const& timeout_ms, GC::Ref<OnScriptComplete> on_complete);

}
