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
#include <LibDevTools/Actors/ThreadActor.h>
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
    response.set("hasException"sv, false);
    response.set("helperResult"sv, JsonValue {});
}

static void received_debugger_evaluation_result(JsonObject& response, ThreadActor& thread, String result_id, String input, WebView::DebuggerEvaluationResult result)
{
    response.set("type"sv, "evaluationResult"_string);
    response.set("timestamp"sv, AK::UnixDateTime::now().milliseconds_since_epoch());
    response.set("resultID"sv, move(result_id));
    response.set("input"sv, move(input));
    if (result.is_throw) {
        response.set("result"sv, JsonValue {});
        response.set("exception"sv, thread.serialize_debugger_value(result.value));
        response.set("hasException"sv, true);
    } else {
        response.set("result"sv, thread.serialize_debugger_value(result.value));
        response.set("exception"sv, JsonValue {});
        response.set("hasException"sv, false);
    }
    response.set("exceptionMessage"sv, JsonValue {});
    response.set("helperResult"sv, JsonValue {});
}

NonnullRefPtr<ConsoleActor> ConsoleActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<ThreadActor> thread)
{
    return adopt_ref(*new ConsoleActor(devtools, move(name), move(tab), move(thread)));
}

ConsoleActor::ConsoleActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<ThreadActor> thread)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_thread(move(thread))
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

        auto frame_actor = message.data.get_string("frameActor"sv);
        if (frame_actor.has_value()) {
            auto tab = m_tab.strong_ref();
            auto thread = m_thread.strong_ref();
            auto frame_id = thread ? thread->frame_id_for_actor(*frame_actor) : Optional<u64> {};
            if (!tab || !thread || !frame_id.has_value()) {
                JsonObject evaluation_result;
                evaluation_result.set("type"sv, "evaluationResult"sv);
                evaluation_result.set("timestamp"sv, AK::UnixDateTime::now().milliseconds_since_epoch());
                evaluation_result.set("resultID"sv, move(result_id));
                evaluation_result.set("input"sv, *text);
                evaluation_result.set("result"sv, JsonValue {});
                evaluation_result.set("exception"sv, "Unable to locate paused frame"sv);
                evaluation_result.set("exceptionMessage"sv, "Unable to locate paused frame"sv);
                evaluation_result.set("hasException"sv, true);
                evaluation_result.set("helperResult"sv, JsonValue {});
                send_message(move(evaluation_result));
                return;
            }

            devtools().delegate().evaluate_javascript_in_debugger_frame(tab->description(), *frame_id, *text,
                [weak_self = make_weak_ptr<ConsoleActor>(), weak_thread = thread->make_weak_ptr<ThreadActor>(), result_id = move(result_id), input = *text](auto result) mutable {
                    auto self = weak_self.strong_ref();
                    auto thread = weak_thread.strong_ref();
                    if (!self || !thread)
                        return;
                    JsonObject response;
                    if (result.is_error()) {
                        auto error = result.release_error();
                        response.set("type"sv, "evaluationResult"sv);
                        response.set("timestamp"sv, AK::UnixDateTime::now().milliseconds_since_epoch());
                        response.set("resultID"sv, move(result_id));
                        response.set("input"sv, move(input));
                        response.set("result"sv, JsonValue {});
                        response.set("exception"sv, error);
                        response.set("exceptionMessage"sv, move(error));
                        response.set("hasException"sv, true);
                        response.set("helperResult"sv, JsonValue {});
                    } else {
                        received_debugger_evaluation_result(response, *thread, move(result_id), move(input), result.release_value());
                    }
                    self->send_message(move(response));
                });
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
