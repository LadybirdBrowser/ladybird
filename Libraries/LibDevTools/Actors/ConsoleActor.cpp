/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Time.h>
#include <LibDevTools/Actors/ConsoleActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

static void received_console_result(JsonObject& response, String result_id, String input, JsonValue result)
{
    response.set("type"sv, "evaluationResult"_string);
    response.set("timestamp"sv, AK::UnixDateTime::now().milliseconds_since_epoch());
    response.set("resultID"sv, move(result_id));
    response.set("input"sv, move(input));
    response.set("result"sv, move(result));
    response.set("exception"sv, JsonValue {});
    response.set("exceptionMessage"sv, JsonValue {});
    response.set("helperResult"sv, JsonValue {});
}

NonnullRefPtr<ConsoleActor> ConsoleActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new ConsoleActor(devtools, move(name), move(tab)));
}

ConsoleActor::ConsoleActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
{
}

ConsoleActor::~ConsoleActor() = default;

void ConsoleActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "autocomplete"sv) {
        response.set("matches"sv, JsonArray {});
        response.set("matchProp"sv, String {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "evaluateJSAsync"sv) {
        auto text = get_required_parameter<String>(message, "text"sv);
        if (!text.has_value())
            return;

        auto result_id = MUST(String::formatted("{}-{}", name(), m_execution_id++));

        response.set("resultID"sv, result_id);
        send_response(message, move(response));

        // FIXME: We do not support eager evaluation of scripts. Just bail for now.
        if (message.data.get_bool("eager"sv).value_or(false)) {
            return;
        }

        if (auto tab = m_tab.strong_ref()) {
            devtools().delegate().evaluate_javascript(tab->description(), *text,
                async_handler({}, [result_id, input = *text](auto&, auto result, auto& response) {
                    received_console_result(response, move(result_id), move(input), move(result));
                }));
        }

        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
