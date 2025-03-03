/*
 * Copyright (c) 2021, Brandon Scott <xeon.productions@gmail.com>
 * Copyright (c) 2020, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Gasim Gasimzada <gasim@gasimzada.net>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <WebContent/ConsoleGlobalEnvironmentExtensions.h>
#include <WebContent/PageClient.h>
#include <WebContent/WebContentConsoleClient.h>

namespace WebContent {

GC_DEFINE_ALLOCATOR(WebContentConsoleClient);

WebContentConsoleClient::WebContentConsoleClient(JS::Realm& realm, JS::Console& console, PageClient& client, ConsoleGlobalEnvironmentExtensions& console_global_environment_extensions)
    : ConsoleClient(console)
    , m_realm(realm)
    , m_client(client)
    , m_console_global_environment_extensions(console_global_environment_extensions)
{
}

WebContentConsoleClient::~WebContentConsoleClient() = default;

void WebContentConsoleClient::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_client);
    visitor.visit(m_console_global_environment_extensions);
}

void WebContentConsoleClient::handle_input(StringView js_source)
{
    auto& settings = Web::HTML::relevant_settings_object(*m_console_global_environment_extensions);
    auto script = Web::HTML::ClassicScript::create("(console)", js_source, settings.realm(), settings.api_base_url());

    auto with_scope = JS::new_object_environment(*m_console_global_environment_extensions, true, &settings.realm().global_environment());

    // FIXME: Add parse error printouts back once ClassicScript can report parse errors.
    auto result = script->run(Web::HTML::ClassicScript::RethrowErrors::No, with_scope);

    if (result.value().has_value()) {
        m_console_global_environment_extensions->set_most_recent_result(*result.value());
        handle_result(*result.value());
    }
}

}
