/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/ThreadConfigurationActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<ThreadConfigurationActor> ThreadConfigurationActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new ThreadConfigurationActor(devtools, move(name), move(tab)));
}

ThreadConfigurationActor::ThreadConfigurationActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
{
}

ThreadConfigurationActor::~ThreadConfigurationActor() = default;

void ThreadConfigurationActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "updateConfiguration"sv) {
        auto configuration = get_required_parameter<JsonObject>(message, "configuration"sv);
        if (!configuration.has_value())
            return;

        if (auto value = configuration->get_bool("ignoreCaughtExceptions"sv); value.has_value())
            m_configuration.ignore_caught_exceptions = *value;
        if (auto value = configuration->get_bool("pauseOnExceptions"sv); value.has_value())
            m_configuration.pause_on_exceptions = *value;
        if (auto value = configuration->get_bool("shouldPauseOnDebuggerStatement"sv); value.has_value())
            m_configuration.should_pause_on_debugger_statement = *value;
        if (auto value = configuration->get_bool("skipBreakpoints"sv); value.has_value())
            m_configuration.skip_breakpoints = *value;

        reapply_configuration();

        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

JsonObject ThreadConfigurationActor::serialize_configuration() const
{
    JsonObject target;
    target.set("actor"sv, name());

    return target;
}

void ThreadConfigurationActor::reapply_configuration() const
{
    if (auto tab = m_tab.strong_ref())
        devtools().delegate().configure_debugger(tab->description(), m_configuration);
}

}
