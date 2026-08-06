/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibDevTools/Actors/SymbolIteratorActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<SymbolIteratorActor> SymbolIteratorActor::create(DevToolsServer& devtools, String name, WeakPtr<ThreadActor> thread)
{
    return adopt_ref(*new SymbolIteratorActor(devtools, move(name), move(thread)));
}

SymbolIteratorActor::SymbolIteratorActor(DevToolsServer& devtools, String name, WeakPtr<ThreadActor> thread)
    : Actor(devtools, move(name))
    , m_thread(move(thread))
{
}

SymbolIteratorActor::~SymbolIteratorActor() = default;

JsonObject SymbolIteratorActor::form() const
{
    JsonObject form;
    form.set("type"sv, "symbolIterator"sv);
    form.set("actor"sv, name());
    form.set("count"sv, 0);
    return form;
}

void SymbolIteratorActor::handle_message(Message const& message)
{
    if (message.type == "slice"sv || message.type == "all"sv) {
        JsonObject response;
        // FIXME: Report the object's actual symbol-keyed properties.
        response.set("ownSymbols"sv, JsonArray {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "release"sv) {
        send_response(message, {});
        if (auto thread = m_thread.strong_ref()) {
            thread->release_pause_actor(*this);
        } else {
            devtools().unregister_actor(name());
        }
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
