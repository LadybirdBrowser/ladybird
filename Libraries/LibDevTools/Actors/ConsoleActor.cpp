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
#include <LibWebCommon/DOM/NodeType.h>

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

// The client selects the element of an inspectObject event's grip in its Inspector, resolving the grip's
// contentDomReference to a node actor through the walker's getNodeActorFromContentDomReference request.
void ConsoleActor::inspect_element(Web::UniqueNodeID node_id)
{
    auto tab = m_tab.strong_ref();
    if (!tab)
        return;

    JsonObject preview;
    preview.set("kind"sv, "DOMNode"sv);
    preview.set("nodeType"sv, to_underlying(Web::DOM::NodeType::ELEMENT_NODE));

    JsonObject content_dom_reference;
    content_dom_reference.set("browsingContextId"sv, tab->description().id);
    content_dom_reference.set("id"sv, node_id.value());

    JsonObject grip;
    grip.set("type"sv, "object"sv);
    grip.set("preview"sv, move(preview));
    grip.set("contentDomReference"sv, move(content_dom_reference));

    JsonObject message;
    message.set("type"sv, "inspectObject"sv);
    message.set("objectActor"sv, move(grip));
    message.set("inspectFromAnnotation"sv, "browser-context-menu"sv);
    send_message(move(message));
}

void ConsoleActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "autocomplete"sv) {
        auto text = get_required_parameter<String>(message, "text"sv);
        if (!text.has_value())
            return;

        // The client asks for the completions at the cursor, a UTF-16 offset into the text, so only the text before it
        // is completed.
        auto input = text.release_value();
        if (auto cursor = message.data.get_integer<size_t>("cursor"sv); cursor.has_value()) {
            auto utf16_input = Utf16String::from_utf8(input);
            if (*cursor < utf16_input.length_in_code_units())
                input = utf16_input.substring_view(0, *cursor).to_utf8_but_should_be_ported_to_utf16();
        }

        auto tab = m_tab.strong_ref();
        if (!tab) {
            response.set("matches"sv, JsonValue {});
            send_response(message, move(response));
            return;
        }

        devtools().delegate().complete_console_input(tab->description(), input,
            async_handler(message, [](auto&, DevToolsDelegate::ConsoleInputCompletion completion, auto& response) {
                if (completion.matches.has_value()) {
                    JsonArray matches;
                    for (auto& match : *completion.matches)
                        matches.must_append(move(match));
                    response.set("matches"sv, move(matches));
                } else {
                    response.set("matches"sv, JsonValue {});
                }
                response.set("matchProp"sv, move(completion.match_prop));
                response.set("isElementAccess"sv, false);
            }));
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
                async_handler<ConsoleActor>({}, [result_id, input = *text](auto& self, auto result, auto& response) {
                    // The console holds the objects of its results, so they stay expandable after the debugger resumes.
                    auto thread = self.m_thread.strong_ref();
                    auto serialized = thread ? thread->serialize_debugger_value(result, WebView::DebuggerObjectScope::Console) : JsonValue {};
                    received_console_result(response, move(result_id), move(input), move(serialized));
                }));
        }

        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
