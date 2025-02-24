/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Window.h>
#include <WebContent/ConsoleGlobalEnvironmentExtensions.h>
#include <WebContent/DevToolsConsoleClient.h>
#include <WebContent/PageClient.h>

namespace WebContent {

GC_DEFINE_ALLOCATOR(DevToolsConsoleClient);

GC::Ref<DevToolsConsoleClient> DevToolsConsoleClient::create(JS::Realm& realm, JS::Console& console, PageClient& client)
{
    auto& window = as<Web::HTML::Window>(realm.global_object());
    auto console_global_environment_extensions = realm.create<ConsoleGlobalEnvironmentExtensions>(realm, window);

    return realm.heap().allocate<DevToolsConsoleClient>(realm, console, client, console_global_environment_extensions);
}

DevToolsConsoleClient::DevToolsConsoleClient(JS::Realm& realm, JS::Console& console, PageClient& client, ConsoleGlobalEnvironmentExtensions& console_global_environment_extensions)
    : WebContentConsoleClient(realm, console, client, console_global_environment_extensions)
{
}

DevToolsConsoleClient::~DevToolsConsoleClient() = default;

void DevToolsConsoleClient::handle_result(JS::Value result)
{
    (void)result;
}

void DevToolsConsoleClient::report_exception(JS::Error const& exception, bool in_promise)
{
    (void)exception;
    (void)in_promise;
}

void DevToolsConsoleClient::send_messages(i32 start_index)
{
    (void)start_index;
}

// 2.3. Printer(logLevel, args[, options]), https://console.spec.whatwg.org/#printer
JS::ThrowCompletionOr<JS::Value> DevToolsConsoleClient::printer(JS::Console::LogLevel log_level, PrinterArguments arguments)
{
    (void)log_level;
    (void)arguments;
    return JS::js_undefined();
}

}
