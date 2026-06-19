/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/SourceActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<SourceActor> SourceActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, Web::HTML::ScriptRegistry::Description source)
{
    return adopt_ref(*new SourceActor(devtools, move(name), move(tab), move(source)));
}

SourceActor::SourceActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, Web::HTML::ScriptRegistry::Description source)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_source(move(source))
{
}

SourceActor::~SourceActor() = default;

JsonObject SourceActor::serialize_source() const
{
    auto source_url = m_source.url.has_value() ? m_source.url->serialize() : m_source.display_url;

    JsonObject source;
    source.set("actor"sv, name());
    source.set("extensionName"sv, JsonValue {});
    source.set("url"sv, source_url);
    source.set("isBlackBoxed"sv, false);
    source.set("sourceMapBaseURL"sv, source_url);
    source.set("sourceMapURL"sv, JsonValue {});
    source.set("introductionType"sv, m_source.introduction_type);
    source.set("isInlineSource"sv, m_source.is_inline_source);
    source.set("sourceStartLine"sv, m_source.source_start_line);
    source.set("sourceStartColumn"sv, m_source.source_start_column);
    source.set("sourceLength"sv, static_cast<i64>(m_source.source_length));
    if (auto tab = m_tab.strong_ref()) {
        source.set("browsingContextID"sv, tab->description().id);
        source.set("innerWindowId"sv, tab->inner_window_id());
        source.set("resourceId"sv, MUST(String::formatted("source-{}-{}-{}", tab->inner_window_id(), m_source.id.document_id.value(), m_source.id.script_id)));
    }
    return source;
}

void SourceActor::handle_message(Message const& message)
{
    if (message.type == "source"sv) {
        auto tab = m_tab.strong_ref();
        if (!tab) {
            JsonObject response;
            response.set("source"sv, ""sv);
            response.set("contentType"sv, m_source.content_type);
            send_response(message, move(response));
            return;
        }

        devtools().delegate().retrieve_source(tab->description(), m_source.id,
            async_handler<SourceActor>(message, [](auto&, auto source_content, auto& response) {
                // Firefox's longstring protocol accepts a primitive string and wraps it in a SimpleStringFront on the
                // client side.
                response.set("source"sv, move(source_content.text));
                response.set("contentType"sv, move(source_content.content_type));
            }));
        return;
    }

    if (message.type == "getBreakableLines"sv) {
        // FIXME: Compute real breakable lines from the parsed script source.
        JsonObject response;
        JsonArray lines;
        response.set("lines"sv, move(lines));
        send_response(message, move(response));
        return;
    }

    if (message.type == "getBreakpointPositionsCompressed"sv) {
        // FIXME: Compute breakpoint positions once debugger bytecode/source
        //        mapping is exposed to DevTools.
        JsonObject response;
        JsonObject positions;
        response.set("positions"sv, move(positions));
        send_response(message, move(response));
        return;
    }

    if (message.type == "setPausePoints"sv || message.type == "unblackbox"sv) {
        // FIXME: Implement source pause points and blackboxing state.
        JsonObject response;
        send_response(message, move(response));
        return;
    }

    if (message.type == "blackbox"sv) {
        // FIXME: Persist blackboxing state and apply it when debugging.
        JsonObject response;
        response.set("pausedInSource"sv, false);
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
