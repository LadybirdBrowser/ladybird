/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Time.h>
#include <LibDevTools/Actors/ConsoleActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

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

void ConsoleActor::handle_message(StringView type, JsonObject const& message)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "autocomplete"sv) {
        response.set("matches"sv, JsonArray {});
        response.set("matchProp"sv, String {});
        send_message(move(response));
        return;
    }

    if (type == "evaluateJSAsync"sv) {
        auto text = message.get_string("text"sv);
        if (!text.has_value()) {
            send_missing_parameter_error("text"sv);
            return;
        }

        auto result_id = MUST(String::formatted("{}-{}", name(), m_execution_id++));

        response.set("resultID"sv, result_id);
        send_message(move(response));

        // FIXME: We do not support eager evaluation of scripts. Just bail for now.
        if (message.get_bool("eager"sv).value_or(false)) {
            return;
        }

        if (auto tab = m_tab.strong_ref()) {
            auto block_token = block_responses();

            devtools().delegate().evaluate_javascript(tab->description(), *text,
                [result_id, input = *text, weak_self = make_weak_ptr<ConsoleActor>(), block_token = move(block_token)](ErrorOr<JsonValue> result) mutable {
                    if (result.is_error()) {
                        dbgln_if(DEVTOOLS_DEBUG, "Unable to inspect DOM node: {}", result.error());
                        return;
                    }

                    if (auto self = weak_self.strong_ref())
                        self->received_console_result(move(result_id), move(input), result.release_value(), move(block_token));
                });
        }

        return;
    }

    send_unrecognized_packet_type_error(type);
}

void ConsoleActor::received_console_result(String result_id, String input, JsonValue result, BlockToken block_token)
{
    JsonObject message;
    message.set("from"sv, name());
    message.set("type"sv, "evaluationResult"_string);
    message.set("timestamp"sv, AK::UnixDateTime::now().milliseconds_since_epoch());
    message.set("resultID"sv, move(result_id));
    message.set("input"sv, move(input));
    message.set("result"sv, move(result));
    message.set("exception"sv, JsonValue {});
    message.set("exceptionMessage"sv, JsonValue {});
    message.set("helperResult"sv, JsonValue {});

    send_message(move(message), move(block_token));
}

}
