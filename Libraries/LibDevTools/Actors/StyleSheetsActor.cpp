/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/StyleSheetsActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<StyleSheetsActor> StyleSheetsActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new StyleSheetsActor(devtools, move(name), move(tab)));
}

StyleSheetsActor::StyleSheetsActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
{
    if (auto tab = m_tab.strong_ref()) {
        devtools.delegate().listen_for_style_sheet_sources(
            tab->description(),
            [weak_self = make_weak_ptr<StyleSheetsActor>()](Web::CSS::StyleSheetIdentifier const& style_sheet, String source) {
                if (auto self = weak_self.strong_ref())
                    self->style_sheet_source_received(style_sheet, move(source));
            });
    }
}

StyleSheetsActor::~StyleSheetsActor()
{
    if (auto tab = m_tab.strong_ref())
        devtools().delegate().stop_listening_for_style_sheet_sources(tab->description());
}

void StyleSheetsActor::handle_message(Message const& message)
{
    if (message.type == "getText"sv) {
        auto resource_id = get_required_parameter<String>(message, "resourceId"sv);
        if (!resource_id.has_value())
            return;

        auto index = resource_id->bytes_as_string_view().find_last_split_view(':').to_number<size_t>();
        if (!index.has_value() || *index >= m_style_sheets.size()) {
            send_unknown_actor_error(message, *resource_id);
            return;
        }

        if (auto tab = m_tab.strong_ref()) {
            devtools().delegate().retrieve_style_sheet_source(tab->description(), m_style_sheets[*index]);
            m_pending_style_sheet_source_requests.set(*index, { .id = message.id });
        }

        return;
    }

    send_unrecognized_packet_type_error(message);
}

void StyleSheetsActor::set_style_sheets(Vector<Web::CSS::StyleSheetIdentifier> style_sheets)
{
    m_style_sheets = move(style_sheets);
}

void StyleSheetsActor::style_sheet_source_received(Web::CSS::StyleSheetIdentifier const& style_sheet, String source)
{
    auto index = m_style_sheets.find_first_index_if([&](auto const& candidate) {
        return candidate.type == style_sheet.type && candidate.url == style_sheet.url;
    });
    if (!index.has_value())
        return;

    auto pending_message = m_pending_style_sheet_source_requests.take(*index);
    if (!pending_message.has_value())
        return;

    // FIXME: Support the `longString` message type so that we don't have to send the entire style sheet
    //        source at once for large sheets.
    JsonObject response;
    response.set("text"sv, move(source));
    send_response(*pending_message, move(response));
}

}
