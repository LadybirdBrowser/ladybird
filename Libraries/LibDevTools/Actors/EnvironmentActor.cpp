/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDevTools/Actors/EnvironmentActor.h>

namespace DevTools {

NonnullRefPtr<EnvironmentActor> EnvironmentActor::create(DevToolsServer& devtools, String name, JsonObject form)
{
    return adopt_ref(*new EnvironmentActor(devtools, move(name), move(form)));
}

EnvironmentActor::EnvironmentActor(DevToolsServer& devtools, String name, JsonObject form)
    : Actor(devtools, move(name))
    , m_form(move(form))
{
    m_form.set("actor"sv, this->name());
}

EnvironmentActor::~EnvironmentActor() = default;

void EnvironmentActor::handle_message(Message const& message)
{
    send_unrecognized_packet_type_error(message);
}

}
